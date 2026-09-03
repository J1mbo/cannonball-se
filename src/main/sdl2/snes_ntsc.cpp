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
#include <cstdlib>  // atexit (debug-only FLAT constancy report, see gen_slot())
#include <atomic>   // debug-only counters below; snes_ntsc_init() regenerates slots under OpenMP
#include <vector>   // Part C classification buffer in snes_ntsc_blit_hires_vec()


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

// ---------------------------------------------------------------------------
// Debug-only FLAT constancy check (Part C / Stage 3 risk #1 in
// tools/ntsc_bench/analysis/ARCHITECTURE.md section 12: "the claim that a flat
// field gives one value at every position rests on correct_errors; it is
// derived, not yet dumped"). gen_slot() below computes FLAT[phase][slot] as 7
// independent raw sums (one per output position within a steady 12-pixel
// run); ALGORITHM.md 3.6 says a flat field must give the *same* value at
// every position and every phase, so under NDEBUG-off builds gen_slot()
// verifies exactly that for every slot it (re)generates and this reports the
// running tally at process exit. Purely informational - the blitter never
// reads these counters and does not rely on the check having run; it always
// uses whichever 7 (possibly-not-equal, in the hypothetical failure case)
// stored values gen_slot() actually computed. std::atomic because
// snes_ntsc_init() regenerates slots from an OpenMP-parallel loop.
#ifndef NDEBUG
static std::atomic<long> g_flat_check_slots{ 0 };
static std::atomic<long> g_flat_check_fails{ 0 };
static void snes_ntsc_flat_check_report()
{
    long total = g_flat_check_slots.load();
    long fails = g_flat_check_fails.load();
    fprintf(stderr,
        "snes_ntsc: FLAT constancy check: %ld/%ld slot-generations held "
        "(7 stored values equal within each phase, and equal across all 3 phases)\n",
        total - fails, total);
}
static int g_flat_check_report_installed = (atexit(snes_ntsc_flat_check_report), 0);
#endif

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

        // -------------------------------------------------------------------
        // Part C (Stage 3): derive PK/CK/FLAT for this slot/phase from the base
        // block just scattered above (see ARCHITECTURE.md section 6 and the
        // Stage 3 design brief's "Derived tables" / "Why these are exact").
        // All three are plain integer sums of dst_slot's 14-real-word
        // alignment blocks, so wrap-around 32-bit addition makes them exact
        // regardless of summation order.

        // K_a[n]: real tap n (0..13) of alignment a; 0 outside that range
        // (matches the base block's own zero padding, but computed directly
        // here rather than by reading the pad words, for clarity).
        auto Kread = [&](int a, int n) -> snes_ntsc_rgb_t {
            if (n < 0 || n > 13) return 0;
            return dst_slot[snes_ntsc_block_words * a + snes_ntsc_block_lead + n];
        };

        // 1. Pair kernel: PK_a[n] = K_a[n] + K_a[n-1], n = 0..14 (15 words).
        snes_ntsc_rgb_t PK[snes_ntsc_alignment_count][15];
        for (int a = 0; a < snes_ntsc_alignment_count; ++a) {
            for (int n = 0; n <= 14; ++n)
                PK[a][n] = Kread(a, n) + Kread(a, n - 1);

            snes_ntsc_rgb_t* pk_dst = ntsc->plane_pk[bidx][slot][a];
            for (int i = 0; i < (int)snes_ntsc_block_lead; ++i) pk_dst[i] = 0;
            for (int n = 0; n <= 14; ++n) pk_dst[(int)snes_ntsc_block_lead + n] = PK[a][n];
            for (int i = (int)snes_ntsc_block_lead + 15; i < (int)snes_ntsc_pk_block_words; ++i) pk_dst[i] = 0;
        }

        // 2. Chunk kernel: CK[n] = PK_0[n] + PK_1[n-2] + PK_2[n-4], n = 0..18
        // (19 words); PK_a[m] = 0 outside 0..14.
        auto PKread = [&](int a, int m) -> snes_ntsc_rgb_t {
            if (m < 0 || m > 14) return 0;
            return PK[a][m];
        };
        snes_ntsc_rgb_t CK[19];
        for (int n = 0; n <= 18; ++n)
            CK[n] = PKread(0, n) + PKread(1, n - 2) + PKread(2, n - 4);

        {
            snes_ntsc_rgb_t* ck_dst = ntsc->plane_ck[bidx][slot];
            for (int i = 0; i < (int)snes_ntsc_block_lead; ++i) ck_dst[i] = 0;
            for (int n = 0; n <= 18; ++n) ck_dst[(int)snes_ntsc_block_lead + n] = CK[n];
            for (int i = (int)snes_ntsc_block_lead + 19; i < (int)snes_ntsc_ck_words; ++i) ck_dst[i] = 0;
        }

        // 3. Flat pattern: raw[x] = CK[x] + CK[x+7] + (x<=4 ? CK[x+14] : 0),
        // x = 0..6. Stage 3 review: stored already CLAMPED AND CONVERTED to a
        // little-endian RGBA word, with Alevel forced to 0 here (so the alpha
        // byte, bits 31:24, is zero) rather than left RAW - the blitter used to
        // clamp+convert these on every emit, which made a steady-run chunk cost
        // at least as much as the general path it was supposed to avoid. Alevel
        // is applied at emit time as a plain OR of a precomputed
        // snes_v4_splat((uint32_t)Alevel << 24) (see snes_ntsc_blit_hires_vec),
        // which is exactly equivalent to clamp+convert-with-the-real-Alevel
        // because Alevel only ever contributes to bits 31:24 of the final word
        // (see SNES_NTSC_CLAMP_AND_CONVERT_SCALAR / snes_ntsc_simd_clamp_convert)
        // and clamp+convert-with-Alevel=0 leaves those bits zero - PROVIDED
        // Alevel fits in a byte (<=255), the same implicit requirement the
        // scalar macro already has (it doesn't mask Alevel either). Word 7 is
        // unused padding.
        auto CKread = [&](int n) -> snes_ntsc_rgb_t {
            if (n < 0 || n > 18) return 0;
            return CK[n];
        };
        {
            snes_ntsc_rgb_t* flat_dst = ntsc->plane_flat[bidx][slot];
            for (int x = 0; x <= 6; ++x) {
                snes_ntsc_rgb_t raw = CKread(x) + CKread(x + 7) + ((x <= 4) ? CKread(x + 14) : 0);
                SNES_NTSC_CLAMP_AND_CONVERT_SCALAR(raw, 0u);
                flat_dst[x] = raw;
            }
            flat_dst[7] = 0;
        }
    }

#ifndef NDEBUG
    // Debug-only FLAT constancy check (see the comment above g_flat_check_slots):
    // the 7 stored (clamped+converted) values just computed for each phase should
    // all be equal, and
    // identical across all 3 phases. Purely informational; never consulted by
    // the blitter.
    {
        bool ok = true;
        snes_ntsc_rgb_t v0 = ntsc->plane_flat[0][slot][0];
        for (int b = 0; b < snes_ntsc_burst_count && ok; ++b)
            for (int x = 0; x < 7; ++x)
                if (ntsc->plane_flat[b][slot][x] != v0) { ok = false; break; }
        g_flat_check_slots.fetch_add(1, std::memory_order_relaxed);
        if (!ok) {
            g_flat_check_fails.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "snes_ntsc: FLAT constancy FAILED for slot %d (colour 0x%04x)\n",
                slot, (unsigned)colour);
        }
    }
#endif
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

// Part C (Stage 3): the run-length ladder, see ARCHITECTURE.md section 6 and
// the Stage 3 design brief. Real game content is 65% uniform 6-pixel chunks
// and 91% identical even/odd pairs (frame_stats.py over tools/ntsc_bench/
// frames/), so most chunks never need the 24-pixel general kernel above at
// all: a chunk-wide classification (one compare per pixel, done once per row
// before any accumulation) picks, per 6-pixel chunk, the cheapest of four
// paths that all sum to exactly the same 32-bit wrap-around total the general
// kernel would have produced (PK/CK/FLAT are themselves just precomputed
// partial sums of the same K blocks - see gen_slot() in this file). Applied
// only to the row's G main groups (pixels 2..24G+1); the row's prologue
// (virtual group -1) and tail group keep using SNES_NTSC_VEC_GROUP_BODY()
// unchanged above - together they are at most 2 of a row's G+2 groups (26-33
// for the widths this filter supports), so shortcutting them buys nothing
// worth the extra edge-case surface of teaching the ladder about black
// padding and the tail's short/spill semantics.

// Compile-time experiment (Stage 3 review): which ladder levels are enabled,
// bit0 = flat/skip, bit1 = CK (uniform chunk), bit2 = PK (pair). Clearing a
// bit routes that case to the next path down (flat to uniform to pair to
// general), rather than deleting a level outright, so e.g. mask 1 still
// finds pairs/uniform colour runs via the general per-pixel path, just
// without the CK/PK tables. Default 7 = the full ladder; run_host.sh/
// run_pi.sh can override via BENCH_CXXFLAGS=-DSNES_NTSC_LADDER_MASK=<n> to
// measure which level(s) actually pay off on a given core (see README).
#ifndef SNES_NTSC_LADDER_MASK
#define SNES_NTSC_LADDER_MASK 7
#endif

// One PK (pair) window: identical to SNES_NTSC_VEC_PIXEL's load pattern
// (see its comment above snes_ntsc_blit_hires_vec) but reading the 24-word
// plane_pk block for alignment ALIGN instead of the 20-word plane block. PK's
// real span is 15 words (one more than K's 14), so the r-range needing a 5th
// load shifts from {r==3} to {r==2,r==3} - i.e. M is a simple threshold on R,
// so unlike SNES_NTSC_VEC_PIXEL (frozen Stage-2 code, left untouched) this
// derives it internally instead of taking it as a parameter. Pixel K's slot
// is slotv[K]; both pixels of an identical pair share one slot, so either
// index works - callers pass the pair's even one, whose (ALIGN,V0,R) is
// exactly its entry in the per-pixel K table above.
#define SNES_NTSC_VEC_PK(K, ALIGN, V0, R) do {                                         \
    const snes_ntsc_rgb_t* base = PPK + (size_t)slotv[K] * (size_t)snes_ntsc_pk_slot_words \
                                     + (size_t)(ALIGN) * (size_t)snes_ntsc_pk_block_words;  \
    A[(V0)+0] = snes_v4_add(A[(V0)+0], snes_v4_load_u(base + (3-(R)+0)));               \
    A[(V0)+1] = snes_v4_add(A[(V0)+1], snes_v4_load_u(base + (3-(R)+4)));               \
    A[(V0)+2] = snes_v4_add(A[(V0)+2], snes_v4_load_u(base + (3-(R)+8)));               \
    A[(V0)+3] = snes_v4_add(A[(V0)+3], snes_v4_load_u(base + (3-(R)+12)));              \
    if ((R) >= 2)                                                                      \
        A[(V0)+4] = snes_v4_add(A[(V0)+4], snes_v4_load_u(base + (3-(R)+16)));         \
} while (0)

// One CK (uniform chunk) window: SLOT's whole 6-pixel contribution in one go,
// reading the 28-word plane_ck block (no per-alignment dimension - CK already
// combines all 3 alignments, see gen_slot()). Same addressing shape again;
// CK's real span is 19 words, so every R needs at least 5 loads, with a 6th
// at R in {2,3} - again a simple threshold on R, derived internally.
#define SNES_NTSC_VEC_CK(SLOT, V0, R) do {                                             \
    const snes_ntsc_rgb_t* base = PCK + (size_t)(SLOT) * (size_t)snes_ntsc_ck_words;    \
    A[(V0)+0] = snes_v4_add(A[(V0)+0], snes_v4_load_u(base + (3-(R)+0)));               \
    A[(V0)+1] = snes_v4_add(A[(V0)+1], snes_v4_load_u(base + (3-(R)+4)));               \
    A[(V0)+2] = snes_v4_add(A[(V0)+2], snes_v4_load_u(base + (3-(R)+8)));               \
    A[(V0)+3] = snes_v4_add(A[(V0)+3], snes_v4_load_u(base + (3-(R)+12)));              \
    A[(V0)+4] = snes_v4_add(A[(V0)+4], snes_v4_load_u(base + (3-(R)+16)));              \
    if ((R) >= 2)                                                                      \
        A[(V0)+5] = snes_v4_add(A[(V0)+5], snes_v4_load_u(base + (3-(R)+20)));         \
} while (0)

// Emit chunk C's (C = 4*g+n) 7 outputs as the FLAT constant for its slot,
// unconditionally overwriting out_g+7*n..+6. Stage 3 review, change 1:
// plane_flat now stores the clamp+converted-with-Alevel=0 word (see
// gen_slot()), so this is just 2 unaligned loads + 2 ORs (to fold in the
// real Alevel via the caller's precomputed alpha_vec) + 2 unaligned stores -
// no clamp, no convert, versus the two full snes_ntsc_simd_clamp_convert
// calls this used to cost. Needs PFLAT/out_g/alpha_vec from the caller.
#define SNES_NTSC_VEC_EMIT_FLAT(C_, N_) do {                                           \
    const snes_ntsc_rgb_t* flat_clamped = PFLAT + (size_t)CS(C_) * (size_t)snes_ntsc_flat_words; \
    v4u32 out0 = snes_v4_or(snes_v4_load_u(flat_clamped + 0), alpha_vec);              \
    v4u32 out1 = snes_v4_or(snes_v4_load_u(flat_clamped + 3), alpha_vec);              \
    uint32_t* dst = out_g + 7 * (N_);                                                  \
    snes_v4_store_u(dst, out0);                                                        \
    snes_v4_store_u(dst + 3, out1);                                                    \
} while (0)

// Optional per-path instrumentation (off by default - #define SNES_NTSC_VEC_STATS
// to enable, e.g. -DSNES_NTSC_VEC_STATS on the bench build). Chunk-granularity
// counts for the "skip" (steady run) and "uniform" (CK) paths; pair-decision
// granularity (each chunk has exactly 3) for "pair" (PK) vs "general" (K),
// so g_stat_pair+g_stat_general == 3*(g_stat_total - g_stat_skip - g_stat_uniform).
// atomics: the game runs the two half-frame render threads concurrently.
#ifdef SNES_NTSC_VEC_STATS
#include <atomic>
static std::atomic<long> g_stat_total{ 0 }, g_stat_skip{ 0 }, g_stat_uniform{ 0 };
static std::atomic<long> g_stat_pair{ 0 }, g_stat_general{ 0 };
static void snes_ntsc_vec_stats_report()
{
    long total = g_stat_total.load(), skip = g_stat_skip.load(), uni = g_stat_uniform.load();
    long pair = g_stat_pair.load(), gen = g_stat_general.load();
    fprintf(stderr,
        "snes_ntsc_vec ladder stats: chunks=%ld skip(steady)=%.1f%% uniform(CK)=%.1f%% "
        "| pair-decisions: pair(PK)=%.1f%% general(K)=%.1f%% (of %ld)\n",
        total, total ? 100.0 * skip / total : 0.0, total ? 100.0 * uni / total : 0.0,
        (pair + gen) ? 100.0 * pair / (pair + gen) : 0.0,
        (pair + gen) ? 100.0 * gen / (pair + gen) : 0.0, pair + gen);
}
static int g_stat_report_installed = (atexit(snes_ntsc_vec_stats_report), 0);
#define SNES_NTSC_STAT(counter) (counter).fetch_add(1, std::memory_order_relaxed)
#else
#define SNES_NTSC_STAT(counter) ((void)0)
#endif

// One 6-pixel chunk, global chunk index C (a runtime value: C = 4*g + n where
// g is the current group's runtime loop counter and n in 0..3 is baked into
// which of the 4 macros below a call site uses - only the (align,v0,r)
// constants need to be compile-time immediates, and they only depend on n).
// CS/isFlat/isSkip come from the caller's row-wide classification pre-pass
// (see snes_ntsc_blit_hires_vec below); both are mask-aware (isFlat folds in
// SNES_NTSC_LADDER_MASK bit0, so isSkip - built on isFlat - inherits it), and
// the CK/PK checks below are separately gated on bits 1/2. Each of the 3
// pairs is given explicitly as (K, ALIGN, EV0,ER0, OV0,OR0): K is the even
// pixel's index into slotv (the odd one is K+1); (ALIGN,EV0,ER0) is the even
// pixel's own entry in the per-pixel K table above (SNES_NTSC_VEC_GROUP_BODY)
// - which is also exactly the PK window's (align,v0,r), since an identical
// pair's sum lands at the even pixel's own t0 - and (OV0,OR0) is the odd
// pixel's entry in that same table (its (v0,r) is not simply the even one's
// r+1: r wraps to 0 with v0+1 partway through this 24-pixel table, so both
// are spelled out here rather than derived, to avoid re-deriving that carry
// incorrectly).
#define SNES_NTSC_VEC_CHUNK(C_EXPR, CK_V0, CK_R,                                       \
                             K0, K0A, EV0,ER0, OV0,OR0,                                 \
                             K2, K2A, EV2,ER2, OV2,OR2,                                 \
                             K4, K4A, EV4,ER4, OV4,OR4) do {                            \
    const int C = (C_EXPR);                                                            \
    SNES_NTSC_STAT(g_stat_total);                                                      \
    if (isSkip(C)) {                                                                   \
        /* Steady run: this chunk's whole contribution (outputs 7C..7C+18) will be     \
           overwritten by the FLAT pass below (this chunk's own emit_flat(C), and      \
           chunks C+1/C+2's, whether those land in this group or spill into the        \
           next main group) - see the design brief's "why skip_kernel is exact". */    \
        SNES_NTSC_STAT(g_stat_skip);                                                   \
    } else if ((SNES_NTSC_LADDER_MASK & 2) && CS(C) != 0xFFFFu) {                       \
        SNES_NTSC_STAT(g_stat_uniform);                                                \
        SNES_NTSC_VEC_CK((uint32_t)CS(C), CK_V0, CK_R);                                \
    } else {                                                                           \
        if ((SNES_NTSC_LADDER_MASK & 4) && slotv[K0] == slotv[(K0)+1]) { SNES_NTSC_STAT(g_stat_pair); SNES_NTSC_VEC_PK(K0, K0A, EV0, ER0); } \
        else { SNES_NTSC_STAT(g_stat_general); SNES_NTSC_STAT(g_stat_general);         \
               SNES_NTSC_VEC_PIXEL(K0,     K0A, EV0, ER0, (ER0)==3?4:3);               \
               SNES_NTSC_VEC_PIXEL((K0)+1, K0A, OV0, OR0, (OR0)==3?4:3); }             \
        if ((SNES_NTSC_LADDER_MASK & 4) && slotv[K2] == slotv[(K2)+1]) { SNES_NTSC_STAT(g_stat_pair); SNES_NTSC_VEC_PK(K2, K2A, EV2, ER2); } \
        else { SNES_NTSC_STAT(g_stat_general); SNES_NTSC_STAT(g_stat_general);         \
               SNES_NTSC_VEC_PIXEL(K2,     K2A, EV2, ER2, (ER2)==3?4:3);               \
               SNES_NTSC_VEC_PIXEL((K2)+1, K2A, OV2, OR2, (OR2)==3?4:3); }             \
        if ((SNES_NTSC_LADDER_MASK & 4) && slotv[K4] == slotv[(K4)+1]) { SNES_NTSC_STAT(g_stat_pair); SNES_NTSC_VEC_PK(K4, K4A, EV4, ER4); } \
        else { SNES_NTSC_STAT(g_stat_general); SNES_NTSC_STAT(g_stat_general);         \
               SNES_NTSC_VEC_PIXEL(K4,     K4A, EV4, ER4, (ER4)==3?4:3);               \
               SNES_NTSC_VEC_PIXEL((K4)+1, K4A, OV4, OR4, (OR4)==3?4:3); }             \
    } \
} while (0)

// The 4 chunk positions within a group (n = 0..3, global chunk index
// C = 4*(G) + n), read directly off the per-pixel K table above: chunk n
// owns pixels 6n..6n+5, pairs (6n,6n+1)/(6n+2,6n+3)/(6n+4,6n+5). CK's
// (v0,r) are the ones derived in the design brief ("residues r = 0,3,2,1 for
// c' = 0,1,2,3, first vector v0 = 0,1,3,5").
#define SNES_NTSC_VEC_CHUNK0(G) SNES_NTSC_VEC_CHUNK(4*(G)+0, 0, 0,  \
     0, 0, 0,0, 0,1,    2, 1, 0,2, 0,3,    4, 2, 1,0, 1,1)
#define SNES_NTSC_VEC_CHUNK1(G) SNES_NTSC_VEC_CHUNK(4*(G)+1, 1, 3,  \
     6, 0, 1,3, 2,0,    8, 1, 2,1, 2,2,   10, 2, 2,3, 3,0)
#define SNES_NTSC_VEC_CHUNK2(G) SNES_NTSC_VEC_CHUNK(4*(G)+2, 3, 2,  \
    12, 0, 3,2, 3,3,   14, 1, 4,0, 4,1,   16, 2, 4,2, 4,3)
#define SNES_NTSC_VEC_CHUNK3(G) SNES_NTSC_VEC_CHUNK(4*(G)+3, 5, 1,  \
    18, 0, 5,1, 5,2,   20, 1, 5,3, 6,0,   22, 2, 6,1, 6,2)

void snes_ntsc_blit_hires_vec(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* restrict input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* restrict rgb_out, long out_pitch, long Alevel)
{
    const int W = in_width;
    const int G = (W - 16) / 24; // number of full 24-pixel/28-output groups (26 for 640, 33 for 808)
    const uint32_t alevel_u = (uint32_t)Alevel;
    // Stage 3 review, change 3: clamp_mask_vec/clamp_add_vec/alpha_vec are per-call
    // constants (Alevel and the clamp constants never change within one blit call),
    // so they are splatted once here instead of being re-splatted by every one of the
    // thousands of snes_ntsc_simd_clamp_convert() calls a frame makes. alpha_vec is
    // Alevel already positioned at bits 31:24 (see snes_ntsc_simd_clamp_convert's
    // comment and gen_slot()'s FLAT computation) - Alevel must therefore fit in a
    // byte (<=255, true for the game's Alevel=255), the same requirement the scalar
    // clamp+convert macro already has.
    const v4u32 clamp_mask_vec = snes_v4_splat((uint32_t)snes_ntsc_clamp_mask);
    const v4u32 clamp_add_vec  = snes_v4_splat((uint32_t)snes_ntsc_clamp_add);
    const v4u32 alpha_vec      = snes_v4_splat(alevel_u << 24);

    // Fixed value stored at the last 4 outputs of every row (N-4..N-1, N = 28G+24):
    // the clamp+convert sequence applied to a raw accumulator of zero, exactly
    // reproducing _fast's tail ("zero vector, clamped") - independent of phase and
    // of the row's actual pixel content, so it is computed once for the whole call.
    const v4u32 fixed_tail = snes_ntsc_simd_clamp_convert(snes_v4_zero(), clamp_mask_vec, clamp_add_vec, alpha_vec);

    // Part C classification buffer (see the SNES_NTSC_VEC_CHUNK* macros above):
    // chunk_slot[C] for global chunk index C = -4 .. 4G+5 (main chunks are
    // 0..4G-1; the extra -4..-1 covers lookback into the prologue and 4G..4G+5
    // covers lookahead into the tail, both needed by isFlat()/skip_kernel() at
    // the row's two ends). Sized once per call (W, hence G, is constant across
    // every row of one snes_ntsc_blit_hires_vec() call) and refilled every row.
    const int C_min = -4;
    const int C_max = 4 * G + 5;
    const size_t n_chunks = (size_t)(C_max - C_min + 1);
    std::vector<uint16_t> chunk_slot_buf(n_chunks);
    // flat_buf/r_buf (see isFlat's precompute below) are refilled every row but sized
    // once here, outside the row loop - hoisted out of a per-row scope they were
    // originally in, since a std::vector re-allocating on every one of a frame's 448
    // rows (per render thread) was real, measurable heap-churn overhead on the Pi that
    // had nothing to do with the ladder's own arithmetic (see README).
    std::vector<uint8_t> flat_buf(n_chunks);
    std::vector<uint8_t> r_buf(n_chunks);

    for (; in_height; --in_height)
    {
        const snes_ntsc_rgb_t* restrict P     = (const snes_ntsc_rgb_t*)ntsc->plane[burst_phase];
        const snes_ntsc_rgb_t* restrict PPK   = (const snes_ntsc_rgb_t*)ntsc->plane_pk[burst_phase];
        const snes_ntsc_rgb_t* restrict PCK   = (const snes_ntsc_rgb_t*)ntsc->plane_ck[burst_phase];
        const snes_ntsc_rgb_t* restrict PFLAT = (const snes_ntsc_rgb_t*)ntsc->plane_flat[burst_phase];
        SNES_NTSC_IN_T const* restrict line_in = input;
        uint32_t* restrict line_out = (uint32_t*)rgb_out;

        // Row-wide classification pre-pass (design brief's "Classification"
        // section): chunk C's 6 pixels are at row-positions 6C+2..6C+7 (this
        // is the same numbering the group/pixel indexing above already uses -
        // e.g. main group g's pixel k is at row-position 2+24g+k, and chunk
        // C=4g+n owns k=6n..6n+5, i.e. positions 2+24g+6n..+5 = 6(4g+n)+2..+7).
        // A position outside [0,W) is black-slot padding, exactly matching
        // the prologue/tail's own black fill below. One compare per pixel.
        //
        // Every chunk this loop is ever asked about for a *main* group (C in
        // 0..4G-1) always has its whole 6-pixel window inside [0,W) - only the
        // handful of chunks classified purely for lookback/lookahead at the
        // row's two ends (C < 0 or C > (W-8)/6) can poke outside it. So this
        // is split into an unchecked fast path (no branch, no bounds-checked
        // helper call, for the ~90%+ of chunks that are the common case on
        // any row wide enough to have a full group) and a checked slow path
        // for just those few edge chunks - avoiding a per-pixel bounds check
        // and function-call indirection across the whole row, which measured
        // as a real cost on the Pi's in-order core (see README).
        auto pixel_slot = [&](int pos) -> uint32_t {
            return (pos < 0 || pos >= W) ? (uint32_t)snes_ntsc_black_slot : (uint32_t)line_in[pos];
        };
        for (int C = C_min; C <= C_max; ++C) {
            int pos0 = 6 * C + 2;
            uint32_t s0; bool uniform;
            if (pos0 >= 0 && pos0 + 5 < W) {
                SNES_NTSC_IN_T const* restrict pp = line_in + pos0;
                s0 = (uint32_t)pp[0];
                uniform = (uint32_t)pp[1] == s0 && (uint32_t)pp[2] == s0 && (uint32_t)pp[3] == s0 &&
                          (uint32_t)pp[4] == s0 && (uint32_t)pp[5] == s0;
            } else {
                s0 = pixel_slot(pos0);
                uniform = true;
                for (int i = 1; i < 6; ++i) {
                    if (pixel_slot(pos0 + i) != s0) { uniform = false; break; }
                }
            }
            chunk_slot_buf[(size_t)(C - C_min)] = uniform ? (uint16_t)s0 : (uint16_t)0xFFFFu;
        }
        // CS(C)/isFlat(C)/isSkip(C) are *macros*, not lambdas/functions, deliberately:
        // they are called many times per chunk from inside SNES_NTSC_VEC_CHUNK,
        // interleaved with A[]-touching code, and Stage 2 already found the hard way
        // (see this file's "Register-allocation fix" note in the README) that a real,
        // non-inlined call in that position forces the compiler to spill the live
        // accumulators around every call even when the callee is trivial - a macro
        // can't do that, it is textually inline by construction. isFlat folds in
        // SNES_NTSC_LADDER_MASK bit0 (the flat/skip experiment level) so every
        // consumer - isSkip, the group-level all-skip check, and the per-chunk
        // covered-vector/FLAT-overwrite logic below - is automatically gated by it
        // without separately testing the mask everywhere. Valid only inside
        // snes_ntsc_blit_hires_vec, where chunk_slot_buf/flat_buf/C_min/G are in
        // scope; #undef'd with the other Part B/C macros right after this function.
#define CS(C) (chunk_slot_buf[(size_t)((C) - C_min)])
#define isFlat(C) (((SNES_NTSC_LADDER_MASK) & 1) && (flat_buf[(size_t)((C) - C_min)] != 0))
#define isSkip(C) (((C) + 2 <= 4 * G - 1) && isFlat(C) && isFlat((C)+1) && isFlat((C)+2))

        // isFlat(C) (design brief) is queried many times per chunk (the skip
        // test for chunks C, C-1 and C-2 all consult it, and the FLAT-
        // overwrite pass consults it again for the same C) - precomputing it
        // once per row into a plain array (flat_buf, declared once above the
        // row loop and just refilled here) turns every one of those repeated
        // queries into a single read instead of recomputing isR(C)/isR(C-1)
        // (each itself 2 more CS() reads) from scratch every time, which was
        // the other measurable chunk of the classification overhead. This
        // precompute always evaluates the true flat property (mask bit0 is
        // applied only by the isFlat accessor macro above, not here).
        {
            for (int C = C_min; C <= C_max; ++C) {
                uint16_t sc = CS(C);
                r_buf[(size_t)(C - C_min)] = (uint8_t)((sc != 0xFFFFu) && (C > C_min) && (sc == CS(C - 1)));
            }
#define Rbuf(C) ((C) >= C_min && r_buf[(size_t)((C) - C_min)] != 0)
            for (int C = C_min; C <= C_max; ++C)
                flat_buf[(size_t)(C - C_min)] = (uint8_t)(Rbuf(C) && Rbuf(C - 1));
#undef Rbuf
        }

        v4u32 A[10];
        for (int i = 0; i < 10; ++i) A[i] = snes_v4_zero();

        // Virtual group -1: pixels j = -22..1 (22 black slots then in[0], in[1]).
        // A[0..6] (outputs -28..-1) are discarded; only the spill A[7..9]
        // (outputs 0..11, i.e. group 0's first three vectors) is kept. Always
        // the unmodified Stage-2 general kernel (see the Part C banner comment
        // above for why the ladder is not applied to this virtual group).
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
        // Stage 2 design brief). Part C's run-length ladder replaces the
        // unconditional 24-pixel body here: each of the group's 4 chunks
        // (global index C = 4g+n) independently picks the cheapest of
        // skip/uniform-CK/pair-PK/general-K per the classification above.
        //
        // Stage 3 review, change 2: two group-level shortcuts on top of the
        // per-chunk ladder, both there to stop a flat/steady chunk costing as
        // much as (or more than) the general path once clamp+convert is
        // accounted for:
        //  - All 4 chunks skip_kernel (the common case well inside a sky/road
        //    run): the whole group's 28 outputs are guaranteed covered by the
        //    4 chunks' own emit_flat overwrites, so there is no need to
        //    accumulate anything, nor to clamp/convert/store A[0..6] at all -
        //    just the 4 FLAT emits. The carry into the next group (A[0..2])
        //    is set to zero along with the rest of A[]: chunk 4g+3 being
        //    skip_kernel guarantees emit_flat(4g+4) and emit_flat(4g+5) (the
        //    next group's first two chunks), so whatever A[0..2] would have
        //    fed there is going to be overwritten unconditionally anyway -
        //    zero is exact, not just convenient.
        //  - Otherwise (a "partially flat" group): run the chunk ladder as
        //    before, then clamp/convert/store only the output vectors that
        //    are not entirely covered by an emit_flat chunk (a 7-bit
        //    coverage mask derived from the 4 chunks' own emit_flat flags -
        //    vector v covers outputs 4v..4v+3, chunk c' covers 7c'..7c'+6;
        //    v0 needs only chunk0 flat, v1 straddles chunk0/chunk1 and needs
        //    both, v2 needs chunk1, v3 straddles chunk1/chunk2, v4 needs
        //    chunk2, v5 straddles chunk2/chunk3, v6 needs chunk3) - any
        //    vector left uncovered by this group's own flat chunks still
        //    gets the normal clamp+convert+store, and the subsequent FLAT
        //    overwrite pass (unchanged) still runs for every emit_flat chunk
        //    regardless, so a vector that is only partially covered by one
        //    of two straddling chunks is still stored normally and then has
        //    the covered part correctly overwritten - never skipped, only
        //    ever redundant in that one edge case.
        for (int g = 0; g < G; ++g) {
            uint32_t* restrict out_g = line_out + (size_t)g * 28;

            if (isSkip(4*g+0) && isSkip(4*g+1) && isSkip(4*g+2) && isSkip(4*g+3)) {
                SNES_NTSC_STAT(g_stat_total); SNES_NTSC_STAT(g_stat_total);
                SNES_NTSC_STAT(g_stat_total); SNES_NTSC_STAT(g_stat_total);
                SNES_NTSC_STAT(g_stat_skip); SNES_NTSC_STAT(g_stat_skip);
                SNES_NTSC_STAT(g_stat_skip); SNES_NTSC_STAT(g_stat_skip);
                SNES_NTSC_VEC_EMIT_FLAT(4*g+0, 0);
                SNES_NTSC_VEC_EMIT_FLAT(4*g+1, 1);
                SNES_NTSC_VEC_EMIT_FLAT(4*g+2, 2);
                SNES_NTSC_VEC_EMIT_FLAT(4*g+3, 3);
                for (int i = 0; i < 10; ++i) A[i] = snes_v4_zero();
                continue;
            }

            uint32_t slotv[24];
            SNES_NTSC_IN_T const* restrict p = line_in + 2 + g * 24;
            for (int k = 0; k < 24; ++k) slotv[k] = (uint32_t)p[k];

            SNES_NTSC_VEC_CHUNK0(g);
            SNES_NTSC_VEC_CHUNK1(g);
            SNES_NTSC_VEC_CHUNK2(g);
            SNES_NTSC_VEC_CHUNK3(g);

            {
                const bool f0 = isFlat(4*g+0), f1 = isFlat(4*g+1), f2 = isFlat(4*g+2), f3 = isFlat(4*g+3);
                const bool covered[7] = { f0, f0 && f1, f1, f1 && f2, f2, f2 && f3, f3 };
                for (int v = 0; v < 7; ++v) {
                    if (covered[v]) continue;
                    A[v] = snes_ntsc_simd_clamp_convert(A[v], clamp_mask_vec, clamp_add_vec, alpha_vec);
                    snes_v4_store_a(out_g + v * 4, A[v]);
                }

                // FLAT overwrite pass: any of this group's 4 chunks whose isFlat(C)
                // holds gets its 7 outputs (7n..7n+6, always inside this group's
                // own 28) unconditionally replaced with the already clamp+converted
                // FLAT constant for its slot (see gen_slot() and
                // SNES_NTSC_VEC_EMIT_FLAT) - correct regardless of whether that
                // chunk's own accumulation above ran skip/CK/PK/K, or whether the
                // vector it lives in was even stored above (see the coverage mask
                // just computed): skip_kernel(C) already guarantees every chunk it
                // omits gets overwritten this way, either here or in this row's
                // next group.
                if (f0) SNES_NTSC_VEC_EMIT_FLAT(4*g+0, 0);
                if (f1) SNES_NTSC_VEC_EMIT_FLAT(4*g+1, 1);
                if (f2) SNES_NTSC_VEC_EMIT_FLAT(4*g+2, 2);
                if (f3) SNES_NTSC_VEC_EMIT_FLAT(4*g+3, 3);
            }

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
                A[v] = snes_ntsc_simd_clamp_convert(A[v], clamp_mask_vec, clamp_add_vec, alpha_vec);
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
#undef SNES_NTSC_VEC_PK
#undef SNES_NTSC_VEC_CK
#undef SNES_NTSC_VEC_EMIT_FLAT
#undef SNES_NTSC_VEC_CHUNK
#undef SNES_NTSC_VEC_CHUNK0
#undef SNES_NTSC_VEC_CHUNK1
#undef SNES_NTSC_VEC_CHUNK2
#undef SNES_NTSC_VEC_CHUNK3
#undef SNES_NTSC_STAT
#undef CS
#undef isFlat
#undef isSkip

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
