/* Based on snes_ntsc 0.2.2. http://www.slack.net/~ant/

Updates for CannonBall-SE are Copyright (c) 2025-2026 James Pearce:
- S16 emulation at 24-bit output depth
- threaded initialisation
- slot-indexed, phase-major kernel table (snes_ntsc_init()/update_slot(s)(), see
  snes_ntsc.h's own header comment for the stage-by-stage history)
- snes_ntsc_blit_hires_vec(): the sole hires blitter, a portable (NEON/SSE2/RVV/
  scalar, see snes_ntsc_simd.h) adjacent-outputs vector kernel with an inline
  run-length classifier (uniform chunk / identical pair / steady-run shortcuts
  on top of the general per-pixel path) - see this file's banner comment above
  snes_ntsc_blit_hires_vec() for the full design. The earlier per-ISA
  snes_ntsc_blit_hires_fast() (SSE4/NEON/RVV intrinsics) and the separate
  scalar snes_ntsc_blit_hires() have both been retired (Stage 4); the lores
  blitter (snes_ntsc_blit()) is untouched by any of this.
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

// ---------------------------------------------------------------------------
// snes_ntsc_blit_hires_vec (Part B of the NTSC filter re-architecture, see
// tools/ntsc_bench/analysis/ARCHITECTURE.md section 5): the only hires blitter
// as of Stage 4 - the old per-ISA snes_ntsc_blit_hires_fast() (SSE4/NEON/RVV
// intrinsics) and the separate scalar snes_ntsc_blit_hires() have both been
// retired (their 752/948-wide output and end-of-row/tail semantics are exactly
// what this kernel reproduces below). An adjacent-outputs vector kernel
// computing four consecutive output pixels per v4u32 register from contiguous
// 16-byte window loads, built on the portable v4u32 layer (snes_ntsc_simd.h)
// rather than per-ISA intrinsic macros, so this function compiles and is
// correct on every target (the plain-C v4u32 fallback makes it universal,
// unlike the retired _fast, which only ever existed when SNES_NTSC_HAVE_SIMD).
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

// Part C (Stage 3, re-fused in Stage 4): the run-length ladder, see
// ARCHITECTURE.md section 6. Real game content is 65% uniform 6-pixel chunks
// and 91% identical even/odd pairs (frame_stats.py over tools/ntsc_bench/
// frames/), so most chunks never need the 24-pixel general kernel above at
// all: a per-chunk classification picks, per 6-pixel chunk, the cheapest of
// four paths that all sum to exactly the same 32-bit wrap-around total the
// general kernel would have produced (PK/CK/FLAT are themselves just
// precomputed partial sums of the same K blocks - see gen_slot() in this
// file). Applied only to the row's G main groups (pixels 2..24G+1); the
// row's prologue (virtual group -1) keeps using SNES_NTSC_VEC_GROUP_BODY()
// unchanged above (it is at most 1 of a row's G+2 groups, so shortcutting it
// buys nothing worth the edge-case surface of teaching the ladder about
// black padding); the tail group also keeps SNES_NTSC_VEC_GROUP_BODY() for
// its own accumulation, but does participate in the classifier's run-end
// bookkeeping (see the tail's own comment below).
//
// Stage 4: fused, lookahead-free classification. Stage 3 classified an
// entire row into a heap-allocated table (chunk_slot_buf/flat_buf/r_buf,
// three std::vectors sized once per call and refilled every row) *before*
// any accumulation, so a chunk's flat/skip decision could look ahead at
// chunks up to 2 positions later (isSkip(C) needed isFlat(C), isFlat(C+1)
// *and* isFlat(C+2)). That pre-pass reads every pixel of the row a second
// time (accumulation reads them again via slotv) and, on content the ladder
// can't shortcut anyway, buys nothing - pure overhead. Stage 4 replaces it
// with a state machine carried across chunks in processing order, needing
// only the current group's own 24 already-loaded slots (slotv) plus a
// handful of scalars carried from the last 1-2 chunks - no row-wide table,
// no std::vector, no lookahead:
//
//   uniform(C):  branch-free - OR the XORs of the chunk's other 5 slots
//                against its first; uniform iff the result is zero.
//   run_len(C):  0 if not uniform(C); 1 if uniform(C) but its slot differs
//                from run_len(C-1)'s chunk (or C-1 wasn't uniform); else
//                run_len(C-1) + 1. Reset to 0 at the start of every row (the
//                prologue's own last "chunk" always contains in[0]/in[1]
//                after black padding, so it can never be uniform - the
//                prologue is never classified at all, group 0 just starts
//                with run_len = 0, matching what a real classification of
//                the prologue would have produced anyway).
//   emit_flat(C) = skipped(C) = run_len(C) >= 3 (chunks C-2, C-1, C are all
//                uniform and share one slot): after the group's normal
//                clamp/convert/store, chunk C's own 7 outputs are
//                unconditionally overwritten with the FLAT constant for that
//                slot (SNES_NTSC_VEC_EMIT_FLAT) - the vector-coverage mask
//                still skips clamp/convert/store for any vector fully
//                covered by such a chunk, exactly as Stage 3 did.
//
// A skipped(C) chunk is never accumulated. Its 19-word CK contribution would
// have reached outputs up to 7C+18 - i.e. into chunks C+1 and C+2 - so *if*
// C+1 and C+2 turn out to extend the same run (both also end up skipped,
// hence flat-emitted), those columns are correctly produced by C+1/C+2's own
// FLAT overwrites and C's missing contribution was never needed. But the
// classifier has no lookahead, so it cannot know that in advance; instead,
// when it later processes some chunk C' with run_len(C') <= 1 (uniformity
// broke, or a fresh differently-coloured run started - i.e. the previous run,
// if any, just ended at C'-1), it checks whether the immediately preceding
// chunk (C'-1) and/or the one before that (C'-2) were skipped, and if so adds
// each one's CK contribution retroactively, before accumulating C' itself -
// exactly restoring the sum the general kernel would have produced, just
// later than Stage 3 did it. This retro add reuses the ordinary CK window
// (SNES_NTSC_VEC_CK), placed at the target chunk's own compile-time (v0,r) -
// C' - 1 and C' - 2 are always within 2 positions of C', so both (v0,r) pairs
// are known at compile time for every one of the 4 chunk positions x 2
// lookback distances, no runtime derivation needed (see
// SNES_NTSC_VEC_CHUNK_N's R1V0,R1R/R2V0,R2R parameters below). When the
// retro target crosses into the *previous* group (only possible for chunk
// position 0 needing d=1, or positions 0-1 needing d=2), part of its 19-word
// window lands at a negative vector index relative to the current group's
// A[] - i.e. in output columns the previous group already clamped, converted
// and stored. Those columns are safe to drop unadded: skipped(C'-d) implies
// its own emit_flat(C'-d) already overwrote them with the correct FLAT
// constant when that earlier group was finalized, so nothing there is ever
// read back out of A[] again. SNES_NTSC_VEC_CK's own bounds guards
// (added in Stage 4) drop exactly those loads, at compile time (v0 is always
// a literal at every call site).
//
// An all-skip group (all 4 chunks have run_len >= 3) still short-circuits
// exactly as the Stage 3 review's group-level fast path did: no accumulation,
// no clamp/convert/store, just 4 FLAT emits and A[] zeroed for the next
// group's seed - correct because a chunk with run_len >= 3 can never also be
// pending a retro add (retro add only fires when run_len <= 1), so there is
// nothing outstanding to lose by skipping the whole group's accumulation.

// Compile-time experiment (Stage 3 review): which ladder levels are enabled,
// bit0 = flat/skip, bit1 = CK (uniform chunk), bit2 = PK (pair). Clearing a
// bit routes that case to the next path down (flat to uniform to pair to
// general), rather than deleting a level outright, so e.g. mask 1 still
// finds pairs/uniform colour runs via the general per-pixel path, just
// without the CK/PK tables. run_host.sh/run_pi.sh can override via
// BENCH_CXXFLAGS=-DSNES_NTSC_LADDER_MASK=<n> to measure which level(s)
// actually pay off on a given core (see README).
//
// Stage 4: default changed from 7 to 3 (flat/skip + CK, PK off). The Stage 3
// mask experiment (README "Ladder-level experiment") measured PK (bit2) as
// giving nothing on real content (`--input`: 9.26 ms either way) while
// actively regressing transition-heavy content (`--pattern runs`: 11.99 ms
// at mask 3 vs 15.81 ms at mask 7, a 32% regression) - the extra pair-
// equality branch and plane_pk lookup is frequently attempted and wasted
// whenever a chunk is neither flat nor uniform. PK's table (plane_pk,
// gen_slot()'s PK derivation) and code (SNES_NTSC_VEC_PK, the pair branch
// below) are both kept, purely so mask 7 (or any BENCH_CXXFLAGS override)
// can still re-enable it for content that does resemble `--pattern runs`.
#ifndef SNES_NTSC_LADDER_MASK
#define SNES_NTSC_LADDER_MASK 3
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
//
// Stage 4: V0 may be negative here (a retroactive add of a chunk that turned
// out to belong to the *previous* group's coordinate frame - see the
// classifier's "run end" handling in snes_ntsc_blit_hires_vec below), so every
// store is guarded by "target vector index >= 0". V0/R are always compile-
// time immediates at every call site (the classifier's own placement, and
// every retroactive placement, are both fully enumerated by chunk position),
// so each guard folds away entirely - to nothing for the classifier's own
// (always non-negative) placement, and to a fixed subset of the 5-6 loads for
// a retroactive one; the dropped loads' columns belong to a chunk whose own
// emit_flat overwrite already covers them (see the design comment below).
#define SNES_NTSC_VEC_CK(SLOT, V0, R) do {                                             \
    const snes_ntsc_rgb_t* base = PCK + (size_t)(SLOT) * (size_t)snes_ntsc_ck_words;    \
    if ((V0)+0 >= 0) A[(V0)+0] = snes_v4_add(A[(V0)+0], snes_v4_load_u(base + (3-(R)+0)));  \
    if ((V0)+1 >= 0) A[(V0)+1] = snes_v4_add(A[(V0)+1], snes_v4_load_u(base + (3-(R)+4)));  \
    if ((V0)+2 >= 0) A[(V0)+2] = snes_v4_add(A[(V0)+2], snes_v4_load_u(base + (3-(R)+8)));  \
    if ((V0)+3 >= 0) A[(V0)+3] = snes_v4_add(A[(V0)+3], snes_v4_load_u(base + (3-(R)+12))); \
    if ((V0)+4 >= 0) A[(V0)+4] = snes_v4_add(A[(V0)+4], snes_v4_load_u(base + (3-(R)+16))); \
    if ((R) >= 2 && (V0)+5 >= 0)                                                       \
        A[(V0)+5] = snes_v4_add(A[(V0)+5], snes_v4_load_u(base + (3-(R)+20)));         \
} while (0)

// Emit chunk N_'s (N_ = 0..3, this group's chunk position) 7 outputs as the
// FLAT constant for SLOT, unconditionally overwriting out_g+7*N_..+6. Stage 3
// review, change 1: plane_flat now stores the clamp+converted-with-Alevel=0
// word (see gen_slot()), so this is just 2 unaligned loads + 2 ORs (to fold
// in the real Alevel via the caller's precomputed alpha_vec) + 2 unaligned
// stores - no clamp, no convert. Needs PFLAT/out_g/alpha_vec from the caller;
// SLOT is the chunk's own classified slot (Stage 4: passed directly by the
// caller - chunk_slot[N_] - rather than looked up from a row-wide table).
#define SNES_NTSC_VEC_EMIT_FLAT(SLOT, N_) do {                                         \
    const snes_ntsc_rgb_t* flat_clamped = PFLAT + (size_t)(SLOT) * (size_t)snes_ntsc_flat_words; \
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

// One 6-pixel chunk at this group's local position N (0..3), fused,
// lookahead-free classification (Stage 4 - see snes_ntsc_blit_hires_vec's
// banner comment below for the full run_len/skipped/retro-add design). All
// decisions are made from this group's own 24 already-loaded slots
// (chunk_uniform[N]/chunk_slot[N]/run_len[N], computed by the caller just
// before calling this) plus a small state pair carried across chunks
// (skip1/skip1_slot, skip2/skip2_slot: whether/what the previous one or two
// chunks were, when skipped) - no row-wide pre-pass, no lookahead.
//
// R1V0,R1R / R2V0,R2R are this position's compile-time CK placement for a
// *retroactive* add of the immediately-preceding chunk (d=1) / the one before
// that (d=2) - see the design comment below for the rel=7*(c'-d) derivation;
// both may be negative (a retroactive target that fell in the previous
// group), which SNES_NTSC_VEC_CK's own bounds guards handle. OWN_V0,OWN_R is
// this position's normal (d=0) CK placement, same values Stage 3 used.
// K0/K2/K4 (+ALIGN/EV/OV/ER/OR) are unchanged from Stage 3 - this group's 3
// even/odd pixel pairs and their PK/general placement.
#define SNES_NTSC_VEC_CHUNK_N(N, OWN_V0, OWN_R, R1V0, R1R, R2V0, R2R,                   \
                             K0, K0A, EV0,ER0, OV0,OR0,                                 \
                             K2, K2A, EV2,ER2, OV2,OR2,                                 \
                             K4, K4A, EV4,ER4, OV4,OR4) do {                            \
    SNES_NTSC_STAT(g_stat_total);                                                      \
    /* Run-end retro-add: this chunk's run_len <= 1 means the previous run (if any)     \
       ended at N-1, so any of the last two chunks that were provisionally skipped      \
       (their contribution never accumulated) must be added now, before this chunk's    \
       own contribution - see the design comment's "why the retro add is exact". Once   \
       consumed here, skip1/skip2 must NOT also shift forward into skip2 for the next   \
       chunk below (their entire 19-word contribution was already placed in full by     \
       this add) - otherwise a long-since-resolved skip would be retro-added a second   \
       time at some later run end, double-counting it. */                              \
    const bool retro_N = (run_len[N] <= 1);                                            \
    if (retro_N) {                                                                     \
        if (skip1) SNES_NTSC_VEC_CK(skip1_slot, R1V0, R1R);                            \
        if (skip2) SNES_NTSC_VEC_CK(skip2_slot, R2V0, R2R);                            \
    }                                                                                  \
    if (flat3[N]) {                                                                    \
        /* Steady run (run_len[N] >= 3): not accumulated - its contribution           \
           (outputs 7N..7N+18) is either covered by this/the next 1-2 chunks' own      \
           emit_flat overwrite, or added back retroactively above when the run ends. */ \
        SNES_NTSC_STAT(g_stat_skip);                                                   \
    } else if ((SNES_NTSC_LADDER_MASK & 2) && chunk_uniform[N]) {                       \
        SNES_NTSC_STAT(g_stat_uniform);                                                \
        SNES_NTSC_VEC_CK(chunk_slot[N], OWN_V0, OWN_R);                                \
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
    /* Shift the carried state for the next chunk: N-1 -> N-2, N -> N-1 - unless a      \
       retro add just consumed the old skip1/skip2 above, in which case both are fully  \
       resolved and nothing carries forward (see the comment above retro_N). */         \
    if (retro_N) { skip2 = false; }                                                    \
    else         { skip2 = skip1; skip2_slot = skip1_slot; }                           \
    skip1 = flat3[N]; skip1_slot = chunk_slot[N];                                      \
} while (0)

// The 4 chunk positions within a group (N = 0..3), read directly off the
// per-pixel K table above: chunk N owns pixels 6N..6N+5, pairs
// (6N,6N+1)/(6N+2,6N+3)/(6N+4,6N+5). OWN_V0/OWN_R are the same CK placements
// Stage 3 used ("residues r = 0,3,2,1 for c' = 0,1,2,3, first vector
// v0 = 0,1,3,5"); R1/R2 are that same table evaluated at c'-1 and c'-2 (see
// the design comment below for why retroactively placing chunk c'-d uses
// exactly chunk (c'-d)'s own (v0,r) - shifted back a whole group, i.e. -7 on
// v0, whenever c'-d < 0).
#define SNES_NTSC_VEC_CHUNK0() SNES_NTSC_VEC_CHUNK_N(0, 0,0, -2,1, -4,2,  \
     0, 0, 0,0, 0,1,    2, 1, 0,2, 0,3,    4, 2, 1,0, 1,1)
#define SNES_NTSC_VEC_CHUNK1() SNES_NTSC_VEC_CHUNK_N(1, 1,3, 0,0, -2,1,  \
     6, 0, 1,3, 2,0,    8, 1, 2,1, 2,2,   10, 2, 2,3, 3,0)
#define SNES_NTSC_VEC_CHUNK2() SNES_NTSC_VEC_CHUNK_N(2, 3,2, 1,3, 0,0,  \
    12, 0, 3,2, 3,3,   14, 1, 4,0, 4,1,   16, 2, 4,2, 4,3)
#define SNES_NTSC_VEC_CHUNK3() SNES_NTSC_VEC_CHUNK_N(3, 5,1, 3,2, 1,3,  \
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

    for (; in_height; --in_height)
    {
        const snes_ntsc_rgb_t* restrict P     = (const snes_ntsc_rgb_t*)ntsc->plane[burst_phase];
        const snes_ntsc_rgb_t* restrict PPK   = (const snes_ntsc_rgb_t*)ntsc->plane_pk[burst_phase];
        const snes_ntsc_rgb_t* restrict PCK   = (const snes_ntsc_rgb_t*)ntsc->plane_ck[burst_phase];
        const snes_ntsc_rgb_t* restrict PFLAT = (const snes_ntsc_rgb_t*)ntsc->plane_flat[burst_phase];
        SNES_NTSC_IN_T const* restrict line_in = input;
        uint32_t* restrict line_out = (uint32_t*)rgb_out;

        // Part C classifier state, carried across chunks in processing order for the
        // whole row (see the fused-classifier design comment above
        // SNES_NTSC_VEC_CHUNK_N): run_len_carry/slot_carry are run_len(C-1) and its
        // chunk's slot (slot_carry meaningful only when run_len_carry > 0); skip1/
        // skip1_slot and skip2/skip2_slot record whether chunks C-1 and C-2 were
        // skipped (run_len >= 3) and, if so, their shared slot, for a possible retro
        // add. All reset at the start of every row, per the design comment's "the
        // prologue is never classified, group 0 just starts with run_len = 0".
        int run_len_carry = 0; uint32_t slot_carry = 0;
        bool skip1 = false; uint32_t skip1_slot = 0;
        bool skip2 = false; uint32_t skip2_slot = 0;

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

            uint32_t slotv[24];
            SNES_NTSC_IN_T const* restrict p = line_in + 2 + g * 24;
            for (int k = 0; k < 24; ++k) slotv[k] = (uint32_t)p[k];

            // Classify this group's 4 chunks from the slots just loaded (see the
            // fused-classifier design comment above): branch-free uniform test,
            // then run_len chained from the carried-in state through this group's
            // own 4 chunks. Cheap scalar bookkeeping only - no table, no lookahead.
            uint32_t chunk_slot[4]; bool chunk_uniform[4]; int run_len[4]; bool flat3[4];
            for (int n = 0; n < 4; ++n) {
                uint32_t s0 = slotv[6*n];
                uint32_t u = (slotv[6*n+1]^s0) | (slotv[6*n+2]^s0) | (slotv[6*n+3]^s0) |
                             (slotv[6*n+4]^s0) | (slotv[6*n+5]^s0);
                chunk_uniform[n] = (u == 0);
                chunk_slot[n] = s0;
                uint32_t prev_slot = (n == 0) ? slot_carry    : chunk_slot[n-1];
                int      prev_len  = (n == 0) ? run_len_carry : run_len[n-1];
                if (!chunk_uniform[n])                              run_len[n] = 0;
                else if (prev_len > 0 && chunk_slot[n] == prev_slot) run_len[n] = prev_len + 1;
                else                                                 run_len[n] = 1;
                flat3[n] = ((SNES_NTSC_LADDER_MASK) & 1) && (run_len[n] >= 3);
            }
            run_len_carry = run_len[3];
            slot_carry = chunk_slot[3];

            if (flat3[0] && flat3[1] && flat3[2] && flat3[3]) {
                // All-skip group fast path (see the design comment above): no chunk
                // here can have a retro add pending (that only fires at run_len <= 1),
                // so no accumulation is needed at all - just the 4 FLAT emits.
                SNES_NTSC_STAT(g_stat_total); SNES_NTSC_STAT(g_stat_total);
                SNES_NTSC_STAT(g_stat_total); SNES_NTSC_STAT(g_stat_total);
                SNES_NTSC_STAT(g_stat_skip); SNES_NTSC_STAT(g_stat_skip);
                SNES_NTSC_STAT(g_stat_skip); SNES_NTSC_STAT(g_stat_skip);
                SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[0], 0);
                SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[1], 1);
                SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[2], 2);
                SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[3], 3);
                for (int i = 0; i < 10; ++i) A[i] = snes_v4_zero();
                skip2 = flat3[2]; skip2_slot = chunk_slot[2];
                skip1 = flat3[3]; skip1_slot = chunk_slot[3];
                continue;
            }

            SNES_NTSC_VEC_CHUNK0();
            SNES_NTSC_VEC_CHUNK1();
            SNES_NTSC_VEC_CHUNK2();
            SNES_NTSC_VEC_CHUNK3();

            {
                const bool f0 = flat3[0], f1 = flat3[1], f2 = flat3[2], f3 = flat3[3];
                const bool covered[7] = { f0, f0 && f1, f1, f1 && f2, f2, f2 && f3, f3 };
                for (int v = 0; v < 7; ++v) {
                    if (covered[v]) continue;
                    A[v] = snes_ntsc_simd_clamp_convert(A[v], clamp_mask_vec, clamp_add_vec, alpha_vec);
                    snes_v4_store_a(out_g + v * 4, A[v]);
                }

                // FLAT overwrite pass: any of this group's 4 chunks whose emit_flat
                // holds gets its 7 outputs (7n..7n+6, always inside this group's
                // own 28) unconditionally replaced with the already clamp+converted
                // FLAT constant for its slot (see gen_slot() and
                // SNES_NTSC_VEC_EMIT_FLAT) - correct regardless of whether that
                // chunk's own accumulation above ran skip/CK/PK/K, or whether the
                // vector it lives in was even stored above (see the coverage mask
                // just computed).
                if (f0) SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[0], 0);
                if (f1) SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[1], 1);
                if (f2) SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[2], 2);
                if (f3) SNES_NTSC_VEC_EMIT_FLAT(chunk_slot[3], 3);
            }

            A[0] = A[7]; A[1] = A[8]; A[2] = A[9];
            A[3] = A[4] = A[5] = A[6] = A[7] = A[8] = A[9] = snes_v4_zero();
        }

        // Tail group G: the last 14 real pixels (j = W-14..W-1) then black
        // (j = W..W+9), reproducing _fast's end-of-row handling. Only A[0..4]
        // (outputs 28G..28G+19) are real sums; the last 4 outputs of the row
        // get the precomputed fixed_tail value rather than a 6th accumulator
        // _fast never computes. The tail keeps the unmodified Stage-2 general
        // kernel for its own accumulation (see the Part C banner comment
        // above), but it is still owed the classifier's run-end bookkeeping:
        // the last main group's last two chunks (4G-1, 4G-2) may have been
        // provisionally skipped with no later chunk ever triggering their
        // retro add (the row simply ends first) - so, unconditionally, before
        // accumulating anything, retro-add each one that was skipped, at the
        // same (v0,r) a chunk at this group's position 0 would use for d=1/d=2
        // (rel=-7/-14) - identical to SNES_NTSC_VEC_CHUNK0's own R1V0,R1R/
        // R2V0,R2R. Bounds-guarded exactly as any other retro add: only the
        // parts landing in this (fresh, zeroed-into-seeded) A[] are added: the
        // rest were already covered by that chunk's own emit_flat overwrite
        // when the last main group was finalized.
        {
            if (skip1) SNES_NTSC_VEC_CK(skip1_slot, -2, 1);
            if (skip2) SNES_NTSC_VEC_CK(skip2_slot, -4, 2);

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
#undef SNES_NTSC_VEC_CHUNK_N
#undef SNES_NTSC_VEC_CHUNK0
#undef SNES_NTSC_VEC_CHUNK1
#undef SNES_NTSC_VEC_CHUNK2
#undef SNES_NTSC_VEC_CHUNK3
#undef SNES_NTSC_STAT

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
