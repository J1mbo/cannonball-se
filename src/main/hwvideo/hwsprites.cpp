#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwarch.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iterator>

/***************************************************************************
    Video Emulation: OutRun Sprite Rendering Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/* This version is tuned for CannonBall-SE, modifications Copyright (c) 2025, James Pearce
   Approx. 40% speed-up on Pi2 */

/*******************************************************************************************
*  Out Run/X-Board-style sprites
*
*      Offs  Bits               Usage
*       +0   e------- --------  Signify end of sprite list
*       +0   -h-h---- --------  Hide this sprite if either bit is set
*       +0   --c----- --------  JJP - clip required (partially off-screen)
*       +0   ----bbb- --------  Sprite bank
*       +0   -------t tttttttt  Top scanline of sprite + 256
*       +2   oooooooo oooooooo  Offset within selected sprite bank
*       +4   ppppppp- --------  Signed 7-bit pitch value between scanlines
*       +4   -------x xxxxxxxx  X position of sprite (position $BE is screen position 0)
*       +6   -s------ --------  Enable shadows
*       +6   --pp---- --------  Sprite priority, relative to tilemaps
*       +6   ----vvvv vvvvvvvv  Zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size) - JJP - this is wrong, 0x100 is 2x size
*       +8   y------- --------  Render from top-to-bottom (1) or bottom-to-top (0) on screen
*       +8   -f------ --------  Horizontal flip: read the data backwards if set
*       +8   --x----- --------  Render from left-to-right (1) or right-to-left (0) on screen
//*       +8   ------hh hhhhhhhh  Horizontal zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +8   ------ww wwwwwwww  Sprite width on-screen (in native res)
*       +E   dddddddd dddddddd  Scratch space for current address
*
*  Out Run only:
*       +A   hhhhhhhh --------  Height in scanlines - 1
*       +A   -------- -ccccccc  Sprite color palette
*
*  X-Board only:
*       +A   ----hhhh hhhhhhhh  Height in scanlines - 1
*       +C   -------- cccccccc  Sprite color palette
*
*  Final bitmap format:
*
*            -s------ --------  Shadow control
*            --pp---- --------  Sprite priority
*            ----cccc cccc----  Sprite color palette
*            -------- ----llll  4-bit pixel data
*
 *******************************************************************************************/

// SPRITE_IMPLEMENTATION_SPEC.md S0.2: the CMake option PIXEL_ACCURACY
// (CMakeLists.txt, default ON => -DPIXEL_ACCURACY=1) used to have no effect
// on this file at all: an unconditional "#define PIXEL_ACCURACY 0" used to
// sit here and always override it before the macro table further down was
// ever reached, so hardware pixel accuracy (glow) has always shipped
// disabled regardless of the CMake setting. Fixed to a default that a
// caller-supplied definition can still override. The macro table used by
// render()'s dispatch switch below no longer depends on this value at all
// (see the note there), so render()'s compiled behaviour is unchanged by
// this fix; only the initial value of the new runtime `glow` member (S0.5,
// not yet consulted by render() itself) is affected.
#ifndef PIXEL_ACCURACY
#define PIXEL_ACCURACY 1
#endif

hwsprites::hwsprites() : glow(PIXEL_ACCURACY != 0)
{
    // SPRITE_IMPLEMENTATION_SPEC.md S3.2: default shadow_rows to all-ones
    // (the "not provably clean" state, disabling the filter) so any caller
    // that drives decode_row/replay/draw_sprite_fast directly -- the S1/S2
    // unit tests, and S2's own draw_sprite_fast(glow=true) target chief
    // among them -- sees the same unfiltered push behaviour as before S3
    // without having to call render() first. render() itself overwrites the
    // whole array at the top of every frame regardless.
    std::fill_n(shadow_rows, std::size(shadow_rows), ~0ull);
}

hwsprites::~hwsprites()
{
}


void hwsprites::init(const uint8_t* src_sprites)
{
    reset();
    if (src_sprites)
    {
        // Convert S16 tiles to a more useable format
        const uint8_t *spr = src_sprites;

        for (uint32_t i = 0; i < SPRITES_LENGTH; i++)
        {
            uint8_t d3 = *spr++;
            uint8_t d2 = *spr++;
            uint8_t d1 = *spr++;
            uint8_t d0 = *spr++;

            // Forward (just endian swap of bytes, keep pixel order p0..p7)
            sprites[i] = ((uint32_t)d0 << 24) |
                         ((uint32_t)d1 << 16) |
                         ((uint32_t)d2 <<  8) |
                         ((uint32_t)d3 <<  0);
        }
    }
    // SPRITE_IMPLEMENTATION_SPEC.md S1.2 (FABLE_ASSESSMENT.md 6): x1/x2 had
    // no in-class initialiser and no caller was guaranteed to set them
    // before the first render(); the real game calls Video::set_video_mode
    // (which sets config.s16_width) before sprite_layer->init() (video.cpp),
    // so config.s16_width is already valid here -- default to the full-width
    // clip window so x1/x2 are never read uninitialised.
    set_x_clip(false);
}


void hwsprites::reset()
{
    // SPRITE_IMPLEMENTATION_SPEC.md S1.2: "reset clears only ram/ramBuff."
    // The lazy sprites_flipped/sprites_shadowinfo side tables the shipped
    // renderer used to reset here are gone (S1 drops them from the game
    // build); draw_sprite_fast/draw_sprite_px carry no state that
    // reset()/init() needs to clear.
    std::fill_n(ram,     std::size(ram),     0x0);
    std::fill_n(ramBuff, std::size(ramBuff), 0x0);
}

// Clip areas of the screen in wide-screen mode
void hwsprites::set_x_clip(bool on)
{
    // Clip to central 320 width window.
    if (on)
    {
        x1 = config.s16_x_off;
        x2 = x1 + S16_WIDTH;

        if (config.video.hires)
        {
            x1 <<= 1;
            x2 <<= 1;
        }
    }
    // Allow full wide-screen.
    else
    {
        x1 = 0;
        x2 = config.s16_width;
    }
}

uint8_t hwsprites::read(const uint16_t adr)
{
    uint16_t a = adr >> 1;
    if ((adr & 1) == 1)
        return ram[a] & 0xff;
    else
        return ram[a] >> 8;
}

void hwsprites::write(const uint16_t adr, const uint16_t data)
{
    ram[adr >> 1] = data;
}

// Copy back buffer to main ram, ready for blit
void hwsprites::swap()
{
    uint16_t *src = (uint16_t *)ram;
    uint16_t *dst = (uint16_t *)ramBuff;

    // swap the halves of the road RAM
    for (uint16_t i = 0; i < SPRITE_RAM_SIZE; i++)
    {
        uint16_t temp = *src;
        *src++ = *dst;
        *dst++ = temp;
    }
}

// S0.5: unconditional test/inspection accessor. Copies exactly what
// hwsprites::swap() leaves ramBuff holding (SPRITE_RAM_SIZE == 0x800
// halfwords) -- used by the driver's `replay` to load a captured frame's
// sprite RAM without needing friend access.
void hwsprites::load_sprite_ram(const uint16_t* buf)
{
    std::memcpy(ramBuff, buf, sizeof(ramBuff));
}


// -----------------------------------------------------------------------------------------
// SPRITE_IMPLEMENTATION_SPEC.md S0.2: this table used to be split
// "#if PIXEL_ACCURACY / #else" between a single glow-capable macro
// (draw_pixel(), which -- grep confirms -- was never actually invoked
// anywhere in render()'s dispatch switch below) and the twelve macros the
// switch really uses. Since only the twelve are ever called, that split made
// PIXEL_ACCURACY==1 fail to compile (draw_pixel_1row() etc. would not exist)
// -- collateral damage of the PIXEL_ACCURACY fix above, now corrected. The
// twelve macros are unconditional, with byte-identical bodies to what
// render() has always compiled against, so render()'s output is unchanged
// for any value of PIXEL_ACCURACY. draw_pixel() itself (the dead glow macro)
// is not carried forward; the hardware glow behaviour it once sketched --
// "a sprite drawn on top of a shadow clears the shadow flag for its opaque
// pixels, offset by one pixel horizontally" -- is now the definition
// implemented and tested by draw_sprite_px's glow branch below (S0.4).
// -----------------------------------------------------------------------------------------

// MACROS WITH SHADOW SUPPORT

#define IF_IS_DRAWABLE_()        if (x >= x1 && x < x2 && pix != 0 && pix != 15)
#define IF_IS_DRAWABLE()         if (((unsigned)(pix - 1u) < 14u) && ((unsigned)(x - x1) < span))
#define IS_DRAWABLE()               (((unsigned)(pix - 1u) < 14u) && ((unsigned)(x - x1) < span))
#define IF_IS_DRAWABLE_NO_CLIP() if  ((unsigned)(pix - 1u) < 14u)
#define IS_DRAWABLE_NO_CLIP()        ((unsigned)(pix - 1u) < 14u)

// SPRITE_IMPLEMENTATION_SPEC.md S1.2: the twelve draw_pixel_*() macros that
// used to live here (shadow/no-shadow x clipped/unclipped x 1/2/3-row) were
// the shipped render()'s per-pixel accumulator body; render() below is now
// the S1 LINEBUF rewrite and does not use them. The five IF_IS_DRAWABLE*/
// IS_DRAWABLE* helper macros immediately above are kept: render_ref's own
// "_ref"-suffixed draw_pixel_*_ref() macros (CB_RENDER_TEST, further down
// this file) still call them unmodified, so the twin's copy of the shipped
// algorithm continues to compile and behave exactly as before.

void hwsprites::render(uint16_t* pixels, uint8_t priority, bool dest_clean)
{
    // SPRITE_IMPLEMENTATION_SPEC.md S3.2: reset the per-row shadow-seen
    // bitmap for this frame. All-zero only when the caller proves bit 12 is
    // already clear on every row before the sprite pass starts (dest_clean,
    // video.cpp's fg_enabled); otherwise all-ones, so every row reads as
    // "already touched" and replay()'s glow-push filter below never skips
    // anything (the conservative default whenever a stale bit 12 from an
    // earlier frame cannot be ruled out).
    std::memset(shadow_rows, dest_clean ? 0x00 : 0xff, sizeof(shadow_rows));

    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S1.2: dispatch. Each engine-emitted
    // record either fits draw_sprite_fast's envelope (the closed-form
    // LINEBUF core: decode each visible source row once, replay it onto
    // every destination row that shares it) or falls back to the pixel-
    // serial oracle render_px, which is unconditionally correct for any
    // record (S0.4) -- ydelta == -1 (bottom-to-top; real ROM data never
    // emits this, set_hrender always forces the top-to-bottom bit),
    // zoom < SPR_FAST_ZOOM_MIN (below every shipping zoom table), or
    // pend > SPR_PEND_MAX (wider than the line buffer's margin allows).
    // pend <= 0 draws nothing at all (every row starts at xpos).
    // ---------------------------------------------------------------------
    for (uint16_t d = 0; d < SPRITE_RAM_SIZE; d += 16)
    {
        if ((ramBuff[d + 0] & 0x8000) != 0) break; // end of sprite list

        SpriteRec rec;
        if (!parse_record(d, priority, rec)) continue;

        const int32_t pend = x2 - rec.xpos;
        if (pend <= 0) continue;

        if (rec.ydelta == -1 || rec.zoom < SPR_FAST_ZOOM_MIN || pend > SPR_PEND_MAX) {
#ifdef CB_SPRITE_STATS
            stat_fallback++;
#endif
            draw_sprite_px(rec, pixels, glow);
        } else {
            draw_sprite_fast(rec, pixels, pend);
        }
    }
}

// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S1.2: LINEBUF core -- schedule table,
// decode-once, replay-many (both replay strategies D/D'), ROM prefetch.
// Scalar/portable only (S1; NEON replay/decode land in S4). Nothing here
// keeps state across sprites: line[]/rec[]/nrec are scratch reused per row,
// sched_tab is a pure cache keyed by zoom.
// =============================================================================================

// sched(zoom): builds (on first use) and caches run[SPR_SCHED_LEN] for this
// (already post-halving) zoom -- one run length per nibble position, by the
// same reference accumulator the shipped renderer's row loop used
// (verbatim recurrence): "xacc = 0; for i: r = 0; while (xacc < 0x200) {
// r++; xacc += zoom; } xacc -= 0x200; run[i] = r;". This is the
// division-free closed form of SCHEDULE_VERIFICATION.md's scaling law:
// run[i] == ceil(512*(i+1)/zoom) - ceil(512*i/zoom) (proved by unit s1a).
const uint8_t* hwsprites::sched(uint32_t zoom)
{
    std::unique_ptr<uint8_t[]>& slot = sched_tab[zoom & 0xfff];
    if (!slot) {
        slot.reset(new uint8_t[SPR_SCHED_LEN]);
        uint32_t xacc = 0;
        for (int32_t i = 0; i < SPR_SCHED_LEN; i++) {
            int32_t r = 0;
            while (xacc < 0x200) { r++; xacc += zoom; }
            xacc -= 0x200;
            assert(zoom < (uint32_t)SPR_FAST_ZOOM_MIN || r <= 4);
            slot[i] = (uint8_t)r;
        }
    }
    return slot.get();
}

// emit(type, pos): appends a span-type change at output position pos to
// rec[]/nrec, applying the two rules that keep records the maximal runs of
// one type (S2's glow depends on this): a zero-length record -- a run-0
// pixel landed exactly on the previous boundary -- is dropped by
// decrementing nrec first; a record whose type equals the (possibly
// just-uncovered) previous record's type is a no-op merge.
void hwsprites::emit(uint8_t type, int32_t pos)
{
    if (nrec > 0 && rec[nrec - 1].pos == (uint16_t)pos)
        nrec--;
    if (nrec > 0 && rec[nrec - 1].type == type)
        return;
    rec[nrec].pos = (uint16_t)pos;
    rec[nrec].type = type;
    rec[nrec].flags = 0;
    nrec++;
}

// decode_row: expands source row j of sprite r into line[] (output-order
// n|color halfwords, always written four-at-a-time regardless of this
// nibble's own run length -- safe because any overrun lands on positions
// the next source pixel overwrites, or beyond the sentinel where replay
// never reads, per SPR_LB_LEN's margin) and into rec[]/nrec (the maximal-run
// span records). Reads each ROM word once, mirrors flipped rows on the fly
// with nibrev() (no sprites_flipped side table) and classifies shadow per
// nibble during decode (no sprites_shadowinfo side table).
void hwsprites::decode_row(const SpriteRec& r, int32_t j, int32_t pend)
{
    int32_t pos = 0;
    int32_t words = 0;
    nrec = 0;
    uint8_t prev = SPR_GAP;
    const uint8_t* run = sched((uint32_t)r.zoom);
    const int64_t rowbase = (int64_t)r.bank * 0x10000 + r.addr + (int64_t)r.pitch * j;

    for (;;) {
        if (pos >= pend || words == SPR_WMAX) break;
        const int64_t widx = r.flip ? (rowbase + r.pitch - 1 - words) : (rowbase + words);
        if (widx < 0 || widx >= (int64_t)SPRITES_LENGTH) break;

        uint32_t w = sprites[widx];
        if (r.flip) w = nibrev(w);

        for (int k = 0; k < 8; k++) {
            const unsigned n = (w >> (28 - 4 * k)) & 0xF;
            const uint8_t t = (n == 0 || n == 15) ? SPR_GAP
                             : ((r.shadow && n == 10) ? SPR_SHADOW : SPR_COLOUR);
            if (t != prev) { emit(t, pos); prev = t; }

            const uint16_t v = (uint16_t)(n | r.color);
            const uint64_t fill = (uint64_t)v * 0x0001000100010001ULL;
            std::memcpy(line + pos, &fill, 8);

            pos += run[words * 8 + k];
        }

        words++;
        if ((w & 0xF0) == 0xF0) break; // terminator, tested on the (mirrored) word after it is fully drawn
    }
    emit(SPR_GAP, pos); // sentinel; guarantees the last record is a GAP
}

// prefetch_rom_row: read-prefetch the two ROM words decode_row will touch
// first and last for source row jrow ("the words at k = 0 and k = pitch -
// 1"), mirrored for flip so the two hinted addresses are always the row's
// actual first/last read regardless of direction. Bounds-checked; a row
// that will never be decoded, or whose hinted words fall outside the ROM
// image, simply prefetches nothing.
void hwsprites::prefetch_rom_row(const SpriteRec& r, int32_t jrow) const
{
    if (jrow < 0) return;
    const int64_t rowbase = (int64_t)r.bank * 0x10000 + r.addr + (int64_t)r.pitch * jrow;
    const int64_t w_first = r.flip ? (rowbase + r.pitch - 1) : rowbase;
    const int64_t w_last  = r.flip ? rowbase : (rowbase + r.pitch - 1);
    if (w_first >= 0 && w_first < (int64_t)SPRITES_LENGTH) cb_prefetch_r(&sprites[w_first]);
    if (w_last  >= 0 && w_last  < (int64_t)SPRITES_LENGTH) cb_prefetch_r(&sprites[w_last]);
}

// replay: writes the just-decoded row's records onto destination rows
// [da, db) of sprite r. COLOUR spans are store-only -- D copies the whole
// span once per row (memcpy); D' loads each 4-halfword chunk of the span
// exactly once and stores it to every destination row, with a scalar
// halfword tail -- the destination is never read by either. SHADOW spans
// are an inline read-modify-write, one pixel at a time, and are never
// deferred or batched across rows. Both modes write exactly [lo, hi) and
// nothing outside it; replay_mode just selects between them (measured, not
// verified separately -- S1.5 picks the shipped default).
void hwsprites::replay(const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1)
{
#ifdef CB_RENDER_TEST
    // S1.4 negative control only (never set by the game, never true unless
    // the driver's own negative-control test sets it): widen the right edge
    // of the clip window past x2 by a fixed margin, so replay draws some of
    // the pixels decode_row already decoded (it overshoots pend by up to
    // ~34, S1.2) but the real clip test would have discarded -- an F2-scale
    // overrun (a handful of pixels past the sprite's true edge) rather than
    // an unbounded one. This is deliberately NOT "cx0 = 0, cx1 = SPR_LB_LEN"
    // (disable clipping outright): that widens by up to ~4096 pixels per
    // row, which reliably walks clean off the end of the pixel allocation
    // and corrupts the heap allocator's own bookkeeping (observed here as
    // an unconditional abort on the next operator delete/new, before the
    // driver's memcmp -- and therefore its PASS/FAIL report -- ever runs).
    // A bounded overrun still lands outside the sprite's clipped extent
    // (into the pad, the spare rows, or a neighbouring row/sprite's pixels)
    // often enough for fuzz's whole-allocation memcmp to reliably catch it,
    // which is the property this control exists to demonstrate, without
    // crashing the harness before it can report that.
    if (debug_disable_clip) { cx1 += 32; }
#endif

    const int32_t scrn_width = (int32_t)config.s16_width;
    uint16_t* const base = pixels + (size_t)r.top * scrn_width + r.xpos;

    // SPRITE_IMPLEMENTATION_SPEC.md S3.2: shadow-destination prefetch, up to
    // SPR_SHADOW_PF_ROWS destination-row-groups' worth ahead of this one, at
    // this row's own SHADOW spans' x-range -- consecutive source rows lie at
    // nearly the same x (DESIGN_REVIEW_BANDWIDTH.md section 4, amendment 2:
    // a plain one-row-ahead prefetch undersizes a 200-250 cycle miss under
    // bus contention; two rows' worth of decode time covers it). Issued
    // before any of this row's own RMW work below, for the longest possible
    // head start; touches nothing (cb_prefetch_w only, PRFM PSTL1KEEP /
    // __builtin_prefetch(p,1,3), a no-op on MSVC), and is bounds-checked
    // against d_hi so it never names a row past the sprite's own extent.
    {
        const int32_t d_hi = std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top);
        const int32_t pf_hi = std::min<int32_t>(db + SPR_SHADOW_PF_ROWS * (db - da), d_hi);
        if (pf_hi > db) {
            for (int32_t k = 0; k + 1 < nrec; k++) {
                if (rec[k].type != SPR_SHADOW) continue;
                const int32_t lo = std::max<int32_t>(rec[k].pos, cx0);
                const int32_t hi = std::min<int32_t>(rec[k + 1].pos, cx1);
                if (hi <= lo) continue;
                for (int32_t d = db; d < pf_hi; d++) {
                    uint16_t* const rowp = base + (size_t)d * scrn_width;
                    uintptr_t p = (uintptr_t)(rowp + lo) & ~(uintptr_t)63;
                    const uintptr_t p_last = (uintptr_t)(rowp + hi - 1);
                    for (; p <= p_last; p += 64)
                        cb_prefetch_w((const void*)p);
                }
            }
        }
    }

    for (int32_t k = 0; k + 1 < nrec; k++) {
        if (rec[k].type == SPR_GAP) continue;

        const int32_t lo = std::max<int32_t>(rec[k].pos, cx0);
        const int32_t hi = std::min<int32_t>(rec[k + 1].pos, cx1);
        if (hi <= lo) continue;

        if (rec[k].type == SPR_COLOUR) {
            if (replay_mode == 0) {
                // D: span-copy, one memcpy per destination row.
                for (int32_t d = da; d < db; d++)
                    std::memcpy(base + (size_t)d * scrn_width + lo, line + lo,
                                (size_t)(hi - lo) * sizeof(uint16_t));
            } else {
                // D': load each chunk once, store it to every destination row.
                int32_t p = lo;
                for (; p + 4 <= hi; p += 4) {
                    uint64_t chunk;
                    std::memcpy(&chunk, line + p, 8);
                    for (int32_t d = da; d < db; d++)
                        std::memcpy(base + (size_t)d * scrn_width + p, &chunk, 8);
                }
                for (; p < hi; p++) {
                    const uint16_t v = line[p];
                    for (int32_t d = da; d < db; d++)
                        (base + (size_t)d * scrn_width)[p] = v;
                }
            }

            // SPRITE_IMPLEMENTATION_SPEC.md S2.2 (DESIGN_GLOW.md section 1):
            // one deferred bit-12 clear at the left neighbour of this run's
            // first drawn pixel, per destination row, iff that neighbour is
            // inside the clip window (lo > cx0, i.e. x(lo) > x1). Records
            // are maximal runs (emit()'s drop/merge rule, S1.2) walked left
            // to right, so rec[k-1] -- when non-empty -- is always the
            // screen pixel at lo-1; DESIGN_GLOW.md section 2's amendment
            // suppresses the clear for a right-to-left sprite when that
            // neighbour is a SHADOW run of this same sprite row (the
            // hardware clears before it ORs in that case, not after). This
            // is the only destination read the colour path adds, and it
            // happens later, at the flush -- the copy above never reads the
            // destination.
            if (glow && lo > cx0) {
                const bool suppress = r.rtl && k > 0 && rec[k - 1].type == SPR_SHADOW;
                uint16_t* const col = base + (lo - 1);
                for (int32_t d = da; d < db; d++) {
                    if (suppress) {
#ifdef CB_SPRITE_STATS
                        stat_glow_suppressed_rtl++;
#endif
                    } else {
                        // SPRITE_IMPLEMENTATION_SPEC.md S3.2: bit 12 can only
                        // ever have been set by a shadow OR (replay's SHADOW
                        // branch below, or an all-ones bitmap when the frame
                        // is not provably clean), so a clear on a row whose
                        // shadow_rows bit is still 0 is a provable no-op --
                        // skip the push (and its eventual destination load
                        // at the flush) entirely.
                        const int32_t y = r.top + d;
                        if ((shadow_rows[y >> 6] >> (y & 63)) & 1ull) {
                            glow_push(col + (size_t)d * scrn_width);
                        }
#ifdef CB_SPRITE_STATS
                        else {
                            stat_shadow_bitmap_filtered++;
                        }
#endif
                    }
                }
            }
        } else { // SPR_SHADOW: inline read-modify-write, never deferred, never batched across rows
            for (int32_t d = da; d < db; d++) {
                // SPRITE_IMPLEMENTATION_SPEC.md S3.2: mark this destination
                // row as shadow-touched before the RMW below, so any glow
                // push depending on it (this row, this sprite, or a later
                // one) sees the bit set. Setting it here, before the OR,
                // guarantees this for the same row (the only ordering S3.3
                // requires).
                const int32_t y = r.top + d;
                shadow_rows[y >> 6] |= 1ull << (y & 63);
                uint16_t* rowp = base + (size_t)d * scrn_width;
                for (int32_t p = lo; p < hi; p++)
                    rowp[p] |= S16_PALETTE_ENTRIES;
            }
        }
    }
}

// glow_push/glow_flush (S2.2): the per-sprite deferred glow-clear ring.
// Pushing when the ring is already full flushes it first (never grows past
// SPR_GLOW_RING); draw_sprite_fast flushes once more, unconditionally, after
// the sprite's last row, so a partial ring is never carried into the next
// sprite (DESIGN_GLOW.md section 3: deferring across sprites is unsafe, a
// later sprite's shadow OR to the same address must never be undone by an
// earlier sprite's clear -- deferring no further than sprite end guarantees
// it, since no address is written twice within one sprite).
void hwsprites::glow_push(uint16_t* p)
{
    if (glow_n == SPR_GLOW_RING) {
#ifdef CB_SPRITE_STATS
        stat_glow_flush_full++;
#endif
        glow_flush();
    }
    glow_ring[glow_n++] = p;
#ifdef CB_SPRITE_STATS
    stat_glow_pushed++;
#endif
}

void hwsprites::glow_flush()
{
    for (int32_t i = 0; i < glow_n; i++) {
#ifdef CB_SPRITE_STATS
        if (*glow_ring[i] & S16_PALETTE_ENTRIES) stat_glow_changed++;
#endif
        *glow_ring[i] &= 0xfff; // matches draw_sprite_px's glow branch exactly
    }
    glow_n = 0;
}

// draw_sprite_fast: the closed-form LINEBUF vertical loop. Every destination
// row d in [d_lo, d_hi) reads source row src(d) = (d*zoom) >> 9
// (division-free once zoom is fixed, exactly the shipped accumulator's
// result -- unit s1a); consecutive d's sharing a source row are grouped so
// that row is decoded exactly once and replayed onto every row in the
// group. render()'s dispatch guarantees r.ydelta == 1 here (ydelta == -1
// routes to draw_sprite_px), so y = r.top + d and d_lo/d_hi need only clip
// to [0, s16_height).
void hwsprites::draw_sprite_fast(const SpriteRec& r, uint16_t* pixels, int32_t pend)
{
    const int32_t d_lo = std::max<int32_t>(0, -r.top);
    const int32_t d_hi = std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top);
    if (d_lo >= d_hi) return;

    const int32_t cx0 = (int32_t)x1 - r.xpos;
    const int32_t cx1 = pend;

    // Initial ROM prefetch: the first two source rows this sprite will read.
    {
        const int32_t j0 = (int32_t)(((int64_t)d_lo * r.zoom) >> 9);
        prefetch_rom_row(r, j0);
        prefetch_rom_row(r, j0 + 1);
    }

    int32_t d = d_lo;
    while (d < d_hi) {
        const int32_t j = (int32_t)(((int64_t)d * r.zoom) >> 9);
        const int32_t da = d;
        d++;
        while (d < d_hi && (int32_t)(((int64_t)d * r.zoom) >> 9) == j) d++;
        const int32_t db = d;

        prefetch_rom_row(r, j + SPR_ROM_PF_ROWS);

        decode_row(r, j, pend);
        replay(r, pixels, da, db, cx0, cx1);
    }

    // SPRITE_IMPLEMENTATION_SPEC.md S2.2: flush the glow ring here, once per
    // sprite, never per frame (DESIGN_GLOW.md section 3). A no-op when glow
    // is false or this sprite pushed nothing.
    glow_flush();
}


// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S0.4: nibrev, parse_record, draw_sprite_px, render_px.
//
// Compiled always (S0.4): render_px is also the in-game fallback for
// out-of-envelope sprites from S1.2 onward, so it (and everything it needs)
// lives here unconditionally rather than under CB_RENDER_TEST. It is a
// straightforward, obviously-correct reimplementation that draws one output
// pixel at a time from the source using the documented S16 semantics --
// deliberately not sharing any code or lazy state with render() above, so it
// serves as an independent correctness oracle: glow=false reproduces the
// shipped output modulo F1/F2 (SPRITE_IMPLEMENTATION_SPEC.md preamble items
// 1 and 6); glow=true is the PIXEL_ACCURACY==1 hardware-accurate output.
// =============================================================================================

uint32_t hwsprites::nibrev(uint32_t w)
{
    return cb_bswap32(((w >> 4) & 0x0F0F0F0Fu) | ((w << 4) & 0xF0F0F0F0u));
}

// Verbatim (S0.4) from hwsprites.cpp:425-462,468-469,476-487,509-519,601-611
// of the shipped render() above, re-expressed as a standalone parse with no
// side effects on the lazy tables. Returns false to skip this record (wrong
// priority, hidden, or zoom == 0 -- the shipped renderer would loop forever
// on zoom == 0). The end-of-list test (ramBuff[d+0] & 0x8000) is the
// caller's job, exactly as render()'s own dispatch loop does it.
bool hwsprites::parse_record(uint16_t d, uint8_t priority, SpriteRec& r) const
{
    r.sprpri = 1u << ((ramBuff[d+3] >> 12) & 3);
    if (r.sprpri != priority) return false;

    int16_t hide = (int16_t)(ramBuff[d+0] & 0x5000);
    r.height = (ramBuff[d+5] >> 8) + 1;
    if (hide != 0) return false;

    // bank is 3 bits (0-7); SPRITES_LENGTH is always exactly 4 banks of
    // 0x10000, so "% 4" (SPRITE_IMPLEMENTATION_SPEC.md S0.4) and
    // "% (SPRITES_LENGTH / 0x10000)" (render()'s numbanks) are the same
    // operation -- spelled via the class constant so the two can never drift.
    r.bank   = ((ramBuff[d+0] >> 9) & 7) % (SPRITES_LENGTH / 0x10000);
    r.top    = (int32_t)(ramBuff[d+0] & 0x1ff) - 0x100;
    r.clip   = (ramBuff[d+0] & 0x2000) != 0;
    r.addr   = (int32_t)ramBuff[d+1];
    r.pitch  = ((ramBuff[d+2] >> 1) | ((ramBuff[d+4] & 0x1000) << 3)) >> 8;
    r.shadow = (ramBuff[d+3] >> 14) & 1;
    r.zoom   = ramBuff[d+3] & 0xfff;
    r.ydelta = (ramBuff[d+4] & 0x8000) ? 1 : -1;
    r.flip   = (~ramBuff[d+4] >> 14) & 1;
    r.xdelta = (ramBuff[d+4] & 0x2000) ? 1 : -1;
    r.width  = ramBuff[d+4] & 0x7ff;
    r.color  = 0x800 + ((ramBuff[d+5] & 0x7f) << 4);
    r.xpos   = ramBuff[d+6];
    r.offset = (int16_t)ramBuff[d+15];

    if (r.xpos < 0x80 && r.xdelta < 0)
        r.xpos += 0x200;
    r.xpos -= 0xbe;

    r.ytarget = r.top + r.ydelta * r.height;
    r.xpos += config.s16_x_off;

    if (config.video.hires) {
        r.xpos <<= 1;
        r.top <<= 1;
        r.ytarget <<= 1;
        r.zoom >>= 1;
    }

    r.rtl = (r.xdelta == -1);
    if (r.rtl) {
        r.xpos -= (r.width << (config.video.hires ? 1 : 0));
        if (r.flip) {
            r.flip = 0;
            r.addr -= (r.pitch - 1);
        } else {
            r.flip = 1;
            r.addr += (r.pitch - 1);
        }
    }

    if (config.video.hiresprites == 1)
        r.xpos += r.offset;

    if (r.flip)
        r.addr = r.addr - r.pitch + 1;

    r.H = r.ytarget - r.top;
    if (r.zoom == 0) return false;

    return true;
}

// One sprite's worth of render_px (S0.4): expand each visited source row
// into ex[] (output-order nibbles), then apply in hardware order. Expansion
// has no feedback into drawing, so "expand then apply" equals the
// interleaved upstream form. Rows outside [0, s16_height) are skipped (no
// ROM read, no drawing) but the row accumulator still advances for them,
// exactly as render()'s unconditional per-iteration yacc/addr update does.
void hwsprites::draw_sprite_px(const SpriteRec& r, uint16_t* pixels, bool glow)
{
    const int32_t pend = x2 - r.xpos;
    if (pend <= 0) return;

    const int32_t nsteps = (r.ydelta > 0) ? r.H : -r.H;
    if (nsteps <= 0) return;

    uint8_t ex[SPR_EX_LEN];
    int64_t row_addr = r.addr;
    uint32_t yacc = 0;
    const int32_t scrn_width = config.s16_width;

    for (int32_t d = 0; d < nsteps; d++) {
        const int32_t y = r.top + r.ydelta * d;
        if (y >= 0 && y < config.s16_height) {
            // ---- expand this source row into ex[] (output-order nibbles) ----
            int32_t p = 0;
            int32_t xacc = 0;
            int32_t wk = 0;
            for (;;) {
                if (p >= pend || wk == SPR_WMAX) break;
                const int64_t widx = (int64_t)r.bank * 0x10000 + row_addr +
                                      (r.flip ? (int64_t)(r.pitch - 1 - wk) : (int64_t)wk);
                if (widx < 0 || widx >= (int64_t)SPRITES_LENGTH) break;
                uint32_t w = sprites[widx];
                if (r.flip) w = nibrev(w);

                for (int nb = 0; nb < 8; nb++) {
                    const unsigned n = (w >> (28 - 4 * nb)) & 0xF;
                    while (xacc < 0x200 && p < SPR_EX_LEN) {
                        ex[p++] = (uint8_t)n;
                        xacc += r.zoom;
                    }
                    xacc -= 0x200;
                }
                wk++;
                if ((w & 0xF0) == 0xF0) break;
            }

            // ---- apply in hardware draw order ----
            uint16_t* row = pixels + (size_t)y * scrn_width;
            if (!r.rtl) {
                for (int32_t q = 0; q < p; q++) {
                    const int32_t x = r.xpos + q;
                    if (x < x1 || x >= x2) continue;
                    const unsigned n = ex[q];
                    if (r.shadow && n == 0xA) {
                        // SPRITE_IMPLEMENTATION_SPEC.md S3.2/S3.3: mark this
                        // destination row shadow-touched, exactly as
                        // replay()'s SHADOW branch does, so a later fast-path
                        // sprite's glow push on this same row (this function
                        // is the out-of-envelope fallback within the same
                        // render() dispatch loop replay() serves) is never
                        // filtered by a bitmap this function's own shadow
                        // writes left stale. Harmless when draw_sprite_px is
                        // called standalone via render_px() -- nothing reads
                        // shadow_rows outside a render() call, which resets
                        // the whole bitmap at its own top regardless.
                        shadow_rows[y >> 6] |= 1ull << (y & 63);
                        row[x] |= S16_PALETTE_ENTRIES;
                    } else if (n != 0 && n != 15) {
                        if (glow && x > x1) row[x - 1] &= 0xfff;
                        row[x] = (uint16_t)(n | r.color);
                    }
                }
            } else {
                for (int32_t q = p - 1; q >= 0; q--) {
                    const int32_t x = r.xpos + q;
                    if (x < x1 || x >= x2) continue;
                    const unsigned n = ex[q];
                    if (r.shadow && n == 0xA) {
                        // SPRITE_IMPLEMENTATION_SPEC.md S3.2/S3.3: mark this
                        // destination row shadow-touched, exactly as
                        // replay()'s SHADOW branch does, so a later fast-path
                        // sprite's glow push on this same row (this function
                        // is the out-of-envelope fallback within the same
                        // render() dispatch loop replay() serves) is never
                        // filtered by a bitmap this function's own shadow
                        // writes left stale. Harmless when draw_sprite_px is
                        // called standalone via render_px() -- nothing reads
                        // shadow_rows outside a render() call, which resets
                        // the whole bitmap at its own top regardless.
                        shadow_rows[y >> 6] |= 1ull << (y & 63);
                        row[x] |= S16_PALETTE_ENTRIES;
                    } else if (n != 0 && n != 15) {
                        if (glow && x > x1) row[x - 1] &= 0xfff;
                        row[x] = (uint16_t)(n | r.color);
                    }
                }
            }
        }

        // advance the row accumulator regardless of whether this row was drawn
        yacc += r.zoom;
        row_addr += r.pitch * (int64_t)(yacc >> 9);
        yacc &= 0x1ff;
    }
}

void hwsprites::render_px(uint16_t* pixels, uint8_t priority, bool glow)
{
    for (uint16_t d = 0; d < SPRITE_RAM_SIZE; d += 16) {
        if ((ramBuff[d + 0] & 0x8000) != 0) break;
        SpriteRec rec;
        if (!parse_record(d, priority, rec)) continue;
        draw_sprite_px(rec, pixels, glow);
    }
}


#ifdef CB_RENDER_TEST
// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S0.3: render_ref, a verbatim twin of the
// shipped render() above, with exactly one behavioural switch permitted
// (ref_prepass_fix, the F1 fix at the two pre-pass loop bounds below).
// Everything else -- variable names, control flow, comments -- is copied
// unchanged, except the dead instrumentation the spec calls out for
// deletion (reps/freq/the two high_resolution_clock::now() calls/
// draw[jump_key]+=/the trailing commented-out debug block), which render()
// itself no longer has either (S0.5). The lazy side tables and the twelve
// draw_pixel_* macros are duplicated (sprites_flipped_ref/
// sprites_shadowinfo_ref, the "_ref" macro suffixes) so this twin can be
// driven through its own independent session history without contaminating
// -- or being contaminated by -- the shipped render()'s tables.
// =============================================================================================

void hwsprites::reset_ref_tables()
{
    std::fill_n(sprites_flipped_ref,    std::size(sprites_flipped_ref),    0xffffffff);
    std::fill_n(sprites_shadowinfo_ref, std::size(sprites_shadowinfo_ref), 0xff);
}

#define draw_pixel_1row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}


void hwsprites::render_ref(uint16_t* pixels, const uint8_t priority)
{
    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 16) // was +=8
    {
        // stop when we hit the end of sprite list
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri  = 1 << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        // if hidden, or top greater than/equal to bottom, or invalid bank, punt
        int16_t hide    = (ramBuff[data+0] & 0x5000);
        int32_t height  = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        int16_t bank      =  (ramBuff[data+0] >> 9) & 7;
        int32_t top       =  (ramBuff[data+0] & 0x1ff) - 0x100;
        bool     clip     =  (ramBuff[data+0] & 0x2000);
        uint32_t addr     =   ramBuff[data+1];
        int32_t pitch     = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        uint8_t shadow    =  (ramBuff[data+3] >> 14) & 1;
        int32_t  zoom     =   ramBuff[data+3] & 0x0fff;
        int32_t ydelta    = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t flip      = (~ramBuff[data+4] >> 14) & 1;
        int32_t xdelta    = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t width     =   ramBuff[data+4] & 0x7ff;
        int32_t color     = COLOR_BASE +
                            ((ramBuff[data+5] & 0x7f) << 4);
        int32_t sprite_scanlines =
                            ((ramBuff[data+5] & 0xff00) >> 16) +1;
        int32_t xpos      =   ramBuff[data+6]; // moved from original structure to accomodate widescreen
        int32_t rawh      =   ramBuff[data+7]; // JJP - height of sprite in rows
        int32_t y, ytarget, yacc = 0;

        int16_t offset    =   ramBuff[data+15]; // JJP - provides horizontal offset for hires sprite rendering
        // adjust X coordinate
        // note: the threshhold below is a guess. If it is too high, rachero will draw garbage
        // If it is too low, smgp won't draw the bottom part of the road
        if (xpos < 0x80 && xdelta < 0)
            xpos += 0x200;
        xpos -= 0xbe;

        // clamp to within the memory region size
        if (numbanks)
            bank %= numbanks;

        const uint32_t* spriterom = sprites + 0x10000 * bank;
        uint32_t* spriterom_flipped = sprites_flipped_ref + 0x10000 * bank;
        uint8_t* spriterom_shadowinfo = sprites_shadowinfo_ref + 0x10000 * bank;

        // loop from top to bottom
        ytarget = top + ydelta * height;

        // Adjust for widescreen mode
        xpos += config.s16_x_off;

        // Adjust for hi-res mode
        if (config.video.hires) {
            xpos <<= 1;
            top <<= 1;
            ytarget <<= 1;
            zoom >>= 1;
        }

        // JJP - maintain converted sprites to aid writing them
        uint32_t sprite_height = 0;
        // Determine underlying sprite height
        int32_t steps = ytarget - top;  // true as ydelta is always + or - 1.
        // Use 64-bit multiply to avoid overflow
        sprite_height = ((uint64_t)steps * zoom) >> 9;

        if (xdelta == -1) {
            xdelta = 1;       // always draw left-to-right
            xpos  -= (width << (config.video.hires ? 1 : 0));   // move draw position left by rendered width
            if (flip) {
                flip = 0;
                addr -= (pitch - 1);
            } else {
                flip = 1;
                addr += (pitch - 1);
            }
        }

        uint32_t shadowaddr = addr;

        if (flip) {
            // first encounter of sprite, created flipped entry
            shadowaddr -= (pitch - 1); // start of data, if unflipped

            // S0.3/F1 fix: ref_prepass_fix widens this bound from
            // "y < sprite_height" (the shipped, history-dependent bound) to
            // "y <= sprite_height", building every row the draw loop below
            // can possibly read.
            for (int y = 0; y < (int)sprite_height + (ref_prepass_fix ? 1 : 0); y++) {
                // calculate the addresses for this line
                uint32_t readaddr     = shadowaddr;
                uint32_t writeaddr    = shadowaddr + pitch;
                uint32_t shadow_found = 0;
                if (spriterom_flipped[readaddr] == 0xffffffff) {
                    // first time we've seen this row
                    for (int x = 0; x < pitch; x++) {
                        uint32_t pixels = spriterom[readaddr++];
                        uint8_t  px[8];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0)
                            shadow_found = 0x11;
                        // process eight pixels
                        px[0] = (pixels >> 28) & 0xf;
                        px[1] = (pixels >> 24) & 0xf;
                        px[2] = (pixels >> 20) & 0xf;
                        px[3] = (pixels >> 16) & 0xf;
                        px[4] = (pixels >> 12) & 0xf;
                        px[5] = (pixels >>  8) & 0xf;
                        px[6] = (pixels >>  4) & 0xf;
                        px[7] = (pixels >>  0) & 0xf;

                        spriterom_flipped[--writeaddr] =
                            (px[0] <<  0) |
                            (px[1] <<  4) |
                            (px[2] <<  8) |
                            (px[3] << 12) |
                            (px[4] << 16) |
                            (px[5] << 20) |
                            (px[6] << 24) |
                            (px[7] << 28);
                    };
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        } else {
            // not flipped
            for (int y = 0; y < (int)sprite_height + (ref_prepass_fix ? 1 : 0); y++) {
                if (spriterom_shadowinfo[shadowaddr] == 0xff) {
                    // first time seeing this sprite line
                    uint32_t readaddr = shadowaddr;
                    uint32_t shadow_found = 0;
                    for (int i = 0; i < pitch; ++i) {
                        uint32_t pixels = spriterom[readaddr++];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0) {
                            shadow_found = 0x11;
                            break;
                        }
                    }
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        }

        // adjust x-position with pre-determined offset for hi-res sprite rendering
        if (config.video.hiresprites == 1)
            xpos += offset;

        // choose which ROM to read from - flipped or non-flipped
        const uint32_t* spritedata;
        if (flip) {
            addr = addr - pitch + 1; // set pointer to start of sprite row
            spritedata = &spriterom_flipped[0];
        } else {
            spritedata = &spriterom[0];
        }

        const uint16_t scrn_width = config.s16_width;
        const unsigned span = (unsigned)(x2 - x1);

        uint32_t jump_key = 3;
        // drawing loop
        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < config.s16_height)
            {
                uint16_t* pPix1 = pixels + ((y+0) * scrn_width) + xpos;
                uint32_t spriteaddr = addr;
                int32_t xacc = 0;

                // determine how many rows will be written on this pass
                uint16_t count = 1;
                uint32_t frac = yacc + zoom;
                uint16_t countmax = std::min( (uint16_t)(ytarget - y),
                                              std::min<uint16_t>(3, config.s16_height - y));
                while (frac < 0x200 && count < countmax) {
                    frac += zoom;
                    count++;
                }

                bool shadowfound = shadow && (spriterom_shadowinfo[addr] == 0x11);

                count = (count - 1) & 3; // 0..3
                jump_key =   (count                   ) |
                             ((clip)         ? 0u : 4u) |
                             ((shadowfound)  ? 8u : 0u);

                switch (jump_key) {

                    case 0:
                    {
                        // no shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 1:
                    {
                        // no shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 2:
                    {
                        // no shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 3: break; // count==4 - not used as <1%
                    case 4:
                    {
                        // no shadows, not clipped, count==1, not flipped
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 5:
                    {
                        // no shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 6:
                    {
                        // no shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 7: break; // count==4 - not used as <1%
                    case 8:
                    {
                        // shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 9:
                    {
                        // shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 10:
                    {
                        // shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 11: break; // count==4 - not used as <1%
                    case 12:
                    {
                        // shadows, not clipped, count==1, not flipped
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 13:
                    {
                        // shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 14:
                    {
                        // shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 15: break; // count==4 - not used as <1%
                }
            }
            // accumulate zoom factors; if we carry into the high bit, skip an extra row
            yacc += zoom;
            addr += pitch * (yacc >> 9);
            yacc &= 0x1ff;
        }
    }
}

#undef draw_pixel_1row_ref
#undef draw_pixel_2row_ref
#undef draw_pixel_3row_ref
#undef draw_pixel_1row_nc_ref
#undef draw_pixel_2row_nc_ref
#undef draw_pixel_3row_nc_ref
#undef draw_pixel_1row_ns_ref
#undef draw_pixel_2row_ns_ref
#undef draw_pixel_3row_ns_ref
#undef draw_pixel_1row_nc_ns_ref
#undef draw_pixel_2row_nc_ns_ref
#undef draw_pixel_3row_nc_ns_ref

#endif // CB_RENDER_TEST
