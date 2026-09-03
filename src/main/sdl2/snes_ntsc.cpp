/* Based on snes_ntsc 0.2.2. http://www.slack.net/~ant/

Updates for CannonBall-SE are Copyright (c) 2025 James Pearce:
- S16 emulation at 24-bit output depth
- threaded initialisation
- SSE/NEON SIMD blitters
Updates Aug-25:  removed AVX dependency (now works with only SSE4.1)
                 removed ARM AArch64 dependency (now works with ARMv7 NEON e.g. Pi2 v1.1)
Updates Sep-25:  added scalar fall-back for Pi1 and PiZero
Updates Feb-26:  added SSE2 fall-back; SSE4.1 now selected at compile time via __SSE4_1__
                 so no explicit -msse4.1 flag is required
                 RISC-V RVV 1.0 SIMD path (WITH_RVV=ON / -march=rv64gcv)
*/
#include <stdio.h>

#include "snes_ntsc.h"
#ifdef _OPENMP
#include <omp.h>    // JJP - OpenMP support for multi-threading initialization
#endif
#include <stdint.h> // uint32_t etc
#include "snes_ntsc_simd.h" // portable v4u32 SIMD layer for snes_ntsc_blit_hires_vec (Stage 2)


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


// ---------------------------------------------------------------------------
// Slot-indexed, phase-major kernel table (Part A of the NTSC re-architecture,
// see tools/ntsc_bench/analysis/ARCHITECTURE.md section 4).
//
// Opaque copy of the generator state that snes_ntsc_update_slot(s)() needs to
// regenerate an individual slot later, without the caller having to keep its
// own snes_ntsc_setup_t around. Stored inside snes_ntsc_t::gen_state, whose
// size is fixed by the public header, so its fit is checked here.
struct snes_ntsc_gen_state_t
{
    init_t impl;
    int merge_fields;
};
static_assert(sizeof(snes_ntsc_gen_state_t) <= sizeof(snes_ntsc_t::gen_state),
    "snes_ntsc_gen_state_t must fit inside snes_ntsc_t::gen_state");

// Generates one slot's kernel (all three burst phases) from a packed S16
// colour value, running exactly the float/YIQ/gen_kernel/merge/correct_errors
// sequence the old whole-table snes_ntsc_init() ran per colour entry, into a
// temporary flat buffer shaped like the old table row, then scatters that
// into the slot-indexed, phase-major, block-padded layout of ntsc->plane
// (see SNES_NTSC_KW in snes_ntsc.h for the inverse of this scatter, used by
// the blitters to read it back).
static void gen_slot(snes_ntsc_t* ntsc, int slot, unsigned short colour,
    init_t* impl, int merge_fields)
{
    unsigned int red_index   = (colour >> 10 & 0x1F) + ((colour >> 15) * 32);
    unsigned int green_index = (colour >> 5  & 0x1F) + ((colour >> 15) * 32);
    unsigned int blue_index  = (colour >> 0  & 0x1F) + ((colour >> 15) * 32);

    unsigned int ir = S16_rgbVals[red_index];
    unsigned int ig = S16_rgbVals[green_index];
    unsigned int ib = S16_rgbVals[blue_index];

    float rr = impl->to_float[ir];
    float gg = impl->to_float[ig];
    float bb = impl->to_float[ib];

    float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
    int r, g, b = YIQ_TO_RGB(y, i, q, impl->to_rgb, int, r, g);
    snes_ntsc_rgb_t rgb = PACK_RGB(r, g, b);

    snes_ntsc_rgb_t tmp[snes_ntsc_entry_size]; // classic flat layout: 3 x 42 words
    gen_kernel(impl, y, i, q, tmp);
    if (merge_fields) merge_kernel_fields(tmp);
    correct_errors(rgb, tmp);

    // Scatter tmp[burst_size*b + rgb_kernel_size*a + n] (b<3 bursts, a<3
    // alignments, n<14 words) into plane[b][slot][block_words*a + block_lead + n],
    // zeroing the leading and trailing pad words of every 20-word alignment block
    // (snes_ntsc_block_lead = 3 leading, block_words - block_lead - 14 = 3 trailing).
    for (int bidx = 0; bidx < snes_ntsc_burst_count; ++bidx) {
        snes_ntsc_rgb_t const* src_burst = tmp + burst_size * bidx;
        snes_ntsc_rgb_t* dst_slot = ntsc->plane[bidx][slot];
        for (int a = 0; a < alignment_count; ++a) {
            snes_ntsc_rgb_t const* src = src_burst + rgb_kernel_size * a;
            snes_ntsc_rgb_t* dst = dst_slot + snes_ntsc_block_words * a;
            for (int n = 0; n < (int)snes_ntsc_block_lead; ++n)
                dst[n] = 0;
            for (int n = 0; n < (int)rgb_kernel_size; ++n)
                dst[(int)snes_ntsc_block_lead + n] = src[n];
            for (int n = (int)snes_ntsc_block_lead + (int)rgb_kernel_size; n < (int)snes_ntsc_block_words; ++n)
                dst[n] = 0;
        }
    }
}

void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup,
    unsigned short const* slot_colours)
{
    int merge_fields;
    init_t impl;
    if (!setup)
        setup = &snes_ntsc_composite;
    init(&impl, setup);

    merge_fields = setup->merge_fields;
    if (setup->artifacts <= -1 && setup->fringing <= -1)
        merge_fields = 1;

    // Persist the generator state so snes_ntsc_update_slot(s)() can later
    // regenerate individual slots without needing the setup again.
    {
        snes_ntsc_gen_state_t* gs = reinterpret_cast<snes_ntsc_gen_state_t*>(ntsc->gen_state);
        gs->impl = impl;
        gs->merge_fields = merge_fields;
    }

#ifdef _OPENMP
#pragma omp parallel
    {
        init_t impl_local = impl; // or use firstprivate(impl)
        int merge_fields_local = merge_fields;
        int slot;

#pragma omp for schedule(static)
        for (slot = 0; slot < snes_ntsc_slot_total; ++slot) {
            // Slots 0..snes_ntsc_slot_count-1 are live palette entries (standard +
            // shadow); the black slot and the spare slot beyond it are always black.
            unsigned short colour = (slot < snes_ntsc_slot_count)
                ? (slot_colours ? slot_colours[slot] : (unsigned short)0)
                : (unsigned short)0;
            gen_slot(ntsc, slot, colour, &impl_local, merge_fields_local);
        }
    }
    #else
        for (int slot = 0; slot < snes_ntsc_slot_total; slot++) {
            unsigned short colour = (slot < snes_ntsc_slot_count)
                ? (slot_colours ? slot_colours[slot] : (unsigned short)0)
                : (unsigned short)0;
            gen_slot(ntsc, slot, colour, &impl, merge_fields);
        }
    #endif
}

void snes_ntsc_update_slot(snes_ntsc_t* ntsc, int slot, unsigned short colour)
{
    snes_ntsc_gen_state_t* gs = reinterpret_cast<snes_ntsc_gen_state_t*>(ntsc->gen_state);
    gen_slot(ntsc, slot, colour, &gs->impl, gs->merge_fields);
}

void snes_ntsc_update_slots(snes_ntsc_t* ntsc, int const* slots, int count,
    unsigned short const* slot_colours)
{
    snes_ntsc_gen_state_t* gs = reinterpret_cast<snes_ntsc_gen_state_t*>(ntsc->gen_state);
    init_t* impl = &gs->impl;
    int merge_fields = gs->merge_fields;

#ifdef _OPENMP
    if (count > 64) {
        int i;
#pragma omp parallel for schedule(static)
        for (i = 0; i < count; ++i)
            gen_slot(ntsc, slots[i], slot_colours[i], impl, merge_fields);
        return;
    }
#endif
    for (int i = 0; i < count; ++i)
        gen_slot(ntsc, slots[i], slot_colours[i], impl, merge_fields);
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
            snes_ntsc_black_slot, snes_ntsc_black_slot, SNES_NTSC_ADJ_IN(*line_in));
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
        SNES_NTSC_COLOR_IN(0, snes_ntsc_black_slot);
        SNES_NTSC_RGB_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black_slot);
        SNES_NTSC_RGB_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black_slot);
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
            snes_ntsc_black_slot, snes_ntsc_black_slot, snes_ntsc_black_slot,
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

        SNES_NTSC_COLOR_IN(0, snes_ntsc_black_slot);
        SNES_NTSC_HIRES_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black_slot);
        SNES_NTSC_HIRES_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black_slot);
        SNES_NTSC_HIRES_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(3, snes_ntsc_black_slot);
        SNES_NTSC_HIRES_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(4, snes_ntsc_black_slot);
        SNES_NTSC_HIRES_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(5, snes_ntsc_black_slot);
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
#  elif defined(__riscv) && defined(__riscv_v)
    #define CHECK_ALL_EQUAL CHECK_ALL_EQUAL_RVV
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

/* --- RISC-V Vector (RVV 1.0) --- */
#define CHECK_ALL_EQUAL_RVV do {                                                    \
  SIMD_INPUT_REGISTER_t first_line = ROTATE_OUT(xmm0, xmm6, 2);                    \
  uint16_t first_val = EXTRACT_SIMD_REGISTER_VALUE(first_line, 0);                  \
  vuint16m1_t ref = __riscv_vmv_v_x_u16m1(first_val, 4);                            \
  vbool16_t cmp   = __riscv_vmseq_vv_u16m1_b16(first_line, ref, 4);                \
  cmp = __riscv_vmand_mm_b16(cmp, __riscv_vmseq_vv_u16m1_b16(xmm1, ref, 4));       \
  cmp = __riscv_vmand_mm_b16(cmp, __riscv_vmseq_vv_u16m1_b16(xmm2, ref, 4));       \
  cmp = __riscv_vmand_mm_b16(cmp, __riscv_vmseq_vv_u16m1_b16(xmm3, ref, 4));       \
  cmp = __riscv_vmand_mm_b16(cmp, __riscv_vmseq_vv_u16m1_b16(xmm4, ref, 4));       \
  cmp = __riscv_vmand_mm_b16(cmp, __riscv_vmseq_vv_u16m1_b16(xmm5, ref, 4));       \
  uint16_t mask = (uint16_t)-(__riscv_vcpop_m_b16(cmp, 4) == 4);                   \
  this_colour = first_val & mask;                                                   \
} while(0)


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
            snes_ntsc_black_slot, snes_ntsc_black_slot, snes_ntsc_black_slot,
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

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(snes_ntsc_black_slot));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(2), 0);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(snes_ntsc_black_slot));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(3), 1);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(snes_ntsc_black_slot));
            xmm14 = INSERT_SIMD_REGISTER_VALUE(xmm14, SNES_NTSC_HIRES_OUT_SIMD(4), 2);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(snes_ntsc_black_slot));
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

// ---------------------------------------------------------------------------
// snes_ntsc_blit_hires_vec (Stage 2 of the NTSC filter re-architecture, Part B -
// see tools/ntsc_bench/analysis/ARCHITECTURE.md section 5): an adjacent-outputs
// vector kernel computing four consecutive output pixels per v4u32 register
// from contiguous 16-byte window loads. Bit-identical to
// snes_ntsc_blit_hires_fast() above (same 752/948-wide output, same tail
// semantics reproducing _fast's dropped-last-two-input-pixels behaviour), but
// built on the portable v4u32 layer (snes_ntsc_simd.h) rather than the per-ISA
// macros _fast uses, so this function compiles and is correct on every target
// (the plain-C v4u32 fallback makes it universal, unlike _fast which only
// exists when SNES_NTSC_HAVE_SIMD).
//
// One row is processed in groups of 24 input pixels / 28 output pixels, using
// 10 accumulator vectors A[0..9]: A[0..6] hold the group's own 28 outputs,
// A[7..9] accumulate the "spill" into the next group's first 12 outputs (a
// pixel's 14-tap contribution can extend up to 3 v4u32's past its group's
// own 7). Each group is seeded with the previous group's spill (rotated into
// A[0..2]) and starts with A[3..9] at zero; after the body runs, A[0..6] are
// clamped, converted and stored, then rotated for the next group. A virtual
// group -1 (22 black pixels then in[0], in[1], from zeroed accumulators)
// seeds group 0's first three vectors, reproducing _fast's black-padded
// prologue; a tail group (the last 14 real pixels then black) produces only
// A[0..4] plus one fixed "zero raw, clamped" vector for the final four
// outputs, reproducing _fast's black-padded, two-pixel-short tail exactly
// rather than computing a 6th accumulator _fast never produces.
//
// Pixel k (0..23) of any group contributes its 14-word kernel block (at
// alignment `a = (k%6)>>1`) to accumulator vectors v0..v0+M, where v0 and the
// intra-block residue r come from rel(k) = k + k/6 (the output offset of the
// pixel's first tap, relative to the group's own base) via r = rel(k)%4,
// v0 = (rel(k)-r)/4, M = (r==3) ? 4 : 3 - see the Stage 2 design brief for the
// derivation. Vector v0+m's load starts at word offset (3 - r + 4m) of the
// pixel's padded 20-word alignment block, which - thanks to the block's 3
// leading/3 trailing zero pad words (snes_ntsc_block_lead in snes_ntsc.h) -
// is always a valid in-block 16-byte window, so every load is unconditional
// and every offset a compile-time immediate.
#define SNES_NTSC_VEC_PIXEL(K, ALIGN, V0, R, M) do {                                    \
    const snes_ntsc_rgb_t* base = P + (size_t)slotv[K] * (size_t)snes_ntsc_slot_words   \
                                     + (size_t)(ALIGN) * (size_t)snes_ntsc_block_words;  \
    A[(V0)+0] = snes_v4_add(A[(V0)+0], snes_v4_load_u(base + (3-(R)+0)));                \
    A[(V0)+1] = snes_v4_add(A[(V0)+1], snes_v4_load_u(base + (3-(R)+4)));                \
    A[(V0)+2] = snes_v4_add(A[(V0)+2], snes_v4_load_u(base + (3-(R)+8)));                \
    A[(V0)+3] = snes_v4_add(A[(V0)+3], snes_v4_load_u(base + (3-(R)+12)));               \
    if ((M) == 4)                                                                        \
        A[(V0)+4] = snes_v4_add(A[(V0)+4], snes_v4_load_u(base + (3-(R)+16)));           \
} while (0)

// The 24-pixel group body, identical for every group (real, the virtual
// left-edge group -1, and the right-edge tail group): 24 uses of the PIXEL
// macro above, operating on the caller's local `P` (row's current phase
// plane base), `slotv` (24 slot indices - snes_ntsc_black_slot for a
// virtual/padding pixel) and `A` (the 10-vector accumulator array, A[0..2]
// pre-seeded by the caller from the previous group's spill, A[3..9] zero on
// entry). This is a *macro*, not a function: the whole 24-pixel sequence is
// textually inlined at each of the three call sites in
// snes_ntsc_blit_hires_vec() below, rather than called through a pointer to
// `A`, so the compiler sees A[0..9] as an ordinary fully-constant-indexed
// local array with no address ever taken and can scalar-replace it into
// plain registers (confirmed against the Cortex-A53 build - passing `A` by
// pointer to a real, non-inlined subroutine, which is what an equivalent
// `static inline` *function* compiled down to here, forced every
// accumulator through an actual stack slot on every pixel instead of a
// register, roughly doubling memory traffic for no algorithmic reason).
#define SNES_NTSC_VEC_GROUP_BODY() do {  \
    SNES_NTSC_VEC_PIXEL( 0, 0, 0, 0, 3); \
    SNES_NTSC_VEC_PIXEL( 1, 0, 0, 1, 3); \
    SNES_NTSC_VEC_PIXEL( 2, 1, 0, 2, 3); \
    SNES_NTSC_VEC_PIXEL( 3, 1, 0, 3, 4); \
    SNES_NTSC_VEC_PIXEL( 4, 2, 1, 0, 3); \
    SNES_NTSC_VEC_PIXEL( 5, 2, 1, 1, 3); \
    SNES_NTSC_VEC_PIXEL( 6, 0, 1, 3, 4); \
    SNES_NTSC_VEC_PIXEL( 7, 0, 2, 0, 3); \
    SNES_NTSC_VEC_PIXEL( 8, 1, 2, 1, 3); \
    SNES_NTSC_VEC_PIXEL( 9, 1, 2, 2, 3); \
    SNES_NTSC_VEC_PIXEL(10, 2, 2, 3, 4); \
    SNES_NTSC_VEC_PIXEL(11, 2, 3, 0, 3); \
    SNES_NTSC_VEC_PIXEL(12, 0, 3, 2, 3); \
    SNES_NTSC_VEC_PIXEL(13, 0, 3, 3, 4); \
    SNES_NTSC_VEC_PIXEL(14, 1, 4, 0, 3); \
    SNES_NTSC_VEC_PIXEL(15, 1, 4, 1, 3); \
    SNES_NTSC_VEC_PIXEL(16, 2, 4, 2, 3); \
    SNES_NTSC_VEC_PIXEL(17, 2, 4, 3, 4); \
    SNES_NTSC_VEC_PIXEL(18, 0, 5, 1, 3); \
    SNES_NTSC_VEC_PIXEL(19, 0, 5, 2, 3); \
    SNES_NTSC_VEC_PIXEL(20, 1, 5, 3, 4); \
    SNES_NTSC_VEC_PIXEL(21, 1, 6, 0, 3); \
    SNES_NTSC_VEC_PIXEL(22, 2, 6, 1, 3); \
    SNES_NTSC_VEC_PIXEL(23, 2, 6, 2, 3); \
} while (0)

void snes_ntsc_blit_hires_vec(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* restrict input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* restrict rgb_out, long out_pitch, long Alevel)
{
    const int W = in_width;
    const int G = (W - 16) / 24; // number of full 24-pixel/28-output groups (26 for 640, 33 for 808)
    const uint32_t alevel_u = (uint32_t)Alevel;

    // Fixed value stored at the last 4 outputs of every row (N-4..N-1, N = 28G+24):
    // the clamp+convert sequence applied to a raw accumulator of zero, exactly
    // reproducing _fast's tail ("zero vector, clamped") - independent of phase and
    // of the row's actual pixel content, so it is computed once for the whole call.
    const v4u32 fixed_tail = snes_ntsc_simd_clamp_convert(
        snes_v4_zero(), (uint32_t)snes_ntsc_clamp_mask, (uint32_t)snes_ntsc_clamp_add, alevel_u);

    for (; in_height; --in_height)
    {
        const snes_ntsc_rgb_t* restrict P = (const snes_ntsc_rgb_t*)ntsc->plane[burst_phase];
        SNES_NTSC_IN_T const* restrict line_in = input;
        uint32_t* restrict line_out = (uint32_t*)rgb_out;

        v4u32 A[10];
        for (int i = 0; i < 10; ++i) A[i] = snes_v4_zero();

        // Virtual group -1: pixels j = -22..1 (22 black slots then in[0], in[1]).
        // A[0..6] (outputs -28..-1) are discarded; only the spill A[7..9]
        // (outputs 0..11, i.e. group 0's first three vectors) is kept.
        {
            uint32_t slotv[24];
            for (int k = 0; k < 22; ++k) slotv[k] = (uint32_t)snes_ntsc_black_slot;
            slotv[22] = (uint32_t)line_in[0];
            slotv[23] = (uint32_t)line_in[1];
            SNES_NTSC_VEC_GROUP_BODY();
            A[0] = A[7]; A[1] = A[8]; A[2] = A[9];
            A[3] = A[4] = A[5] = A[6] = A[7] = A[8] = A[9] = snes_v4_zero();
        }

        // Main groups 0..G-1: pixels j = 24g+2 .. 24g+25, all real (holds
        // whenever (W-16) % 24 == 0, true for both 640 and 808 - see the
        // Stage 2 design brief).
        for (int g = 0; g < G; ++g) {
            uint32_t slotv[24];
            SNES_NTSC_IN_T const* restrict p = line_in + 2 + g * 24;
            for (int k = 0; k < 24; ++k) slotv[k] = (uint32_t)p[k];
            SNES_NTSC_VEC_GROUP_BODY();

            for (int v = 0; v < 7; ++v)
                A[v] = snes_ntsc_simd_clamp_convert(A[v], (uint32_t)snes_ntsc_clamp_mask,
                                                     (uint32_t)snes_ntsc_clamp_add, alevel_u);
            uint32_t* restrict out_g = line_out + (size_t)g * 28;
            for (int v = 0; v < 7; ++v)
                snes_v4_store_a(out_g + v * 4, A[v]);

            A[0] = A[7]; A[1] = A[8]; A[2] = A[9];
            A[3] = A[4] = A[5] = A[6] = A[7] = A[8] = A[9] = snes_v4_zero();
        }

        // Tail group G: the last 14 real pixels (j = W-14..W-1) then black
        // (j = W..W+9), reproducing _fast's end-of-row handling. Only A[0..4]
        // (outputs 28G..28G+19) are real sums; the last 4 outputs of the row
        // get the precomputed fixed_tail value rather than a 6th accumulator
        // _fast never computes.
        {
            uint32_t slotv[24];
            SNES_NTSC_IN_T const* restrict p = line_in + (W - 14);
            for (int k = 0; k < 14; ++k) slotv[k] = (uint32_t)p[k];
            for (int k = 14; k < 24; ++k) slotv[k] = (uint32_t)snes_ntsc_black_slot;
            SNES_NTSC_VEC_GROUP_BODY();

            for (int v = 0; v < 5; ++v)
                A[v] = snes_ntsc_simd_clamp_convert(A[v], (uint32_t)snes_ntsc_clamp_mask,
                                                     (uint32_t)snes_ntsc_clamp_add, alevel_u);
            uint32_t* restrict out_g = line_out + (size_t)G * 28;
            for (int v = 0; v < 5; ++v)
                snes_v4_store_a(out_g + v * 4, A[v]);
            snes_v4_store_a(out_g + 5 * 4, fixed_tail);
        }

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}
#undef SNES_NTSC_VEC_GROUP_BODY
#undef SNES_NTSC_VEC_PIXEL

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
