#pragma once

#include "stdint.hpp"
#include "hwvideo/hwarch.hpp" // SPRITE_R1_LLD.md sec. 2.1: CB_ALWAYS_INLINE, used below
#include <memory>

class video;

class hwsprites
{
public:
    hwsprites();
    ~hwsprites();
    void init(const uint8_t*);
    void reset();
    void set_x_clip(bool);
    void swap();
    uint8_t read(const uint16_t adr);
    void write(const uint16_t adr, const uint16_t data);
    // SPRITE_IMPLEMENTATION_SPEC.md S1.2: dest_clean is the S3 "row already
    // clean" hint (video.cpp's fg_enabled); default false so every existing
    // call site (video.cpp:350, the driver) keeps compiling unchanged. S1
    // does not consult it yet (that lands with S3's shadow_rows bitmap).
    void render(uint16_t* pixels, uint8_t priority, bool dest_clean = false);

    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S0.5: unconditional test/inspection
    // accessors. Harmless to the shipped game (read-only, or a currently-
    // inert setter), used by the standalone driver
    // (tools/sprite_bench/test) and by Video::prepare_frame's -dumpspr
    // capture and CB_SPRITE_REF_PX gate so neither needs friend access.
    // ---------------------------------------------------------------------
    const uint16_t* sprite_ram_buffer() const { return ramBuff; }
    uint16_t get_x1() const { return x1; }
    uint16_t get_x2() const { return x2; }
    void set_glow(bool v) { glow = v; }
    bool get_glow() const { return glow; }
    // S1 stub: selects the colour-replay strategy (0 = D, 1 = D'); unused
    // before S1's draw_sprite_fast exists, harmless to set now (-spritereplay).
    void set_replay_mode(int m) { replay_mode = m; }
    int get_replay_mode() const { return replay_mode; }
    // Copies 0x800 halfwords (SPRITE_RAM_SIZE) into ramBuff, exactly what
    // hwsprites::swap() leaves it holding at the start of a real render() --
    // used by the driver's `replay` to load a captured frame's sprite RAM.
    void load_sprite_ram(const uint16_t* buf);

    // S0.4: the pixel-serial oracle. Compiled always (S0.4): it is also the
    // in-game fallback for out-of-envelope sprites from S1.2 onward, and the
    // CB_SPRITE_REF_PX whole-game reference build (S0.8) calls it directly
    // in place of render(). glow=false reproduces the shipped (modulo F1/F2)
    // output; glow=true is the PIXEL_ACCURACY==1 hardware-accurate output.
    void render_px(uint16_t* pixels, uint8_t priority, bool glow);

private:
    // Clip values.
    uint16_t x1, x2;

    // S0.2: runtime glow flag, initialised from PIXEL_ACCURACY (see the
    // hwsprites.cpp note next to that macro). Not yet consulted by render()
    // itself (that lands with S1/S2's rewrite of the draw loop); render_px
    // always takes glow as an explicit parameter regardless of this member,
    // so this flag only matters via set_glow/get_glow and the game's
    // -glow/-noglow flags, which currently affect only CB_SPRITE_REF_PX and
    // -dumpspr's recorded header field.
    bool glow;
    int replay_mode = 0; // S1 stub (set_replay_mode); unused before S1.

    // 128 sprites, 16 bytes each (0x400)
    static const uint16_t SPRITE_RAM_SIZE = 128 * 16; // was *8
    static const uint32_t SPRITES_LENGTH = 0x100000 >> 2;
    static const uint16_t COLOR_BASE = 0x800;

    uint32_t sprites[SPRITES_LENGTH];               // Little-endian forward sprites
    // SPRITE_IMPLEMENTATION_SPEC.md S1.2: the shipped renderer's lazy
    // "sprites_flipped"/"sprites_shadowinfo" side tables (2.25 MB, and the
    // source of finding F1, the preamble's history-dependence defect) are
    // dropped from the game build here. draw_sprite_fast mirrors flipped rows
    // on the fly with nibrev() and classifies shadow nibbles per-nibble
    // during decode instead. The _ref twin keeps its own copies
    // (sprites_flipped_ref/sprites_shadowinfo_ref below, CB_RENDER_TEST only)
    // so it can still reproduce the shipped renderer's history-dependent
    // behaviour for the S0 selftest/replay classifier.

    // Two halves of RAM
    uint16_t ram[SPRITE_RAM_SIZE];
    uint16_t ramBuff[SPRITE_RAM_SIZE];

    // ---------------------------------------------------------------------
    // S0.4: sprite record parse and the pixel-serial oracle's per-sprite
    // body. Private (encapsulation only -- these are compiled into every
    // build, S0.4); the test driver reaches them through SpriteTestAccess
    // below. parse_record is verbatim from hwsprites.cpp:425-462,468-469,
    // 476-487,509-519,601-611 as SPRITE_IMPLEMENTATION_SPEC.md S0.4 specifies,
    // shared with S1's draw_sprite_fast dispatch once that exists.
    // ---------------------------------------------------------------------
    struct SpriteRec
    {
        uint32_t sprpri;
        int32_t  height;
        int32_t  bank;
        int32_t  top;
        bool     clip;      // stat only (fact 3); draw_sprite_px always clips exactly
        int32_t  addr;      // row-0 ROM word offset within the bank; may go negative (S0.4)
        int32_t  pitch;
        uint8_t  shadow;
        int32_t  zoom;
        int32_t  ydelta;
        int32_t  flip;
        int32_t  xdelta;
        int32_t  width;
        int32_t  color;
        int32_t  xpos;
        int16_t  offset;
        int32_t  ytarget;
        bool     rtl;
        int32_t  H;         // ytarget - top; positive iff ydelta == 1
    };

    static const int32_t SPR_WMAX  = 256;   // bounds an unterminated row (never hit by real ROM data)
    static const int32_t SPR_EX_LEN = 12288; // pend(<=5026) + one word's worth of expansion margin

    // nibrev(w): word w with its eight nibbles in reverse order (nibble k of
    // the result == nibble 7-k of w). Used to read a row backwards for a
    // flipped sprite without the lazy sprites_flipped side table.
    static uint32_t nibrev(uint32_t w);

    // Returns false if this record should be skipped (wrong priority,
    // hidden, or zoom == 0 -- the shipped renderer would loop forever on
    // zoom == 0, S0.4). The caller is responsible for the end-of-list test
    // (ramBuff[d+0] & 0x8000), exactly as render()'s own dispatch loop does.
    bool parse_record(uint16_t d, uint8_t priority, SpriteRec& r) const;

    // One sprite's worth of render_px, in hardware draw order (S0.4).
    void draw_sprite_px(const SpriteRec& r, uint16_t* pixels, bool glow);

    // -----------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S1 -- LINEBUF core. draw_sprite_fast is
    // the fast in-envelope path render() dispatches to; decode_row/replay/
    // emit/sched are its pieces, all scalar/portable (S1; NEON is S4).
    // -----------------------------------------------------------------------

    // Tunable constants (S1.2 defaults).
    static const int32_t SPR_LB_LEN        = 4160;             // line buffer halfwords
    static const int32_t SPR_REC_LEN       = 8 * SPR_WMAX + 8; // span record table length
    static const int32_t SPR_SCHED_LEN     = 8 * SPR_WMAX;     // schedule table length (one run per nibble)
    static const int32_t SPR_FAST_ZOOM_MIN = 0x80;             // below this, render_px (S1.2 fallback envelope)
    static const int32_t SPR_PEND_MAX      = SPR_LB_LEN - 64;  // above this, render_px (line buffer margin)
    static const int32_t SPR_ROM_PF_ROWS   = 2;                // ROM source-row read-ahead
    static const int32_t SPR_GLOW_RING     = 64;                // S2.1: per-sprite deferred glow-clear ring depth
    static const int32_t SPR_SHADOW_PF_ROWS = 2;                // S3.2: shadow-destination prefetch depth, in destination-row-groups ahead

    // Span record type. GAP produces no output and is never replayed;
    // COLOUR is a store-only run; SHADOW is an inline read-modify-write run.
    // "flags" is reserved (S2's glow amendment uses it later); zero in S1.
    enum : uint8_t { SPR_GAP = 0, SPR_COLOUR = 1, SPR_SHADOW = 2 };
    struct SpanRec { uint16_t pos; uint8_t type; uint8_t flags; };

    // SPRITE_R1_LLD.md sec. 2.2: per-sprite context, built once by make_ctx
    // (R1a: one sched() call, all per-sprite constants computed once instead
    // of being reread from `r`/`this`/`config` inside decode_row/replay's
    // per-row and per-nibble loops). Plain data, no methods; the kernels
    // below copy the fields they use into locals at entry (sec. 2.4) -- the
    // struct itself is a convenience (one sched() call, one argument, `this`
    // kept out of the static kernels), not the alias-reload fix.
    struct SpriteCtx
    {
        // scratch (this object's own per-row buffers)
        uint16_t*       line;       // = this->line
        SpanRec*        rec;        // = this->rec
        // schedule: ONE sched() call per sprite (R0 item 4)
        const uint8_t*  run;        // sched(zoom)
        const uint8_t*  wsum;       // run + SPR_SCHED_LEN
        // ROM
        const uint32_t* rom;        // = this->sprites
        int32_t         rowoff0;    // bank*0x10000 + addr: word index of row 0 (may be negative, S0.4)
        int32_t         pitch;      // 0..255
        // decode constants
        uint32_t        color;      // 0x800..0xFF0
        uint32_t        smask;      // shadow ? 0xFFFFFFFF : 0
        int32_t         pend;       // x2 - xpos (decode's horizontal stop)
        int32_t         zoom;       // post-halving, 0x80..0xFFF
        uint8_t         shadow, flip, rtl, glow;   // 0/1
        // destination
        uint16_t*       base;       // pixels + top*stride + xpos (nullptr when built for decode only)
        int32_t         stride;     // config.s16_width
        int32_t         top;        // r.top (shadow_rows / glow filter use y = top + d)
        int32_t         d_lo, d_hi; // destination row range (HEAD 756-757)
        int32_t         cx0, cx1;   // row-local clip window (HEAD 760-761; cx1 widened under debug_disable_clip)
        int32_t         replay_mode;
    };

    // sched(zoom): run[SPR_SCHED_LEN] for this (already post-halving) zoom,
    // built on first use by the reference accumulator (S1.2) and cached.
    // zoom is 12 bits (parse_record masks with 0xfff), so 0x1000 slots covers
    // every value; about 160 are ever populated in a real session. Each
    // cached slot is actually SPR_SCHED_LEN + SPR_WMAX bytes: the run[]
    // table followed by S4b's wsum[] table (wsum(zoom) below just offsets
    // into the same allocation), so sched() remains the sole builder/cache
    // and its own signature/callers are untouched by S4.
    const uint8_t* sched(uint32_t zoom);
    std::unique_ptr<uint8_t[]> sched_tab[0x1000];

    // SPRITE_IMPLEMENTATION_SPEC.md S4b: wsum(zoom)[m] = sum(run[8m..8m+7]),
    // the total output run of source word m at this zoom -- lets decode_row's
    // GAP/all-SHADOW word classes advance `pos` by a whole word in one step
    // (pos += wsum(zoom)[words]) instead of the 8-step per-nibble loop,
    // landing on exactly the same `pos` decode_row's slow path would reach
    // (S4.3: the record sequence and every position are unchanged, only the
    // per-word work is). Built alongside run[] by sched() on first use.
    const uint8_t* wsum(uint32_t zoom) { return sched(zoom) + SPR_SCHED_LEN; }

    // SPRITE_IMPLEMENTATION_SPEC.md S4b (DESIGN_FABLE.md 2.4): the exact
    // "does word w need decode_row's per-nibble MIXED loop" test, via SWAR --
    // true iff any nibble of w is 0 (GAP), 0xF (terminator/GAP), or, when
    // smask is all-ones (the shadow flag is set), 0xA (SHADOW). smask must be
    // 0 (shadow flag clear -- 0xA nibbles are plain colour) or 0xFFFFFFFF
    // (shadow flag set). False means every nibble of w is safely a COLOUR
    // pixel under decode_row's fast path. The any-zero-nibble SWAR form has
    // no false positives/negatives (DESIGN_FABLE.md 2.4), so this is exact,
    // not a filter -- unit s4a proves it exhaustively over all 2^32 words.
    static bool classify_mixed(uint32_t w, uint32_t smask);

    // Per-row decode/replay scratch. Spec fact: "line, rec, nrec are per
    // row; the only per-frame state is S3's" -- kept as members purely so
    // decode_row/replay (and the S0.5 test hooks) can share them without
    // threading extra parameters; nothing here survives, or needs to
    // survive, past the sprite currently being drawn. R1a (SPRITE_R1_LLD.md
    // sec. 2.5/5.7): the old member emit() is gone -- decode_row_k keeps its
    // running record count in a local and appends via the static emit_l(),
    // so its address is never taken; nrec itself remains a member solely
    // because the decode_row/replay harness wrappers hand a record count
    // from one call to the other exactly as HEAD did (sec. 2.7).
    uint16_t line[SPR_LB_LEN];
    SpanRec  rec[SPR_REC_LEN];
    int32_t  nrec = 0;

    // Decodes source row j of sprite r into line[]/rec[] (S1.2). pend is
    // x2 - r.xpos, i.e. the same value draw_sprite_fast used to choose this
    // sprite for the fast path. R1a: thin wrapper (SPRITE_R1_LLD.md sec.
    // 2.7) -- builds a SpriteCtx and calls decode_row_k, storing the
    // returned record count into the member nrec (the only place nrec is
    // written; only the driver's SpriteTestAccess::decode_row/nrec hooks and
    // this file's own replay() wrapper ever see it).
    void decode_row(const SpriteRec& r, int32_t j, int32_t pend);

    // Replays the just-decoded row's records onto destination rows [da, db)
    // of sprite r (S1.2). cx0/cx1 are the row-local clip window
    // (max(x1-xpos,0 not applied here -- callers pass x1-xpos/pend directly)
    // so every store/RMW lands only in [max(cx0,rec.pos), min(cx1,next.pos)).
    // R1a: thin wrapper -- builds a SpriteCtx (with the caller's own cx0/cx1,
    // exactly as HEAD used its arguments) and calls replay_k with the member
    // nrec set by the decode_row wrapper above (SPRITE_R1_LLD.md sec. 2.7).
    void replay(const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1);

    // SPRITE_IMPLEMENTATION_SPEC.md S4a: replay()'s two span primitives,
    // factored out so the (possibly NEON) code and the pinned-scalar _s1
    // twins below can be compared bitwise at the granularity the spec tests
    // (one span, any (lo, hi, da, db)). base/scrn_width/da/db/lo/hi have
    // exactly replay()'s meaning; both read/write only row-major halfwords
    // at base + d*scrn_width + p for p in [lo, hi), d in [da, db) -- never
    // outside that span (S4.3). replay_colour_span reads line[lo, hi);
    // replay_shadow_span is a pure read-modify-write of the destination.
    // R1a: same signature/behaviour, now one-line forwarders to the static
    // colour_span_k/shadow_span_k kernels below (SPRITE_R1_LLD.md sec. 4.3),
    // which replay_k also calls directly.
    void replay_colour_span(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi);
    void replay_shadow_span(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi);

    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S2 -- glow (PIXEL_ACCURACY), deferred
    // per sprite. replay() pushes one destination address per maximal
    // visible COLOUR run onto glow_ring (S2.2, the DESIGN_GLOW.md rule);
    // draw_sprite_fast flushes it once, after the sprite's last row, never
    // across sprites or frames (DESIGN_GLOW.md section 3's two-sprite
    // counterexample: a later sprite's shadow OR to the same address must
    // never be undone by an earlier sprite's deferred clear). A no-op when
    // glow is false: nothing is ever pushed, so glow_flush() always finds
    // glow_n == 0. draw_sprite_px implements glow inline and needs none of
    // this (S0.4).
    // ---------------------------------------------------------------------
    uint16_t* glow_ring[SPR_GLOW_RING];
    int32_t   glow_n = 0;

    // glow_push(p): defers a bit-12 clear at destination address p (S2.2).
    // Flushes first if the ring is already full.
    void glow_push(uint16_t* p);
    // glow_flush(): applies every deferred clear (*p &= 0xfff, matching
    // draw_sprite_px's glow branch) and empties the ring. Called at sprite
    // end (draw_sprite_fast) or when glow_push finds the ring full; never at
    // frame end.
    void glow_flush();

    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S3 -- per-row shadow-seen bitmap, the
    // frame-level state the S1 scratch comment above already flagged
    // ("the only per-frame state is S3's"). Bit d of shadow_rows[d >> 6] is
    // set the first time this frame a shadow OR touches destination row d
    // (replay()'s SHADOW branch, before its RMW loop -- and, for the same
    // reason, draw_sprite_px's two shadow branches: it is render()'s
    // out-of-envelope fallback within the same dispatch loop, S1.2, so its
    // shadow writes must be visible to a later fast-path sprite's push on
    // the same row just as replay()'s own are; empirically required, S3
    // task notes -- 10,000-list unit s3 failed at ~7% without it, every
    // failure on a row a fallback record had shadow-written). Bit 12 of a
    // pixel can only ever be set by such an OR, so a colour glow-clear at a
    // row whose bit is still 0 is a provable no-op and replay()'s glow push
    // skips it (S2's push, gated on this bitmap). render() resets the whole
    // bitmap at the top of every frame: all-zero when the caller proves the
    // destination's bit 12 is already clear everywhere (dest_clean, i.e.
    // video.cpp's fg_enabled -- the road foreground repainted every row
    // this frame), all-ones otherwise (every row "already touched," which
    // disables the filter -- the conservative choice whenever a stale bit
    // 12 from an earlier frame cannot be ruled out, e.g. map/music screens
    // where fg_enabled is false). 8*64 = 512 rows covers every s16_height
    // this game ships (max S16_HEIGHT*2 hires, globals.hpp) with margin.
    // ---------------------------------------------------------------------
    uint64_t shadow_rows[8];

    // -----------------------------------------------------------------------
    // SPRITE_R1_LLD.md sec. 2.6/3 -- R1a "de-glue": make_ctx builds a
    // SpriteCtx once per sprite (one sched() call, every other per-sprite
    // constant HEAD used to reread from `r`/`this`/`config` inside the
    // per-row/per-nibble loops); decode_row_k/replay_k are the row/replay
    // kernels, force-inlined into draw_sprite_rows (one real call per
    // sprite, not per row -- R0 item 2) and written to copy every ctx field
    // they use into a local at entry, so no store inside the row loop can
    // alias it back into memory (sec. 2.4 -- passing the ctx by reference is
    // not what fixes the reload; the locals are). R1a writes these
    // non-templated (R1b's <FLIP, MAXRUN, SHRINK> specialisation is a
    // separate, later step); the harness wrappers above (decode_row/replay/
    // replay_colour_span/replay_shadow_span) are unchanged in signature and
    // now just build a ctx and call these.
    // -----------------------------------------------------------------------
    void make_ctx(const SpriteRec& r, uint16_t* pixels, int32_t pend, SpriteCtx& c);

    // SPRITE_R1_LLD.md sec. 2.6/4.1 (R1b): per-sprite kernel selection.
    // K_GEN is the generic path (runtime flip, 8-byte fill, HEAD's own
    // closed-form-scan vertical loop) used for the run-0 regime
    // zoom >= 0x200; the other four cover zoom < 0x200 (MAXRUN pinned by
    // C5, sec. 4.1) crossed with flip, each using the division-free
    // vertical recurrence.
    enum Kernel : uint8_t { K_F0R2, K_F1R2, K_F0R4, K_F1R4, K_GEN };
    static Kernel select_kernel(int32_t zoom, int32_t flip);

    // emit()'s replacement (sec. 2.5): the record count is a value in/value
    // out parameter (never a member) so its address is never taken inside
    // the row loop. Same drop/merge rules, same order.
    static CB_ALWAYS_INLINE int32_t emit_l(SpanRec* rec, int32_t n, uint8_t type, int32_t pos);

    // SPRITE_R1_LLD.md sec. 4.2 (R1b): fill by shift-OR at the class width,
    // replacing HEAD's `v * 0x0001000100010001ULL` (R0 item 1 -- 9 `mul`
    // instructions per source word). Specialised for MAXRUN 2 (4-byte
    // store, zoom >= 0x100) and 4 (8-byte store, zoom < 0x100 or the
    // generic/run-0 path); no other MAXRUN is ever instantiated (sched()'s
    // assertion below pins it). `v = nibble | color <= 0xFFF`, so neither
    // specialisation needs to mask before widening.
    template <int MAXRUN> static CB_ALWAYS_INLINE void store_fill(uint16_t* p, uint32_t v);

    // prefetch_rom_row's replacement (sec. 5.1): takes the ctx instead of a
    // SpriteRec/`this`; the set of two hinted addresses is direction-
    // independent so, unlike HEAD, it does not need its own flip test.
    static CB_ALWAYS_INLINE void prefetch_rom_row_k(const SpriteCtx& c, int32_t jrow);

    // decode_row's kernel body (sec. 3.4/3.6/4.2/5.2): per-row ROM window
    // (wlimit) instead of HEAD's per-word int64 bounds test, one 8-byte
    // `run` load per word instead of eight `run[]` byte loads, fill via
    // store_fill<MAXRUN> (R1b's shift-OR, sec. 4.2) instead of HEAD's
    // multiply. FLIP is 0, 1, or 2 (= read c.flip at run time -- used only
    // by the generic/K_GEN instantiation, sec. 2.6); static (no `this`).
    template <int FLIP, int MAXRUN> static CB_ALWAYS_INLINE int32_t decode_row_k(const SpriteCtx& c, int32_t j);

    // replay's kernel body (sec. 3.7/5.4): the S3 shadow-prefetch scan
    // gated on ctx.shadow (HEAD ran it, harmlessly, for every sprite).
    // A member (not static): still owns glow_push/shadow_rows, exactly as
    // HEAD's replay() did. Not templated: one body, called from every
    // draw_sprite_rows_t instantiation (sec. 4.1).
    CB_ALWAYS_INLINE void replay_k(const SpriteCtx& c, int32_t nrec, int32_t da, int32_t db);

    // replay_colour_span/replay_shadow_span's shared bodies (sec. 4.3/5.5):
    // static so replay_k can call them without a SpriteCtx-to-`this` detour;
    // `line`/`mode` are passed explicitly since there is no `this` here (the
    // public member wrappers above pass this->line/this->replay_mode).
    // R1b: colour_span_k gains the 1..7-halfword copy ladder (sec. 4.3);
    // the n >= 8 paths (NEON and the non-NEON HEAD-verbatim fallback) are
    // unchanged from R1a.
    static CB_ALWAYS_INLINE void colour_span_k(const uint16_t* line, uint16_t* base, int32_t stride,
                                                int32_t da, int32_t db, int32_t lo, int32_t hi, int mode);
    static CB_ALWAYS_INLINE void shadow_span_k(uint16_t* base, int32_t stride,
                                                int32_t da, int32_t db, int32_t lo, int32_t hi);

    // The vertical loop (sec. 3.5/4.1/5.6): HEAD's own scan for the first
    // group, then the division-free recurrence da=db; db=min(da+run[j], d_hi)
    // for zoom < 0x200 (SHRINK == false), falling back to HEAD's scan
    // (unchanged) for zoom >= 0x200 (SHRINK == true -- the run-0 regime the
    // recurrence cannot visit). One real call per sprite from
    // draw_sprite_fast, through select_kernel's switch -- five
    // instantiations, one kernel source (sec. 4.1's deviation note: FLIP == 2
    // reads c.flip at run time for the generic instantiation instead of a
    // sixth template parameter).
    template <int FLIP, int MAXRUN, bool SHRINK> void draw_sprite_rows_t(const SpriteCtx& c);

    // The fast in-envelope path (S1.2): closed-form vertical loop
    // (src(d) = (d*zoom) >> 9) grouping destination rows that share a source
    // row, decode-once/replay-many per group. pend is passed in from
    // render()'s dispatch (already computed there to pick this path). R1a:
    // builds one SpriteCtx and calls draw_sprite_rows (sec. 3.9).
    void draw_sprite_fast(const SpriteRec& r, uint16_t* pixels, int32_t pend);

#ifdef CB_SPRITE_STATS
    // S0.9/S1.2: count of records routed to the render_px fallback (out of
    // envelope: ydelta==-1, zoom < SPR_FAST_ZOOM_MIN, or pend > SPR_PEND_MAX).
    // Expected 0 over real captured/in-game sessions (S1.2's rationale: the
    // engine's set_hrender always forces bit 15 i.e. ydelta==1, and no
    // shipping zoom table goes below 0x100 raw / 0x80 post-halving).
    long long stat_fallback = 0;

    // S2.4/S2.5 coverage counters (glow, draw_sprite_fast path only --
    // draw_sprite_px's inline glow needs none of this): ring entries
    // pushed, clears suppressed by the right-to-left/shadow-neighbour
    // amendment (S2.2, DESIGN_GLOW.md section 2), pushed clears that found
    // bit 12 already set (i.e. actually changed a value), and flushes
    // forced by the ring filling before sprite end. All expected non-zero
    // over `fuzz` at glow=true (S2.4).
    long long stat_glow_pushed = 0;
    long long stat_glow_suppressed_rtl = 0;
    long long stat_glow_changed = 0;
    long long stat_glow_flush_full = 0;

    // S3.5 coverage counter: colour glow-clears skipped because the target
    // row's shadow_rows bit was still 0 (no shadow OR has touched that row
    // this frame -- a provable no-op). Expected mostly-zero when
    // dest_clean is false (the bitmap starts all-ones, nothing is ever
    // filtered) and non-zero over real in-game frames with dest_clean true
    // (sky rows above any shadow-casting sprite).
    long long stat_shadow_bitmap_filtered = 0;
#endif

#ifdef CB_RENDER_TEST
    // SPRITE_IMPLEMENTATION_SPEC.md S4.4: pinned-scalar twins of
    // replay_colour_span/replay_shadow_span above, verbatim copies of their
    // pre-S4 (always-scalar) bodies, compiled only for the test driver. On a
    // NEON build, replay_colour_span/replay_shadow_span take the NEON branch
    // and this pair is the only remaining way to exercise "the S1 scalar
    // form" directly, so unit s4b compares the (possibly-NEON) production
    // helpers against these at every (lo, hi, da, db) rather than trusting
    // the #else branch never diverges from what these twins pin down.
    void replay_colour_span_s1(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi);
    void replay_shadow_span_s1(uint16_t* base, int32_t scrn_width, int32_t da, int32_t db, int32_t lo, int32_t hi);
#endif

#ifdef CB_RENDER_TEST
    // S1.4 negative control: when true, replay()'s right clip edge is
    // widened by a fixed, bounded margin (not disabled outright -- see the
    // long comment at its one use site in replay() for why), so some
    // stores/RMWs land past the sprite's true [x1,x2) clip, into the pad,
    // the spare rows, or a neighbour's pixels. Proves fuzz's whole-
    // allocation memcmp actually catches such overruns
    // (SPRITE_IMPLEMENTATION_SPEC.md S1.4); never set by the game, only by
    // the driver's own negative-control test.
    bool debug_disable_clip = false;
#endif

#ifdef CB_RENDER_TEST
    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S0.3: render_ref, a verbatim twin of the
    // shipped render() (hwsprites.cpp:414-1003 as it stood when this branch
    // started), with exactly one behavioural switch (ref_prepass_fix, the F1
    // fix). It needs its own copies of the two lazy side tables so the
    // driver can run it through an independent session history in lockstep
    // with the shipped render() (S0's copy, before S1 replaced it, used its
    // own sprites_flipped/sprites_shadowinfo) without either call's
    // lazy-build state contaminating the other -- selftest needs both
    // sequences to be reproducible in isolation to prove the copy's own
    // pre-pass logic (not just its draw loop) matches the shipped one. S1
    // dropped the game's own copies (SPRITE_IMPLEMENTATION_SPEC.md S1.2); the
    // twin's copies stay here, CB_RENDER_TEST only, so render_ref/selftest
    // still work unchanged.
    // ---------------------------------------------------------------------
    uint32_t sprites_flipped_ref[SPRITES_LENGTH];
    uint8_t  sprites_shadowinfo_ref[SPRITES_LENGTH];

    // F1 switch (S0.3): false reproduces the shipped renderer's
    // history-dependent pre-pass bound (y < sprite_height) bit-for-bit;
    // true widens it to y <= sprite_height, building every row the draw
    // loop can read, making the twin history-independent so it can be
    // proven equal to render_px(glow=false) on every engine-emitted record.
    bool ref_prepass_fix = false;

    void reset_ref_tables();
    void render_ref(uint16_t* pixels, uint8_t priority);

    friend struct SpriteTestAccess;
#endif
};
