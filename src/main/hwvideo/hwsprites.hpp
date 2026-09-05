#pragma once

#include "stdint.hpp"
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

    // Span record type. GAP produces no output and is never replayed;
    // COLOUR is a store-only run; SHADOW is an inline read-modify-write run.
    // "flags" is reserved (S2's glow amendment uses it later); zero in S1.
    enum : uint8_t { SPR_GAP = 0, SPR_COLOUR = 1, SPR_SHADOW = 2 };
    struct SpanRec { uint16_t pos; uint8_t type; uint8_t flags; };

    // sched(zoom): run[SPR_SCHED_LEN] for this (already post-halving) zoom,
    // built on first use by the reference accumulator (S1.2) and cached.
    // zoom is 12 bits (parse_record masks with 0xfff), so 0x1000 slots covers
    // every value; about 160 are ever populated in a real session.
    const uint8_t* sched(uint32_t zoom);
    std::unique_ptr<uint8_t[]> sched_tab[0x1000];

    // Per-row decode/replay scratch. Spec fact: "line, rec, nrec are per
    // row; the only per-frame state is S3's" -- kept as members purely so
    // decode_row/replay/emit (and the S0.5 test hooks) can share them
    // without threading extra parameters; nothing here survives, or needs
    // to survive, past the sprite currently being drawn.
    uint16_t line[SPR_LB_LEN];
    SpanRec  rec[SPR_REC_LEN];
    int32_t  nrec = 0;

    // Appends a span-type change at output position pos to rec[], applying
    // S1.2's drop (zero-length) and merge (equal-to-previous) rules so
    // records are always maximal runs of one type (S2's glow depends on
    // this).
    void emit(uint8_t type, int32_t pos);

    // Decodes source row j of sprite r into line[]/rec[] (S1.2). pend is
    // x2 - r.xpos, i.e. the same value draw_sprite_fast used to choose this
    // sprite for the fast path.
    void decode_row(const SpriteRec& r, int32_t j, int32_t pend);

    // Replays the just-decoded row's records onto destination rows [da, db)
    // of sprite r (S1.2). cx0/cx1 are the row-local clip window
    // (max(x1-xpos,0 not applied here -- callers pass x1-xpos/pend directly)
    // so every store/RMW lands only in [max(cx0,rec.pos), min(cx1,next.pos)).
    void replay(const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1);

    // Bounds-checked read prefetch of ROM row jrow's first and last word
    // (S1.2: "the words at k = 0 and k = pitch - 1"), mirrored for flip so
    // it always names the two words decode_row will actually touch first/
    // last for that row.
    void prefetch_rom_row(const SpriteRec& r, int32_t jrow) const;

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

    // The fast in-envelope path (S1.2): closed-form vertical loop
    // (src(d) = (d*zoom) >> 9) grouping destination rows that share a source
    // row, decode-once/replay-many per group. pend is passed in from
    // render()'s dispatch (already computed there to pick this path).
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
