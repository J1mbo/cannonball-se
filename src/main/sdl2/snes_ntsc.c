/* Based on snes_ntsc 0.2.2. http://www.slack.net/~ant/

Updates for CannonBall-SE are Copyright (c) 2025 James Pearce:
- S16 emulation at 24-bit output depth
- threaded initialisation
- SSE/NEON SIMD blitters
Updates Aug-25:  removed AVX dependency (now works with only SSE4.1)
                 removed ARM AArch64 dependency (now works with ARMv7 NEON e.g. Pi2 v1.1)
Updates Sep-25:  added scalar fall-back for Pi1 and PiZero
*/
#include <stdio.h>

#include "snes_ntsc.h"
#ifdef _OPENMP
#include <omp.h>    // JJP - OpenMP support for multi-threading initialization
#endif
#include <stdint.h> // uint32_t etc


/* Copyright (C) 2006-2007 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

snes_ntsc_setup_t const snes_ntsc_monochrome = { 0,-1, 0, 0,.2,  0,.2,-.2,-.2,-1,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_composite = { 0, 0, 0, 0, 0,  0, 0,  0,  0, 0,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_svideo = { 0, 0, 0, 0,.2,  0,.2, -1, -1, 0,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_rgb = { 0, 0, 0, 0,.2,  0,.7, -1, -1,-1,  1, 0, 0 };

#define alignment_count 3
#define burst_count     3
#define rescale_in      8
#define rescale_out     7

#define artifacts_mid   1.0f
#define fringing_mid    1.0f
#define std_decoder_hue 0

#define rgb_bits        7   /* half normal range to allow for doubled hires pixels */
#define gamma_size      256 // JJP - 8-bit range of input values for S16 as adjusted for DACs

#include "snes_ntsc_impl.h"

/* 3 input pixels -> 8 composite samples */
pixel_info_t const snes_ntsc_pixels[alignment_count] = {
    { PIXEL_OFFSET(-4, -9), { 1, 1, .6667f, 0 } },
    { PIXEL_OFFSET(-2, -7), {       .3333f, 1, 1, .3333f } },
    { PIXEL_OFFSET(0, -5), {                  0, .6667f, 1, 1 } },
};

const unsigned int S16_rgbVals[64] = {
    // standard levels
    0,8,16,24,31,39,47,55,62,70,78,86,94,102,109,117,125,133,140,148,156,164,171,179,187,195,203,211,
    218,226,234,242,
    // shadow levels
    0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120,126,130,136,140,146,
    150,156
};

static void merge_kernel_fields(snes_ntsc_rgb_t* io)
{
    int n;
    for (n = burst_size; n; --n)
    {
        snes_ntsc_rgb_t p0 = io[burst_size * 0] + rgb_bias;
        snes_ntsc_rgb_t p1 = io[burst_size * 1] + rgb_bias;
        snes_ntsc_rgb_t p2 = io[burst_size * 2] + rgb_bias;
        /* merge colors without losing precision */
        io[burst_size * 0] =
            ((p0 + p1 - ((p0 ^ p1) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        io[burst_size * 1] =
            ((p1 + p2 - ((p1 ^ p2) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        io[burst_size * 2] =
            ((p2 + p0 - ((p2 ^ p0) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        ++io;
    }
}

static void correct_errors(snes_ntsc_rgb_t color, snes_ntsc_rgb_t* out)
{
    int n;
    for (n = burst_count; n; --n)
    {
        unsigned i;
        for (i = 0; i < rgb_kernel_size / 2; i++)
        {
            snes_ntsc_rgb_t error = color -
                out[i] - out[(i + 12) % 14 + 14] - out[(i + 10) % 14 + 28] -
                out[i + 7] - out[i + 5 + 14] - out[i + 3 + 28];
            DISTRIBUTE_ERROR(i + 3 + 28, i + 5 + 14, i + 7);
        }
        out += alignment_count * rgb_kernel_size;
    }
}


void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup)
{
    int merge_fields;
    init_t impl;
    if (!setup)
        setup = &snes_ntsc_composite;
    init(&impl, setup);

    merge_fields = setup->merge_fields;
    if (setup->artifacts <= -1 && setup->fringing <= -1)
        merge_fields = 1;

    #ifdef _OPENMP
    #pragma omp parallel
    {
        int thread_count = omp_get_num_threads();
        for (int n = 0; n < thread_count; n++) {
            unsigned int block_size = snes_ntsc_palette_size / omp_get_num_threads(); // amount processed by each thread
            unsigned int start = n * block_size;
            unsigned int end = (n == omp_get_num_threads() - 1) ? snes_ntsc_palette_size : (n + 1) * block_size;
            unsigned int entry;
            for (entry = start; entry < end; entry++) {
                unsigned int red_index = (entry >> 10 & 0x1F) + ((entry >> 15) * 32);
                unsigned int green_index = (entry >> 5 & 0x1F) + ((entry >> 15) * 32);
                unsigned int blue_index = (entry >> 0 & 0x1F) + ((entry >> 15) * 32);
                unsigned int ir = S16_rgbVals[red_index];
                unsigned int ig = S16_rgbVals[green_index];
                unsigned int ib = S16_rgbVals[blue_index];
                float rr = impl.to_float[ir];
                float gg = impl.to_float[ig];
                float bb = impl.to_float[ib];
                float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
                int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
                snes_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
                snes_ntsc_rgb_t* out = ntsc->table[entry];
                gen_kernel(&impl, y, i, q, out);
                if (merge_fields) merge_kernel_fields(out);
                correct_errors(rgb, out);
            }
        }
    }
    #else
        for (unsigned int entry = 0; entry < snes_ntsc_palette_size; entry++) {
            unsigned int red_index   = (entry >> 10 & 0x1F) + ((entry >> 15) * 32);
            unsigned int green_index = (entry >> 5  & 0x1F) + ((entry >> 15) * 32);
            unsigned int blue_index  = (entry >> 0  & 0x1F) + ((entry >> 15) * 32);
            unsigned int ir = S16_rgbVals[red_index];
            unsigned int ig = S16_rgbVals[green_index];
            unsigned int ib = S16_rgbVals[blue_index];
            float rr = impl.to_float[ir];
            float gg = impl.to_float[ig];
            float bb = impl.to_float[ib];
            float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
            int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
            snes_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
            snes_ntsc_rgb_t* out = ntsc->table[entry];
            gen_kernel(&impl, y, i, q, out);
            if (merge_fields) merge_kernel_fields(out);
            correct_errors(rgb, out);
        }
    #endif
}

#ifndef SNES_NTSC_NO_BLITTERS

void snes_ntsc_blit(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel)
{
    int chunk_count = (in_width - 1) / snes_ntsc_in_chunk;
    for (; in_height; --in_height)
    {
        SNES_NTSC_IN_T const* line_in = input;
        SNES_NTSC_BEGIN_ROW(ntsc, burst_phase,
            snes_ntsc_black, snes_ntsc_black, SNES_NTSC_ADJ_IN(*line_in));
        snes_ntsc_out_t* restrict line_out = (snes_ntsc_out_t*)rgb_out;
        int n;
        ++line_in;

        for (n = chunk_count; n; --n)
        {
            /* order of input and output pixels must not be altered */
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(line_in[0]));
            SNES_NTSC_RGB_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(line_in[1]));
            SNES_NTSC_RGB_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(line_in[2]));
            SNES_NTSC_RGB_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

            line_in += 3;
            line_out += 7;
        }

        /* finish final pixels */
        SNES_NTSC_COLOR_IN(0, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}




void snes_ntsc_blit_hires(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel)
{
    int chunk_count = (in_width - 2) / (snes_ntsc_in_chunk * 2);
    for (; in_height; --in_height)
    {
        SNES_NTSC_IN_T const* line_in = input;
        SNES_NTSC_HIRES_ROW(ntsc, burst_phase,
            snes_ntsc_black, snes_ntsc_black, snes_ntsc_black,
            SNES_NTSC_ADJ_IN(line_in[0]),
            SNES_NTSC_ADJ_IN(line_in[1]));
        snes_ntsc_out_t* restrict line_out = (snes_ntsc_out_t*)rgb_out;
        int n;
        line_in += 2;

        for (n = chunk_count; n; --n)
        {
            // twice as many input pixels per chunk, compared to lo-res version
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(line_in[0]));
            SNES_NTSC_HIRES_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(line_in[1]));
            SNES_NTSC_HIRES_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(line_in[2]));
            SNES_NTSC_HIRES_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(line_in[3]));
            SNES_NTSC_HIRES_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(line_in[4]));
            SNES_NTSC_HIRES_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(line_in[5]));
            SNES_NTSC_HIRES_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_HIRES_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

            line_in += 6;
            line_out += 7;
        }

        SNES_NTSC_COLOR_IN(0, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(3, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(4, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(5, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_HIRES_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}

/* JJP - SIMD macro predefined mask vectors for performance */
#if SNES_NTSC_HAVE_SIMD
#  if defined(__aarch64__) || defined(_M_ARM64)
    #define CHECK_ALL_EQUAL CHECK_ALL_EQUAL_AARCH64
#  elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define CHECK_ALL_EQUAL CHECK_ALL_EQUAL_NEON
#  elif defined(_M_X64) || defined(__x86_64__) || defined(__i386__)
    #define CHECK_ALL_EQUAL CHECK_ALL_EQUAL_SSE4
#  endif
#endif


// Macro to check equality of 24 input pixels and store the result in
// variable this_colour. The input pixels are stored in:
// xmm0 lanes 2 and 3; xmm1 to xmm5; and xmm6 lanes 0 and 1.
// Note - assumes we're working with 4x uint16_t in each register (even if those
// registers are wider, e.g. 128-bit on SSE)

// This version is for Intel/AMD chips

#define CHECK_ALL_EQUAL_SSE4 do {                                                   \
    /* Gather xmm0 lanes 2/3 and xmm6 lanes 0/1, as we're running 2-pixels out */   \
    SIMD_INPUT_REGISTER_t first_line = ROTATE_OUT(xmm0, xmm6, 2);                   \
    /* Extract first 16-bit value and broadcast */                                  \
    uint16_t first_val = (uint16_t)_mm_extract_epi16(first_line, 0);                \
    SIMD_INPUT_REGISTER_t ref = _mm_set1_epi16(first_val);                          \
    /* Compare each SIMD register to the reference and aggregate (AND) */           \
    SIMD_INPUT_REGISTER_t cmp = _mm_cmpeq_epi16(first_line, ref);                   \
    cmp = _mm_and_si128(cmp, _mm_cmpeq_epi16(xmm1, ref));                           \
    cmp = _mm_and_si128(cmp, _mm_cmpeq_epi16(xmm2, ref));                           \
    cmp = _mm_and_si128(cmp, _mm_cmpeq_epi16(xmm3, ref));                           \
    cmp = _mm_and_si128(cmp, _mm_cmpeq_epi16(xmm4, ref));                           \
    cmp = _mm_and_si128(cmp, _mm_cmpeq_epi16(xmm5, ref));                           \
    /* Only the low 64 bits (4x u16 -> 8 bytes) matter: check their 8 MSBs */       \
    int mm = _mm_movemask_epi8(cmp);                                                \
    uint16_t mask = (uint16_t)-((mm & 0x00FF) == 0x00FF);                           \
    /* set this_colour = first_val if all equal (in low half), else 0 */            \
    this_colour = first_val & mask;                                                 \
} while (0)


// This version is for ARM 64-bit AArch64
#define CHECK_ALL_EQUAL_AARCH64 do {                                                \
    /* Gather xmm0 lanes 2/3 and xmm6 lanes 0/1, as we're running 2-pixels out */   \
    SIMD_INPUT_REGISTER_t first_line = ROTATE_OUT(xmm0, xmm6, 2);                   \
    /* Extract first value and broadcast to another register */                     \
    uint16_t first_val = vget_lane_u16(first_line, 0);                              \
    SIMD_INPUT_REGISTER_t ref = vdup_n_u16(first_val);                              \
    /* Compare each SIMD register to the reference and aggregate */                 \
    SIMD_INPUT_REGISTER_t cmp = vceq_u16(first_line, ref);                          \
    cmp = vand_u16(cmp, vceq_u16(xmm1, ref));                                       \
    cmp = vand_u16(cmp, vceq_u16(xmm2, ref));                                       \
    cmp = vand_u16(cmp, vceq_u16(xmm3, ref));                                       \
    cmp = vand_u16(cmp, vceq_u16(xmm4, ref));                                       \
    cmp = vand_u16(cmp, vceq_u16(xmm5, ref));                                       \
    /* Check if all comparisons are true (i.e., all lanes are 0xFFFF) */            \
    uint16_t min_c = vminv_u16(cmp);                                                \
    uint16_t mask = -(min_c == 0xFFFF);                                             \
    /* set this_colour = first_val if all equal, else 0 */                          \
    this_colour = first_val & mask;                                                 \
} while(0)

/* --- ARMv7 NEON helpers (safe on Cortex-A7 / Pi 2) --- */
#define CHECK_ALL_EQUAL_NEON do {                                                   \
  /* Gather xmm0 lanes 2/3 and xmm6 lanes 0/1 (now 16-bit lanes) */                 \
  SIMD_INPUT_REGISTER_t first_line = ROTATE_OUT(xmm0, xmm6, 2);                     \
  /* Use lane 0 as the reference value */                                           \
  uint16_t first_val = vget_lane_u16(first_line, 0);                                \
  SIMD_INPUT_REGISTER_t ref = vdup_n_u16(first_val);                                \
  /* Compare each vector to the reference (0xFFFF = equal, 0 = not equal) */        \
  SIMD_INPUT_REGISTER_t cmp = vceq_u16(first_line, ref);                            \
  cmp = vand_u16(cmp, vceq_u16(xmm1, ref));                                         \
  cmp = vand_u16(cmp, vceq_u16(xmm2, ref));                                         \
  cmp = vand_u16(cmp, vceq_u16(xmm3, ref));                                         \
  cmp = vand_u16(cmp, vceq_u16(xmm4, ref));                                         \
  cmp = vand_u16(cmp, vceq_u16(xmm5, ref));                                         \
  /* Pairwise-min reduction (min == AND for {0, 0xFFFF} masks) */                   \
  SIMD_INPUT_REGISTER_t r = vpmin_u16(cmp, cmp);                                    \
  r = vpmin_u16(r, r);                                                              \
  uint16_t lanes_min = vget_lane_u16(r, 0);                                         \
  /* Set this_colour = first_val iff ALL lanes matched; else 0 */                   \
  uint16_t mask = (uint16_t)-(lanes_min == 0xFFFFu);                                \
  this_colour = first_val & mask;                                                   \
} while (0)


#if SNES_NTSC_HAVE_SIMD
void snes_ntsc_blit_hires_fast(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* restrict input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* restrict rgb_out, long out_pitch, long Alevel)
    // This version utilises SIMD registers streamline:
    // 1. Loading of pixels, as six 128-bit loads (providing 24 pixels in 6 registers)
    // 2. Clamp and RGB conversion, as the SIMD registers can be used to perform the calculations
    // 3. Output to memory, as the SIMD registers can be used to store the output values as seven 128-bit stores
    // The Blargg algorithm itself is implemented in full, except for a small change in end-of-line processing
    // to keep output line width divisable by 4 and hence 128-bit aligned.
{
    int chunk_count = in_width / (snes_ntsc_in_chunk * 2); // should be 640 / (3 * 2) = 106

    // Prepare SIMD vectors
    SET_SNES_MASK_VECTORS;
    //for (int y = 0; y<in_height; y++)
    for (; in_height; --in_height)
    {
        int x = 0;
        // SIMD Input Registers (uint16_t x4)
        SIMD_INPUT_REGISTER_t xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6; // inputs

        // Outputs - xmm10..16 (uint32_t x4)
        SIMD_REGISTER_t xmm10 = SIMD_ZERO;
        SIMD_REGISTER_t xmm11 = SIMD_ZERO;
        SIMD_REGISTER_t xmm12 = SIMD_ZERO;
        SIMD_REGISTER_t xmm13 = SIMD_ZERO;
        SIMD_REGISTER_t xmm14 = SIMD_ZERO;
        SIMD_REGISTER_t xmm15 = SIMD_ZERO;
        SIMD_REGISTER_t xmm16 = SIMD_ZERO;

        // setup the first block, which will affect the second block.

        // load the first four pixels from memory
        SNES_NTSC_IN_T const* restrict line_in = input;
        xmm0 = LOAD_SIMD_REGISTER(&line_in[0]);

        // and use the first two pixels for this initial block
        SNES_NTSC_HIRES_ROW(ntsc, burst_phase,
            snes_ntsc_black, snes_ntsc_black, snes_ntsc_black,
            SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 0)),
            SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 1)));
        snes_ntsc_out_t* restrict line_out = (snes_ntsc_out_t*)rgb_out;

        // set up SIMD registers and flags
        int iterations = 0;
        uint16_t last_colour = 0;
        uint16_t this_colour = 0;

        for (int n = (chunk_count>>2); n; --n)
        {
            // Load the next 6x4 = 24 pixels
            xmm1 = LOAD_SIMD_REGISTER(&line_in[4]);
            xmm2 = LOAD_SIMD_REGISTER(&line_in[8]);
            xmm3 = LOAD_SIMD_REGISTER(&line_in[12]);
            xmm4 = LOAD_SIMD_REGISTER(&line_in[16]);
            xmm5 = LOAD_SIMD_REGISTER(&line_in[20]);
            xmm6 = LOAD_SIMD_REGISTER(&line_in[24]); // need 0/1 for this pass; 2/3 for next
            line_in += 24;

            // 1st six. Loop is unrolled to four iterations permitting full use
            // of 4-wide SIMD registers

            CHECK_ALL_EQUAL; // stores result in this_colour, 0 if not equal or the colour if equal

            // identify if this block is a repeat. this_colour must be non-zero to identify a repeat block.
            uint32_t repeat_block = (this_colour == last_colour) & (this_colour != 0); // 0 or 1
            last_colour = this_colour;

            // check if we need to go through the calculation process
            if (!repeat_block) {
                // we need to do the full calculation
                // This block processes 6x4(=24) input pixels to produce 7x4(=28) output pixels

                /* process first six input pixels to produce seven outputs */
                SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 2)));
                xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(0), 0);

                SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 3)));
                xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(1), 1);

                SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 0)));
                xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(2), 2);

                SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 1)));
                xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(3), 3);

                SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 2)));
                xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(4), 0);

                SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 3)));
                xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(5), 1);

                xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(6), 2);

                /* process second six input pixels to produce seven outputs */
                SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 0)));
                xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(0), 3);

                SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 1)));
                xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(1), 0);

                SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 2)));
                xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(2), 1);

                SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 3)));
                xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(3), 2);

                SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 0)));
                xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(4), 3);

                SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 1)));
                xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(5), 0);

                xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(6), 1);

                /* process third six input pixels to produce seven outputs */
                SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 2)));
                xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(0), 2);

                SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 3)));
                xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(1), 3);

                SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm4, 0)));
                xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(2), 0);

                SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm4, 1)));
                xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(3), 1);

                SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm4, 2)));
                xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(4), 2);

                SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm4, 3)));
                xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(5), 3);

                xmm15 = INSERT_SIMD_REGISTER_VALUE(xmm15, SNES_NTSC_HIRES_OUT_SIMD(6), 0);

                /* process fourth set of six input pixels to produce seven outputs */
                SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm5, 0)));
                xmm15 = INSERT_SIMD_REGISTER_VALUE(xmm15, SNES_NTSC_HIRES_OUT_SIMD(0), 1);

                SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm5, 1)));
                xmm15 = INSERT_SIMD_REGISTER_VALUE(xmm15, SNES_NTSC_HIRES_OUT_SIMD(1), 2);

                SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm5, 2)));
                xmm15 = INSERT_SIMD_REGISTER_VALUE(xmm15, SNES_NTSC_HIRES_OUT_SIMD(2), 3);

                SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm5, 3)));
                xmm16 = INSERT_SIMD_REGISTER_VALUE(xmm16, SNES_NTSC_HIRES_OUT_SIMD(3), 0);

                SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm6, 0)));
                xmm16 = INSERT_SIMD_REGISTER_VALUE(xmm16, SNES_NTSC_HIRES_OUT_SIMD(4), 1);

                SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm6, 1)));
                xmm16 = INSERT_SIMD_REGISTER_VALUE(xmm16, SNES_NTSC_HIRES_OUT_SIMD(5), 2);

                xmm16 = INSERT_SIMD_REGISTER_VALUE(xmm16, SNES_NTSC_HIRES_OUT_SIMD(6), 3);

                // Outputs are in xmm10..xmm16 - perform clamp
                SNES_NTSC_CLAMP_AND_CONVERT(xmm10);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm11);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm12);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm13);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm14);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm15);
                SNES_NTSC_CLAMP_AND_CONVERT(xmm16);

            }

            #ifndef _WIN32
                __builtin_prefetch(line_in  + 32, 0, 0);   // prefect next input: read, streaming
            #endif
            SNES_NTSC_RGB_STORE(&line_out[0], xmm10);
            SNES_NTSC_RGB_STORE(&line_out[4], xmm11);
            SNES_NTSC_RGB_STORE(&line_out[8], xmm12);
            SNES_NTSC_RGB_STORE(&line_out[12], xmm13);
            SNES_NTSC_RGB_STORE(&line_out[16], xmm14);
            SNES_NTSC_RGB_STORE(&line_out[20], xmm15);
            SNES_NTSC_RGB_STORE(&line_out[24], xmm16);

            // replace xmm0 with next block (we're 2 input pixels out of sync due to the start of line)
            xmm0 = xmm6;

            xmm10 = xmm11 = xmm12 = xmm13 = xmm14 = xmm15 = xmm16; // in case we can use them next loop

            line_out += 28; // advance pointer (4x7)
            x += 28;
        }

        // At this point with have 6x2 + 2 = 14 input pixels left to process, plus we write out
        // the remaining pixels based on black input pixels to allow the end of line.
        {
            // Load the final 2x6 = 12 pixels
            xmm1 = LOAD_SIMD_REGISTER(&line_in[4]);
            xmm2 = LOAD_SIMD_REGISTER(&line_in[8]);
            xmm3 = LOAD_SIMD_REGISTER(&line_in[12]);

            /* process first six input pixels to produce seven outputs */
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 2)));
            xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(0), 0);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm0, 3)));
            xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(1), 1);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 0)));
            xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(2), 2);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 1)));
            xmm10 = INSERT_SIMD_REGISTER_VALUE(xmm10, SNES_NTSC_HIRES_OUT_SIMD(3), 3);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 2)));
            xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(4), 0);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm1, 3)));
            xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(5), 1);

            xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(6), 2);

            /* process last six input pixels for this row to produce seven outputs */
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 0)));
            xmm11 = INSERT_SIMD_REGISTER_VALUE(xmm11, SNES_NTSC_HIRES_OUT_SIMD(0), 3);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 1)));
            xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(1), 0);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 2)));
            xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(2), 1);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm2, 3)));
            xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(3), 2);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 0)));
            xmm12 = INSERT_SIMD_REGISTER_VALUE(xmm12, SNES_NTSC_HIRES_OUT_SIMD(4), 3);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 1)));
            xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(5), 0);

            xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(6), 1);

            /* process third six input pixels to produce seven outputs */
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 2)));
            xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(0), 2);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(EXTRACT_SIMD_REGISTER_VALUE(xmm3, 3)));
            xmm13 = INSERT_SIMD_REGISTER_VALUE(xmm13, SNES_NTSC_HIRES_OUT_SIMD(1), 3);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(snes_ntsc_black));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(2), 0);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(snes_ntsc_black));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(3), 1);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(snes_ntsc_black));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(4), 2);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(snes_ntsc_black));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(5), 3);

            xmm15 = ZERO_SIMD_REGISTER; // 7th output of this block is black, and 3 pixels padding to retain alignment

            // Clamp the outputs
            SNES_NTSC_CLAMP(xmm10);
            SNES_NTSC_CLAMP(xmm11);
            SNES_NTSC_CLAMP(xmm12);
            SNES_NTSC_CLAMP(xmm13);
            SNES_NTSC_CLAMP(xmm14);
            SNES_NTSC_CLAMP(xmm15);

            // adjust to RGBA and store
            SNES_NTSC_RGB_OUT_STORE(&line_out[0], xmm10, alevel_vec);
            SNES_NTSC_RGB_OUT_STORE(&line_out[4], xmm11, alevel_vec);
            SNES_NTSC_RGB_OUT_STORE(&line_out[8], xmm12, alevel_vec);
            SNES_NTSC_RGB_OUT_STORE(&line_out[12], xmm13, alevel_vec);
            SNES_NTSC_RGB_OUT_STORE(&line_out[16], xmm14, alevel_vec);
            SNES_NTSC_RGB_OUT_STORE(&line_out[20], xmm15, alevel_vec);
        }

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}

#endif

/*
* The following table attempts to depicts how the data is gathered and summed, showing
* the third and fourth iterations of the loop.
* 
* The RGB table for the colour at Line_in[0] is loaded on the first row at the first position
* The second row has one value from the previous iteration, and then loads Line_in[1] at the second
* The third row likewise, but from the second phase offset so table 2.14 rather than 2.0
* Likewise rows 5 & 6 third phase so 2.28.
* 
* The lower table stores the 'afterglow' impact of the first, so values 2.07-2.13 are processed
* In the following pass.
* 
* Values are then summed vertically
*

Phase       Line_in Pixel index within each loop (chunk_count)          Pixel (next iteration)
Offset		[0] 	[1]     [2]     [3]     [4]     [5]     [6] 		[0] 	[1]     [2]     [3]     [4]     [5]     [6]
0.00		2.00	2.01	2.02	2.03	2.04	2.05	2.06		3.00	3.01	3.02	3.03	3.04	3.05	3.06
0.00		1.06	2.00	2.01	2.02	2.03	2.04	2.05		2.06	3.00	3.01	3.02	3.03	3.04	3.05
0.14		1.19	1.20	2.14	2.15	2.16	2.17	2.18		2.19	2.20	3.14	3.15	3.16	3.17	3.18
0.14		1.18	1.19	1.20	2.14	2.15	2.16	2.17		2.18	2.19	2.20	3.14	3.15	3.16	3.17
0.28		1.31	1.32	1.33	1.34	2.28	2.29	2.30		2.31	2.32	2.33	2.34	3.28	3.29	3.30
0.28		1.30	1.31	1.32	1.33	1.34	2.28	2.29		2.30	2.31	2.32	2.33	2.34	3.28	3.29

0.00		1.07	1.08	1.09	1.10	1.11	1.12	1.13		2.07	2.08	2.09	2.10	2.11	2.12	2.13
0.00		0.13	1.07	1.08	1.09	1.10	1.11	1.12		1.13	2.07	2.08	2.09	2.10	2.11	2.12
0.14		0.26	0.27	1.21	1.22	1.23	1.24	1.25		1.26	1.27	2.21	2.22	2.23	2.24	2.25
0.14		0.25	0.26	0.27	1.21	1.22	1.23	1.24		1.25	1.26	1.27	2.21	2.22	2.23	2.24
0.28		0.38	0.39	0.40	0.41	1.35	1.36	1.37		1.38	1.39	1.40	1.41	2.35	2.36	2.37
0.28		0.37	0.38	0.39	0.40	0.41	1.35	1.36		1.37	1.38	1.39	1.40	1.41	2.35	2.36

*/

#endif
