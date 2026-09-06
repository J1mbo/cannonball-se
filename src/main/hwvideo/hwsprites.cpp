#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwarch.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iterator>
#include <utility>   // S1: std::swap (hwsprites::swap()'s ram/ramBuff pointer flip)

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
    std::fill_n(ram,     SPRITE_RAM_SIZE, 0x0);
    std::fill_n(ramBuff, SPRITE_RAM_SIZE, 0x0);
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
//
// Per-frame memory-work Tier 1 #4: this used to exchange every element of
// ram/ramBuff (a full read+write of both halves every frame). ram/ramBuff
// are now a pointer pair over the two ram_store[] halves (hwsprites.hpp), so
// flipping which physical half each name refers to is exactly equivalent --
// every subsequent read of `ramBuff` sees what the element-wise swap would
// have moved there, and every subsequent write via `ram`/write() lands in
// the other half, precisely as before -- at O(1) instead of O(RAM size).
void hwsprites::swap()
{
    std::swap(ram, ramBuff);
}

// S0.5: unconditional test/inspection accessor. Copies exactly what
// hwsprites::swap() leaves ramBuff holding (SPRITE_RAM_SIZE == 0x800
// halfwords) -- used by the driver's `replay` to load a captured frame's
// sprite RAM without needing friend access.
void hwsprites::load_sprite_ram(const uint16_t* buf)
{
    // ramBuff is now a pointer (S1: the ram/ramBuff double-buffer flip), so
    // sizeof(ramBuff) would be sizeof(uint16_t*) instead of the buffer's
    // byte size -- spell it out explicitly.
    std::memcpy(ramBuff, buf, SPRITE_RAM_SIZE * sizeof(uint16_t));
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
        slot.reset(new uint8_t[SPR_SCHED_LEN + SPR_WMAX]);
        uint32_t xacc = 0;
        for (int32_t i = 0; i < SPR_SCHED_LEN; i++) {
            int32_t r = 0;
            while (xacc < 0x200) { r++; xacc += zoom; }
            xacc -= 0x200;
            // SPRITE_R1_LLD.md sec. 4.1: pins the MAXRUN select_kernel commits
            // to -- C5 (SCHEDULE_VERIFICATION.md): zoom >= 0x100 => run <= 2,
            // zoom >= 0x80 => run <= 4. store_fill<2>/<4> (sec. 4.2) rely on
            // this: a run this assertion would trip is a store past the
            // MAXRUN-halfword width the caller selected.
            assert(zoom < (uint32_t)SPR_FAST_ZOOM_MIN ||
                   (r <= 4 && (zoom < 0x100 || r <= 2)));
            slot[i] = (uint8_t)r;
        }
        // SPRITE_IMPLEMENTATION_SPEC.md S4b: wsum[m] = sum(run[8m..8m+7]),
        // appended after run[] in the same allocation (wsum(zoom) above just
        // offsets into it). Every run[] entry here is 1-4 (the assert above,
        // decode_row's fast envelope), so the sum is at most 32 -- fits
        // uint8_t with margin.
        for (int32_t m = 0; m < SPR_WMAX; m++) {
            uint32_t sum = 0;
            for (int32_t k = 0; k < 8; k++) sum += slot[m * 8 + k];
            slot[SPR_SCHED_LEN + m] = (uint8_t)sum;
        }
    }
    return slot.get();
}

// SPRITE_R1_LLD.md sec. 2.5: emit_l(rec, n, type, pos) replaces the old
// member emit(type, pos). Same two rules, same order, appending a span-type
// change at output position pos to rec[]: a zero-length record -- a run-0
// pixel landed exactly on the previous boundary -- is dropped by
// decrementing n first; a record whose type equals the (possibly
// just-uncovered) previous record's type is a no-op merge. The record count
// is now value in/value out (never a member read back through memory
// inside the row loop), so decode_row_k's local `n` stays a register for
// the whole row (sec. 2.4's alias-reload fix).
int32_t hwsprites::emit_l(SpanRec* rec, int32_t n, uint8_t type, int32_t pos)
{
    if (n > 0 && rec[n - 1].pos == (uint16_t)pos)
        n--;
    if (n > 0 && rec[n - 1].type == type)
        return n;
    rec[n].pos = (uint16_t)pos;
    rec[n].type = type;
    rec[n].flags = 0;
    return n + 1;
}

// SPRITE_IMPLEMENTATION_SPEC.md S4b (DESIGN_FABLE.md 2.4): the exact "any
// nibble is 0, 0xF, or (smask set) 0xA" test via SWAR. C1 = 0x11111111,
// C8 = C1 << 3 = 0x88888888. z detects a zero nibble in w (any GAP nibble);
// f detects a zero nibble in ~w, i.e. a 0xF nibble in w (also GAP); a
// detects a zero nibble in w^0xAAAAAAAA, i.e. a 0xA nibble in w, masked by
// smask so it is only consulted when the shadow flag is set (a clear
// smask makes 0xA a plain colour nibble, per S4.3). The "any zero nibble"
// SWAR form has no false positives or negatives (only the "which nibble"
// variant does), so this is an exact classifier, not a filter.
bool hwsprites::classify_mixed(uint32_t w, uint32_t smask)
{
    constexpr uint32_t C1 = 0x11111111u;
    constexpr uint32_t C8 = C1 << 3; // 0x88888888
    const uint32_t z  = (w - C1) & ~w;
    const uint32_t f  = (~w - C1) & w;
    const uint32_t xa = w ^ 0xAAAAAAAAu;
    const uint32_t a  = ((xa - C1) & ~xa) & smask;
    return ((z | f | a) & C8) != 0;
}

// select_kernel (SPRITE_R1_LLD.md sec. 4.1): the run-0 regime (zoom >= 0x200)
// takes the generic instantiation (runtime flip, MAXRUN=4, HEAD's own scan);
// C5 (SCHEDULE_VERIFICATION.md) pins run <= 2 for zoom >= 0x100 and run <= 4
// for zoom >= 0x80 (sched()'s assertion above), so the other four
// instantiations are exact for the whole zoom < 0x200 range decode_row_k's
// caller ever selects them for.
hwsprites::Kernel hwsprites::select_kernel(int32_t zoom, int32_t flip)
{
    if (zoom >= 0x200) return K_GEN;
    if (zoom >= 0x100) return flip ? K_F1R2 : K_F0R2;
    return flip ? K_F1R4 : K_F0R4;
}

// SPRITE_R1_LLD.md sec. 2.7: decode_row is now a thin wrapper -- only the
// driver's SpriteTestAccess::decode_row hook calls this (the game path goes
// through draw_sprite_rows_t -> decode_row_k directly via the ctx built once
// per sprite in draw_sprite_fast). Builds a SpriteCtx (pixels == nullptr:
// decode never touches the destination) and stores decode_row_k's returned
// record count into the member nrec -- the only place nrec is written, so
// the replay() wrapper below and the driver's own nrec(hw) hook keep seeing
// exactly HEAD's decode_row/replay hand-off. R1b: dispatches through the
// same select_kernel switch draw_sprite_fast uses, so unit s1c/s2b/s2c
// exercise the real <FLIP, MAXRUN> instantiation for every zoom they
// generate (sec. 4.1's closing note).
void hwsprites::decode_row(const SpriteRec& r, int32_t j, int32_t pend)
{
    SpriteCtx c;
    make_ctx(r, nullptr, pend, c);
    switch (select_kernel(r.zoom, r.flip)) {
        case K_F0R2: nrec = decode_row_k<0, 2>(c, j); break;
        case K_F1R2: nrec = decode_row_k<1, 2>(c, j); break;
        case K_F0R4: nrec = decode_row_k<0, 4>(c, j); break;
        case K_F1R4: nrec = decode_row_k<1, 4>(c, j); break;
        default:     nrec = decode_row_k<2, 4>(c, j); break;
    }
}

// store_fill<MAXRUN> (SPRITE_R1_LLD.md sec. 4.2, R1b): fill by shift-OR at
// the class width -- removes R0 item 1 (nine `mul x,x,x13` per source word,
// HEAD's `v * 0x0001000100010001ULL`). `v = nibble | color <= 0xFFF` needs
// no masking before widening (HEAD's `and` was the uint16_t -> uint64_t
// widen for the multiply). Same instruction count as HEAD's 8-byte form for
// MAXRUN=4 (three ALU ops either way) but 3-cycle-latency `mul` -> 1-cycle
// `orr`s off the value chain in front of the store, and the multiply pipe
// freed; MAXRUN=2 additionally halves the store width. Overrun safety is
// LLD sec. 4.2's argument verbatim with MAXRUN substituted: a MAXRUN-wide
// store at `pos` writes at most MAXRUN-1 halfwords past this nibble's own
// run, and either a following nibble in the row re-stores its own MAXRUN-
// wide value over every one of them (both stores are the same width) or
// these are the row's final nibble and the spurious positions lie at/after
// the sentinel position, which replay_k never reads (only [rec[k].pos,
// rec[k+1].pos) of a COLOUR record is read from line[]).
template <> CB_ALWAYS_INLINE void hwsprites::store_fill<2>(uint16_t* p, uint32_t v)
{
    const uint32_t v2 = v | (v << 16);       // A64: one `orr wA, wV, wV, lsl #16`
    std::memcpy(p, &v2, 4);                  //      one `str wA, [xLine, xPos, lsl #1]`
}
template <> CB_ALWAYS_INLINE void hwsprites::store_fill<4>(uint16_t* p, uint32_t v)
{
    const uint32_t v2 = v | (v << 16);
    const uint64_t v4 = (uint64_t)v2 | ((uint64_t)v2 << 32); // A64: `orr xA, xA, xA, lsl #32` (w-write zero-extends)
    std::memcpy(p, &v4, 8);                                  //      `str xA, ...`
}

// decode_row_k (SPRITE_R1_LLD.md sec. 3/4.2/5.2): expands source row j into
// line[] (output-order n|color halfwords, always written MAXRUN-at-a-time
// regardless of this nibble's own run length -- safe because any overrun
// lands on positions the next source pixel overwrites, or beyond the
// sentinel where replay never reads, per SPR_LB_LEN's margin) and into
// rec[] (the maximal-run span records, returned as a record count rather
// than the old member nrec). Reads each ROM word once, mirrors flipped rows
// on the fly with nibrev() (when FLIP selects it) and classifies shadow per
// nibble during decode.
//
// R1a de-glue vs HEAD's decode_row (SPRITE_R1_LLD.md sec. 3.1-3.6): every
// field this needs (line, rec, run, wsum, rom, color, smask, pend, flip) is
// copied from the ctx into a local at entry and never has its address taken
// (sec. 2.4 -- this, not passing the ctx by reference, is what stops the
// per-nibble `color` reload after each line[] store); the eight run-length
// bytes for a word are one 8-byte load instead of eight `run[]` byte reads
// (sec. 3.4); the per-word ROM bounds test is a per-row window (wlimit)
// instead of HEAD's per-word int64 compare-and-branch (sec. 3.6).
//
// R1b (sec. 4.1/4.2) adds: <FLIP, MAXRUN> template parameters -- FLIP in
// {0, 1} compiles the nibrev()/reverse-index branch in or out entirely
// (dead code eliminated, no runtime test), FLIP == 2 (the generic/K_GEN
// instantiation only) reads c.flip at run time exactly as R1a's single
// body did; MAXRUN selects store_fill<MAXRUN> above in place of HEAD's
// multiply-splat fill (sec. 4.2's key change, replacing R0 item 1).
//
// SPRITE_IMPLEMENTATION_SPEC.md S4b: before any per-nibble work, classify
// the whole word w. A GAP word (w == 0, every nibble transparent) or an
// all-SHADOW word (w == 0xAAAAAAAA with the shadow flag set) needs no
// per-nibble loop at all: at most one emit_l() (the prev-type dedupe already
// made every other nibble's emit in the slow path a no-op for a uniform
// word) and a single wsum-table jump that lands `pos` on exactly the same
// value the 8-step loop would have reached. Otherwise classify_mixed()'s
// SWAR test decides between the COLOUR fast path (no GAP/terminator/shadow
// nibble present -- one emit_l(), then eight {value, wide store, pos += run}
// steps with no per-nibble type work) and the MIXED path (the original S1
// per-nibble loop, needed only when a word actually mixes types). Every
// branch reaches the identical `pos` and record sequence the S1 per-nibble
// loop would have produced (S4.3); only the *work* done per word changes.
// The terminator test runs after every class -- harmless (and required,
// "keep the test anyway") for GAP/SHADOW words, which can never match it
// (w & 0xF0 is 0x00 or 0xA0, never 0xF0).
template <int FLIP, int MAXRUN>
int32_t hwsprites::decode_row_k(const SpriteCtx& c, int32_t j)
{
    // (a) hoist -- nothing below reads c.* or this->* inside a loop
    // (SPRITE_R1_LLD.md sec. 2.4): these are the locals whose address is
    // never taken, so no store to line[]/rec[] can force any of them to be
    // reloaded from memory.
    uint16_t*       const line   = c.line;
    SpanRec*        const rec    = c.rec;
    const uint8_t*  const run    = c.run;
    const uint8_t*  const wsum   = c.wsum;
    const uint32_t* const rom    = c.rom;
    const uint32_t        color  = c.color;
    const uint32_t        smask  = c.smask;
    const bool            shadow = c.shadow != 0;
    const int32_t         pend   = c.pend;
    const bool            flip   = (FLIP == 2) ? (c.flip != 0) : (FLIP == 1);

    // (b) per-row ROM window (sec. 3.6): replaces HEAD's per-word int64
    // widx compute-and-bounds-test with one per-row wlimit, so the word
    // loop's only per-iteration bound is `words < wlimit`.
    const int32_t rowbase = c.rowoff0 + c.pitch * j;
    const int32_t first   = flip ? rowbase + c.pitch - 1 : rowbase;
    int32_t wlimit = 0;
    if ((uint32_t)first < (uint32_t)SPRITES_LENGTH)
        wlimit = flip ? std::min<int32_t>(SPR_WMAX, first + 1)
                      : std::min<int32_t>(SPR_WMAX, (int32_t)SPRITES_LENGTH - first);
    const int32_t wstep = flip ? -1 : 1;

    int32_t pos = 0, words = 0, n = 0, widx = first;
    uint8_t prev = SPR_GAP;

    while (pos < pend && words < wlimit) {
        uint32_t w = rom[widx];
        if (flip) w = nibrev(w);

        if (w == 0) {
            // GAP word: every nibble transparent, no stores needed.
            if (prev != SPR_GAP) { n = emit_l(rec, n, SPR_GAP, pos); prev = SPR_GAP; }
            pos += wsum[words];
        } else if (shadow && w == 0xAAAAAAAAu) {
            // All-SHADOW word.
            if (prev != SPR_SHADOW) { n = emit_l(rec, n, SPR_SHADOW, pos); prev = SPR_SHADOW; }
            pos += wsum[words];
        } else {
            // (c) one 8-byte load for all eight run lengths of this word
            // (sec. 3.4), instead of HEAD's eight separate run[] byte reads.
            uint64_t runs;
            std::memcpy(&runs, run + (size_t)words * 8, 8);
            if (!classify_mixed(w, smask)) {
                // COLOUR word: no GAP/terminator/(shadow) nibble present.
                if (prev != SPR_COLOUR) { n = emit_l(rec, n, SPR_COLOUR, pos); prev = SPR_COLOUR; }
                CB_UNROLL(8)
                for (int k = 0; k < 8; k++) {
                    store_fill<MAXRUN>(line + pos, ((w >> (28 - 4 * k)) & 0xFu) | color); // (d) sec. 4.2
                    pos += (int32_t)((runs >> (8 * k)) & 0xFFu);
                }
            } else {
                // MIXED word: the S1 per-nibble loop (type lookup needed).
                CB_UNROLL(8)
                for (int k = 0; k < 8; k++) {
                    const uint32_t nib = (w >> (28 - 4 * k)) & 0xFu;
                    const uint8_t t = (nib == 0 || nib == 15) ? SPR_GAP
                                    : ((shadow && nib == 10) ? SPR_SHADOW : SPR_COLOUR);
                    if (t != prev) { n = emit_l(rec, n, t, pos); prev = t; }

                    store_fill<MAXRUN>(line + pos, nib | color);

                    pos += (int32_t)((runs >> (8 * k)) & 0xFFu);
                }
            }
        }

        words++;
        widx += wstep;
        if ((w & 0xF0) == 0xF0) break; // terminator, tested on the (mirrored) word after it is fully drawn
    }
    return emit_l(rec, n, SPR_GAP, pos); // sentinel; guarantees the last record is a GAP
}

// prefetch_rom_row_k (SPRITE_R1_LLD.md sec. 5.1): replaces prefetch_rom_row.
// Read-prefetch the two ROM words decode_row_k will touch first and last for
// source row jrow ("the words at k = 0 and k = pitch - 1"). The *set* of two
// hinted addresses is the same regardless of direction, so (unlike HEAD)
// this needs no flip test of its own. Bounds-checked; a row whose hinted
// words fall outside the ROM image simply prefetches nothing. jrow is never
// negative at either call site (draw_sprite_rows only ever passes
// src(d_lo) or a value derived by adding to it), so HEAD's `if (jrow < 0)
// return` was dead and is dropped.
void hwsprites::prefetch_rom_row_k(const SpriteCtx& c, int32_t jrow)
{
    const int32_t rowbase = c.rowoff0 + c.pitch * jrow;
    const int32_t a = rowbase, b = rowbase + c.pitch - 1;
    if ((uint32_t)a < (uint32_t)SPRITES_LENGTH) cb_prefetch_r(c.rom + a);
    if ((uint32_t)b < (uint32_t)SPRITES_LENGTH) cb_prefetch_r(c.rom + b);
}

// SPRITE_R1_LLD.md sec. 2.7: replay is now a thin wrapper -- only the
// driver's SpriteTestAccess::replay hook calls this (the game path goes
// through draw_sprite_rows -> replay_k directly). Builds a SpriteCtx via
// make_ctx (passing the caller's cx1 through as make_ctx's `pend` argument,
// exactly as draw_sprite_fast's own dispatch does when da/db/cx0/cx1 come
// from the same values decode_row was just called with), then installs the
// caller's own cx0/cx1 -- s2c relies on being able to pass a cx0/cx1
// unrelated to what decode_row used (cx0 = -xpos, cx1 = 1) -- exactly as
// HEAD's replay() used its own arguments rather than anything decode_row
// computed. replay_k reads the member nrec set by the decode_row wrapper
// above.
void hwsprites::replay(const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1)
{
    SpriteCtx c;
    make_ctx(r, pixels, cx1, c);
    c.cx0 = cx0;
    c.cx1 = cx1;
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
    if (debug_disable_clip) { c.cx1 += 32; }
#endif
    replay_k(c, nrec, da, db);
}

// replay_k (SPRITE_R1_LLD.md sec. 3.7/5.4): writes the just-decoded row's
// records onto destination rows [da, db) of the sprite the ctx was built
// for. COLOUR spans are store-only -- D copies the whole span once per row
// (memcpy); D' loads each 4-halfword chunk of the span exactly once and
// stores it to every destination row, with a scalar halfword tail -- the
// destination is never read by either. SHADOW spans are an inline
// read-modify-write, one pixel at a time, and are never deferred or batched
// across rows. Both modes write exactly [lo, hi) and nothing outside it;
// c.replay_mode just selects between them (measured, not verified
// separately -- S1.5 picks the shipped default).
//
// R1a de-glue vs HEAD's replay (SPRITE_R1_LLD.md sec. 3.1/3.7): every ctx
// field is copied into a local at entry (sec. 2.4); the S3 shadow-prefetch
// scan below only runs `if (c.shadow)` -- HEAD ran it, harmlessly, for
// every sprite (it `continue`s on every non-SHADOW record, and a non-shadow
// sprite's decode_row_k never emits a SPR_SHADOW record at all, sec. 3.7),
// so gating it changes no output, only skips provably-dead work.
void hwsprites::replay_k(const SpriteCtx& c, int32_t nrec, int32_t da, int32_t db)
{
    const SpanRec* const rec     = c.rec;
    uint16_t*      const base    = c.base;
    const int32_t        stride  = c.stride, cx0 = c.cx0, cx1 = c.cx1, top = c.top;
    const bool           glow_on = c.glow != 0, rtl = c.rtl != 0;
    const int            mode    = c.replay_mode;

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
    if (c.shadow) {
        const int32_t pf_hi = std::min<int32_t>(db + SPR_SHADOW_PF_ROWS * (db - da), c.d_hi);
        if (pf_hi > db) {
            for (int32_t k = 0; k + 1 < nrec; k++) {
                if (rec[k].type != SPR_SHADOW) continue;
                const int32_t lo = std::max<int32_t>(rec[k].pos, cx0);
                const int32_t hi = std::min<int32_t>(rec[k + 1].pos, cx1);
                if (hi <= lo) continue;
                for (int32_t d = db; d < pf_hi; d++) {
                    uint16_t* const rowp = base + (ptrdiff_t)d * stride;
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
            colour_span_k(c.line, base, stride, da, db, lo, hi, mode);

            // SPRITE_IMPLEMENTATION_SPEC.md S2.2 (DESIGN_GLOW.md section 1):
            // one deferred bit-12 clear at the left neighbour of this run's
            // first drawn pixel, per destination row, iff that neighbour is
            // inside the clip window (lo > cx0, i.e. x(lo) > x1). Records
            // are maximal runs (emit_l()'s drop/merge rule, S1.2) walked
            // left to right, so rec[k-1] -- when non-empty -- is always the
            // screen pixel at lo-1; DESIGN_GLOW.md section 2's amendment
            // suppresses the clear for a right-to-left sprite when that
            // neighbour is a SHADOW run of this same sprite row (the
            // hardware clears before it ORs in that case, not after). This
            // is the only destination read the colour path adds, and it
            // happens later, at the flush -- the copy above never reads the
            // destination.
            if (glow_on && lo > cx0) {
                const bool suppress = rtl && k > 0 && rec[k - 1].type == SPR_SHADOW;
                uint16_t* const col = base + (lo - 1);
                for (int32_t d = da; d < db; d++) {
                    if (suppress) {
#ifdef CB_SPRITE_STATS
                        stat_glow_suppressed_rtl++;
#endif
                    } else {
                        // SPRITE_IMPLEMENTATION_SPEC.md S3.2: bit 12 can only
                        // ever have been set by a shadow OR (the SHADOW
                        // branch below, or an all-ones bitmap when the frame
                        // is not provably clean), so a clear on a row whose
                        // shadow_rows bit is still 0 is a provable no-op --
                        // skip the push (and its eventual destination load
                        // at the flush) entirely.
                        const int32_t y = top + d;
                        if ((shadow_rows[y >> 6] >> (y & 63)) & 1ull) {
                            glow_push(col + (ptrdiff_t)d * stride);
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
            // (unreachable when !c.shadow, sec. 3.7 -- but this branch is
            // unconditional, matching HEAD, so no behaviour depends on the
            // gate above besides skipping the prefetch scan.)
            //
            // SPRITE_IMPLEMENTATION_SPEC.md S3.2: mark every destination row
            // in [da, db) as shadow-touched before any of this span's RMW
            // writes below, so any glow push depending on it (this row, this
            // sprite, or a later one) sees the bit set. Marking the whole
            // group first (rather than interleaved row-by-row) still
            // guarantees each row's own bit is set before its own OR -- the
            // only ordering S3.3 requires -- since the two loops touch
            // disjoint state (shadow_rows vs pixel data).
            for (int32_t d = da; d < db; d++) {
                const int32_t y = top + d;
                shadow_rows[y >> 6] |= 1ull << (y & 63);
            }
            shadow_span_k(base, stride, da, db, lo, hi);
        }
    }
}

// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S4a: replay()'s two span primitives. Both
// touch only row-major halfwords at base + d*scrn_width + p for p in
// [lo, hi), d in [da, db) -- never outside that span (S2's glow ring and
// canaries assume the span is the exact write set). NEON builds (AArch64,
// CB_HAVE_NEON) use 16-byte vector loads/stores over full 8-halfword chunks
// with the final chunk re-based to hi - 8 so it overlaps the previous chunk
// and never passes hi; spans under 8 halfwords keep the S1 scalar form
// unconditionally (the #else branch below on every target, and also the
// NEON branch's own n < 8 fallthrough). CB_FORCE_SCALAR/non-AArch64 builds
// never define CB_HAVE_NEON (hwarch.hpp), so the #else branch is exactly
// the pre-S4 scalar code, unchanged.
// =============================================================================================

// colour_span_k/shadow_span_k (SPRITE_R1_LLD.md sec. 4.3/5.5): static so
// replay_k can call them straight from a SpriteCtx without a `this` detour;
// `line`/`mode` are passed explicitly since there is no `this` here (the
// member wrappers below pass this->line/this->replay_mode).
//
// R1b (sec. 4.3, R0 item 5): n >= 8 is completely unchanged from R1a/HEAD --
// the NEON chunk loops (CB_HAVE_NEON) or, on a build without NEON, HEAD's
// own memcpy-per-row (D) / 8-byte-chunk-plus-halfword-tail (D') scalar form,
// verbatim. What changes is 1 <= n <= 7: instead of falling through to that
// same scalar form (a `bl memcpy` per destination row on every build,
// R0 item 5), a fixed-width copy ladder loads the (1, 2, or 2x4-halfword)
// source once and stores it to every destination row with 2/4/8-byte
// std::memcpy calls, each of which lowers to a plain load/store at a
// constant size -- no libc call, D and D' coincide here (the ladder is
// mode-independent, sec. 4.3's stated simplification).
void hwsprites::colour_span_k(const uint16_t* line, uint16_t* base, int32_t stride,
                               int32_t da, int32_t db, int32_t lo, int32_t hi, int mode)
{
    const int32_t n = hi - lo;
    const uint16_t* const src = line + lo;
    if (n >= 8) {
#if defined(CB_HAVE_NEON)
        if (mode == 0) {
            // D: span-copy, one wide-store loop per destination row.
            for (int32_t d = da; d < db; d++) {
                uint16_t* dst = base + (size_t)d * stride + lo;
                int32_t p = 0;
                for (;;) {
                    vst1q_u16(dst + p, vld1q_u16(src + p));
                    if (p + 8 >= n) break;
                    p = std::min(p + 8, n - 8); // final chunk re-based to n - 8; never passes n
                }
            }
        } else {
            // D': load each 16-byte chunk once, store it to every destination row.
            int32_t p = 0;
            for (;;) {
                const uint16x8_t chunk = vld1q_u16(src + p);
                for (int32_t d = da; d < db; d++)
                    vst1q_u16(base + (size_t)d * stride + lo + p, chunk);
                if (p + 8 >= n) break;
                p = std::min(p + 8, n - 8);
            }
        }
#else
        // HEAD's S1 scalar form, unchanged, now reached only for n >= 8
        // (R1a/HEAD used this for every n on a non-NEON build).
        if (mode == 0) {
            // D: span-copy, one memcpy per destination row.
            for (int32_t d = da; d < db; d++)
                std::memcpy(base + (size_t)d * stride + lo, line + lo, (size_t)n * sizeof(uint16_t));
        } else {
            // D': load each chunk once, store it to every destination row.
            int32_t p = lo;
            for (; p + 4 <= hi; p += 4) {
                uint64_t chunk;
                std::memcpy(&chunk, line + p, 8);
                for (int32_t d = da; d < db; d++)
                    std::memcpy(base + (size_t)d * stride + p, &chunk, 8);
            }
            for (; p < hi; p++) {
                const uint16_t v = line[p];
                for (int32_t d = da; d < db; d++)
                    (base + (size_t)d * stride)[p] = v;
            }
        }
#endif
        return;
    }

    // R1b ladder (SPRITE_R1_LLD.md sec. 4.3), 1 <= n <= 7. Source loaded
    // once, stored to every row: D and D' coincide here, so `mode` is
    // unused. Index math (sec. 4.3): for 4 <= n <= 7, [lo, lo+4) U
    // [hi-4, hi) == [lo, hi) since hi-4 >= lo (n >= 4) and hi-4 <= lo+3 < lo+4
    // (n <= 7); the [8-n, 8) overlap receives line[] copies of the same
    // indices from both stores, so it is not a correctness concern. Same
    // argument at half the width for 2 <= n <= 3. No halfword outside
    // [lo, hi) is written; the destination is never read (a/b are loaded
    // from line[], never from base[]).
    (void)mode;
    if (n >= 4) {                                   // 4..7: [lo, lo+4) U [hi-4, hi) == [lo, hi)
        uint64_t a, b;
        std::memcpy(&a, src, 8);
        std::memcpy(&b, line + hi - 4, 8);
        for (int32_t d = da; d < db; d++) {
            uint16_t* const row = base + (ptrdiff_t)d * stride;
            std::memcpy(row + lo,     &a, 8);
            std::memcpy(row + hi - 4, &b, 8);
        }
    } else if (n >= 2) {                            // 2..3: [lo, lo+2) U [hi-2, hi) == [lo, hi)
        uint32_t a, b;
        std::memcpy(&a, src, 4);
        std::memcpy(&b, line + hi - 2, 4);
        for (int32_t d = da; d < db; d++) {
            uint16_t* const row = base + (ptrdiff_t)d * stride;
            std::memcpy(row + lo,     &a, 4);
            std::memcpy(row + hi - 2, &b, 4);
        }
    } else {                                        // n == 1
        const uint16_t a = *src;
        for (int32_t d = da; d < db; d++)
            (base + (ptrdiff_t)d * stride)[lo] = a;
    }
}

void hwsprites::shadow_span_k(uint16_t* base, int32_t stride, int32_t da, int32_t db, int32_t lo, int32_t hi)
{
    for (int32_t d = da; d < db; d++) {
        uint16_t* rowp = base + (size_t)d * stride;
#if defined(CB_HAVE_NEON)
        const uint16x8_t maskv = vdupq_n_u16((uint16_t)S16_PALETTE_ENTRIES);
        int32_t p = lo;
        for (; p + 8 <= hi; p += 8)
            vst1q_u16(rowp + p, vorrq_u16(vld1q_u16(rowp + p), maskv));
        for (; p < hi; p++) // scalar tail: never reads/writes outside [lo, hi)
            rowp[p] |= S16_PALETTE_ENTRIES;
#else
        for (int32_t p = lo; p < hi; p++)
            rowp[p] |= S16_PALETTE_ENTRIES;
#endif
    }
}

// SPRITE_R1_LLD.md sec. 4.3: replay_colour_span/replay_shadow_span keep
// their exact HEAD signature (the driver's SpriteTestAccess hooks call
// these) and are now one-line forwarders onto the static kernels above,
// passing this->line/this->replay_mode explicitly since the kernels have no
// `this`.
void hwsprites::replay_colour_span(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi)
{
    colour_span_k(line, base, scrn_width, da, db, lo, hi, replay_mode);
}

void hwsprites::replay_shadow_span(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi)
{
    shadow_span_k(base, scrn_width, da, db, lo, hi);
}

#ifdef CB_RENDER_TEST
// SPRITE_IMPLEMENTATION_SPEC.md S4.4: pinned-scalar twins of the two
// functions above, verbatim copies of their pre-S4 bodies (the S0.3 house
// style for "_ref"/"_s1" twins: literal duplication, not a shared #ifdef
// branch, so this pins down "the S1 scalar form" independently of whatever
// replay_colour_span/replay_shadow_span above happen to compile to on this
// build). unit s4b compares the two pairs bitwise over every (lo, hi, da, db)
// it exercises.
void hwsprites::replay_colour_span_s1(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi)
{
    if (replay_mode == 0) {
        for (int32_t d = da; d < db; d++)
            std::memcpy(base + (size_t)d * scrn_width + lo, line + lo, (size_t)(hi - lo) * sizeof(uint16_t));
    } else {
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
}

void hwsprites::replay_shadow_span_s1(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi)
{
    for (int32_t d = da; d < db; d++) {
        uint16_t* rowp = base + (size_t)d * scrn_width;
        for (int32_t p = lo; p < hi; p++)
            rowp[p] |= S16_PALETTE_ENTRIES;
    }
}
#endif

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

// SPRITE_R1_LLD.md sec. 3.8: unrolled x4 -- four loads issued (into locals,
// so the compiler can keep them in flight) before any of the four stores.
// Ring entries within one flush are always distinct addresses (each is
// lo-1 of a distinct (row, maximal colour run); runs on one row are
// disjoint, separated by a non-colour pixel, and the ring is flushed at
// least at every sprite end, DESIGN_GLOW.md section 3), so reordering the
// loads ahead of the stores reads the same values it would have serially;
// `&= 0xfff` is idempotent, so even a duplicate address would still give
// the same result. glow_push (above) is unchanged.
void hwsprites::glow_flush()
{
    const int32_t n = glow_n;
    int32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint16_t* const p0 = glow_ring[i],     * const p1 = glow_ring[i + 1],
                * const p2 = glow_ring[i + 2], * const p3 = glow_ring[i + 3];
        const uint16_t v0 = *p0, v1 = *p1, v2 = *p2, v3 = *p3; // four loads in flight before any store
#ifdef CB_SPRITE_STATS
        stat_glow_changed += (v0 & S16_PALETTE_ENTRIES ? 1 : 0) + (v1 & S16_PALETTE_ENTRIES ? 1 : 0)
                            + (v2 & S16_PALETTE_ENTRIES ? 1 : 0) + (v3 & S16_PALETTE_ENTRIES ? 1 : 0);
#endif
        *p0 = (uint16_t)(v0 & 0xfff); *p1 = (uint16_t)(v1 & 0xfff); // matches draw_sprite_px's glow branch exactly
        *p2 = (uint16_t)(v2 & 0xfff); *p3 = (uint16_t)(v3 & 0xfff);
    }
    for (; i < n; i++) {
#ifdef CB_SPRITE_STATS
        if (*glow_ring[i] & S16_PALETTE_ENTRIES) stat_glow_changed++;
#endif
        *glow_ring[i] &= 0xfff;
    }
    glow_n = 0;
}

// make_ctx (SPRITE_R1_LLD.md sec. 2.3): builds the per-sprite SpriteCtx once
// -- the only sched() call for this sprite (R0 item 4; HEAD's decode_row and
// replay each independently reread sched()/wsum(), which fold to the same
// cached-slot lookup, once per *row*). Every field here is a pure function
// of (r, pixels, pend, this->{x1, glow, replay_mode}, config), none of which
// draw_sprite_fast's loop ever mutates mid-sprite, so computing each once
// equals HEAD recomputing it every row/word/nibble (sec. 3.1's invariant).
// Non-const: sched() may populate a cache slot on first use for this zoom.
void hwsprites::make_ctx(const SpriteRec& r, uint16_t* pixels, int32_t pend, SpriteCtx& c)
{
    c.line    = line;
    c.rec     = rec;
    c.run     = sched((uint32_t)r.zoom);
    c.wsum    = c.run + SPR_SCHED_LEN;
    c.rom     = sprites;
    c.rowoff0 = r.bank * 0x10000 + r.addr;  // int32: |bank*0x10000 + addr| <= 3*65536 + 65790
    c.pitch   = r.pitch;
    c.color   = (uint32_t)r.color;
    c.smask   = r.shadow ? 0xFFFFFFFFu : 0u; // HEAD 388 (decode_row's smask)
    c.pend    = pend;
    c.zoom    = r.zoom;
    c.shadow  = r.shadow ? 1 : 0;
    c.flip    = r.flip   ? 1 : 0;
    c.rtl     = r.rtl    ? 1 : 0;
    c.glow    = glow     ? 1 : 0;
    c.stride  = (int32_t)config.s16_width;   // HEAD's replay() scrn_width
    c.base    = pixels ? pixels + (ptrdiff_t)r.top * c.stride + r.xpos : nullptr; // same address as HEAD's replay() base; ptrdiff_t instead of size_t for a negative top
    c.top     = r.top;
    c.d_lo    = std::max<int32_t>(0, -r.top);                                 // HEAD 756 (draw_sprite_fast)
    c.d_hi    = std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top);   // HEAD 757 == HEAD's replay() d_hi
    c.cx0     = (int32_t)x1 - r.xpos;                                         // HEAD 760
    c.cx1     = pend;                                                         // HEAD 761
#ifdef CB_RENDER_TEST
    if (debug_disable_clip) c.cx1 += 32; // HEAD 484, moved from replay (per sprite == per call: cx1 is constant per sprite)
#endif
    c.replay_mode = replay_mode;
}

// draw_sprite_rows_t (SPRITE_R1_LLD.md sec. 3.5/4.1/5.6): the closed-form
// vertical loop. Every destination row d in [d_lo, d_hi) reads source row
// src(d) = (d*zoom) >> 9 (division-free once zoom is fixed, exactly the
// shipped accumulator's result -- unit s1a); consecutive d's sharing a
// source row are grouped so that row is decoded exactly once and replayed
// onto every row in the group.
//
// R1a vs HEAD's draw_sprite_fast loop (sec. 3.5): the first group is found
// by HEAD's own scan (identical code, so (j, da, db) start identically);
// every later group uses the division-free recurrence
// da = db; db = min(da + run[j], d_hi) when zoom < 0x200 (SPRITE_R1_LLD.md
// sec. 3.5 proves this produces the same (j, da, db) sequence as HEAD's scan
// via the schedule identities C1/C2/C5), falling back to HEAD's own scan,
// unchanged, for zoom >= 0x200 (the run-0 regime the recurrence cannot
// visit -- run[] entries there are 0 for most i, and the table itself is
// only sized for the zoom < 0x200 side).
//
// R1b (sec. 4.1) turns SHRINK into the template parameter that used to be
// `shrink`'s runtime read, and passes <FLIP, MAXRUN> through to
// decode_row_k so the templated fill/flip decisions are made once here per
// sprite, not per row; replay_k stays a single non-template body (sec. 2.6:
// it has no FLIP/MAXRUN-shaped work, only the record loop and copy/RMW
// kernels, which are selected at run time by record type regardless of
// kernel).
template <int FLIP, int MAXRUN, bool SHRINK>
void hwsprites::draw_sprite_rows_t(const SpriteCtx& c)
{
    const int32_t d_hi = c.d_hi, zoom = c.zoom;
    const uint8_t* const run = c.run;

    int32_t d  = c.d_lo;
    int32_t j  = (d * zoom) >> 9;      // HEAD 765/772, int32 (d <= 511, zoom <= 0xFFF -> < 2^21)
    prefetch_rom_row_k(c, j);          // HEAD 766-767: the first two source rows this sprite will read
    prefetch_rom_row_k(c, j + 1);
    int32_t da = d; d++;
    while (d < d_hi && ((d * zoom) >> 9) == j) d++; // first group: HEAD's own scan
    int32_t db = d;

    for (;;) {
        prefetch_rom_row_k(c, j + SPR_ROM_PF_ROWS);           // HEAD 778
        const int32_t nrec = decode_row_k<FLIP, MAXRUN>(c, j); // HEAD 780, inlined, count in a local
        replay_k(c, nrec, da, db);                             // HEAD 781, inlined
        if (db >= d_hi) break;
        da = db;
        if constexpr (!SHRINK) {          // zoom < 0x200: recurrence (sec. 3.5), no division
            j++;
            db = std::min<int32_t>(da + (int32_t)run[j], d_hi);
        } else {                          // zoom >= 0x200 (run-0 regime): HEAD 772-776, unchanged
            j = (da * zoom) >> 9;
            d = da + 1;
            while (d < d_hi && ((d * zoom) >> 9) == j) d++;
            db = d;
        }
    }
}

// draw_sprite_fast (SPRITE_R1_LLD.md sec. 3.9/4.1): the fast in-envelope
// dispatch target. render()'s dispatch guarantees r.ydelta == 1 here
// (ydelta == -1 routes to draw_sprite_px), so y = r.top + d and d_lo/d_hi
// need only clip to [0, s16_height). An empty row range returns before
// sched() is consulted, exactly as HEAD did (HEAD 756-758's d_lo >= d_hi
// check ran before decode_row's first sched() call); otherwise one SpriteCtx
// is built for the whole sprite and select_kernel picks one of the five
// draw_sprite_rows_t instantiations to run (R1b sec. 4.1) before the glow
// flush.
void hwsprites::draw_sprite_fast(const SpriteRec& r, uint16_t* pixels, int32_t pend)
{
    if (std::max<int32_t>(0, -r.top) >= std::min<int32_t>(r.H, (int32_t)config.s16_height - r.top)) return;

    SpriteCtx c;
    make_ctx(r, pixels, pend, c);
    switch (select_kernel(r.zoom, r.flip)) {
        case K_F0R2: draw_sprite_rows_t<0, 2, false>(c); break;
        case K_F1R2: draw_sprite_rows_t<1, 2, false>(c); break;
        case K_F0R4: draw_sprite_rows_t<0, 4, false>(c); break;
        case K_F1R4: draw_sprite_rows_t<1, 4, false>(c); break;
        default:     draw_sprite_rows_t<2, 4, true >(c); break;   // K_GEN
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
