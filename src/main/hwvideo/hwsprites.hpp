#pragma once

#include "stdint.hpp"

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
    void render(uint16_t* pixels, const uint8_t);

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
    uint32_t sprites_flipped[SPRITES_LENGTH];       // Little-endian flipped sprites
    // sprites_shadowinfo contains, at the start address of each sprite,
    // - 0xff for each row that has not yest been processed
    // - 0x11 for each row that contains a shadow entry (0xA);
    // - 0x00 otherwise
    // This helps with rendering as we can use a lower-cost routine 90% of the time
    uint8_t sprites_shadowinfo[SPRITES_LENGTH];

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

#ifdef CB_RENDER_TEST
    // ---------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S0.3: render_ref, a verbatim twin of the
    // shipped render() (hwsprites.cpp:414-1003 as it stood when this branch
    // started), with exactly one behavioural switch (ref_prepass_fix, the F1
    // fix). It needs its own copies of the two lazy side tables so the
    // driver can run it through an independent session history in lockstep
    // with the shipped render() (which uses sprites_flipped/
    // sprites_shadowinfo above) without either call's lazy-build state
    // contaminating the other -- selftest needs both sequences to be
    // reproducible in isolation to prove the copy's own pre-pass logic (not
    // just its draw loop) matches the shipped one. Moved here, still under
    // this guard, is where S1 will relocate the game's own copies once
    // render() itself is replaced and no longer needs them.
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
