#pragma once

#include "stdint.hpp"
#include "globals.hpp"      // S16_HEIGHT (S3: foreground_row_mask/check_foreground_rows signatures)
#include <vector>

// ---------------------------------------------------------------------------
// S3.2: shared road-foreground/background row predicates. Free functions (not
// members) so hwroad.cpp, video.cpp/prepare_frame's debug check and the
// standalone test driver all read exactly one definition -- they cannot
// drift apart. Mirrors the foreground's both-solid skip (hwroad.cpp:631-635,
// plus the per-mode continues at 715-716/776-777) and the background's sky
// fill switch (hwroad.cpp:534-559 lores numbering; 600-625 hires).
// ---------------------------------------------------------------------------

// True iff the foreground pass writes lo-res line yy (hi-res rows 2yy, 2yy+1) given the two
// road RAM words and the control mode.  Mirrors hwroad.cpp:631-635, 715-716, 776-777.
static inline bool road_row_written(unsigned mode, unsigned d0, unsigned d1) {
    const bool s0 = (d0 & 0x800u) != 0, s1 = (d1 & 0x800u) != 0;
    switch (mode & 3u) { case 0: return !s0; case 3: return !s1; default: return !(s0 && s1); }
}
// True iff the background pass fills the line.  Mirrors hwroad.cpp:534-559.
static inline bool road_row_sky(unsigned mode, unsigned d0, unsigned d1) {
    const bool s0 = (d0 & 0x800u) != 0, s1 = (d1 & 0x800u) != 0;
    switch (mode & 3u) { case 0: return s0; case 3: return s1; default: return s0 || s1; }
}

class HWRoad
{
public:
    HWRoad();
    ~HWRoad();

    void init(const uint8_t*, const bool hires);
    inline void write16(uint32_t adr, const uint16_t data) {
        ram[(adr >> 1) & 0x7FF] = data;
    };
    inline void write16(uint32_t* adr, const uint16_t data) {
        uint32_t a = *adr;
        ram[(a >> 1) & 0x7FF] = data;
        *adr += 2;
    };
    inline void write32(uint32_t* adr, const uint32_t data) {
        uint32_t a = (*adr) >> 1;
        ram[a & 0x7FF] = data >> 16;
        ram[(a + 1) & 0x7FF] = data & 0xFFFF;
        *adr += 4;
    };
    uint16_t read_road_control();
    void write_road_control(const uint8_t);
    // S1: render_background now also takes road_fg_rows (see the private
    // declarations below) -- every caller (video.cpp, the road_tiles_test
    // driver) already computes/holds that mask before calling through this
    // pointer, so this is the only call-site change #1 requires.
    void (HWRoad::*render_background)(uint16_t*, const uint8_t*);
    void (HWRoad::*render_foreground)(uint16_t*);

    // S0.4: unconditional accessors (needed by the game's future -dumpram, S0.5,
    // not yet wired up here). Harmless in the shipping game either way.
    const uint16_t* road_ram_buffer() const { return ramBuff; }   // 0x800 halfwords
    uint8_t  get_road_control() const { return road_control; }

    // S3.2: out[yy] = 1 iff the foreground will write lo-res line yy this
    // frame; all zero when !fg_enabled. Derived from ramBuff (the buffer the
    // renderers actually read), road RAM cannot change between this call and
    // the foreground render (ORoad::do_road runs earlier in tick(), on the
    // same thread -- IMPLEMENTATION_SPEC.md S3.2).
    void foreground_row_mask(uint8_t out[S16_HEIGHT], bool fg_enabled) const;

    // S3.2: compiled always (called only under CB_RENDER_CHECKS and by the
    // driver's unit s3). Renders the foreground into `scratch` (same
    // size/alignment as `pixels`, this function pre-fills it with 0xFFFF --
    // a value no road colour can take: color_offset1/2 are 0x400/0x420) and
    // verifies, per lo-res line, that every hi-res row of the line is
    // written in full iff mask[yy] == 1 and untouched iff mask[yy] == 0;
    // also verifies road_row_sky || road_row_written (coverage) for every
    // line. Returns the number of violating lines (0 == pass) and prints the
    // first violation to stderr. `rows_both` (if non-null) receives the
    // count of lines where the background also fills (statistic, not a
    // failure).
    int check_foreground_rows(const uint8_t mask[S16_HEIGHT], bool fg_enabled,
                               uint16_t* scratch, int* rows_both);

#ifdef CB_RENDER_TEST
    // S0.4: lets the standalone test driver (tools/road_tiles/test) reach
    // private state and the _ref reference twins below without
    // "#define private public". Never defined by the game build.
    friend struct RoadTilesTestAccess;
#endif

private:
    uint8_t road_control;
    uint16_t color_offset1;
    uint16_t color_offset2;
    uint16_t color_offset3;
    int32_t x_offset;

    static const uint16_t ROAD_RAM_SIZE = 0x1000;
    static const uint16_t rom_size = 0x8000;

    // Decoded road graphics
    uint8_t roads[0x40200];

    // S6.2: run-length table over roads[] -- one packed uint16_t per maximal
    // run of equal texel value per line, plus a start=512 sentinel entry.
    // 513 lines: 0..255 road 0 candidate lines, 256..511 road 1 candidate
    // lines, 512 the dummy all-3 line (decode_road, roads[0x40200] =
    // 513 * 512 bytes). decode_road is the only writer of roads[]; it rebuilds
    // this table every time it runs. Entry = start (bits 0-9, 0..512) |
    // value << 12 (bits 12-14).
    static const int ROAD_LINES = 513;
    std::vector<uint16_t> run_tab;
    uint32_t run_first[ROAD_LINES + 1];  // run_tab index of line L's first entry; [ROAD_LINES] = total

    // Two halves of RAM. Per-frame memory-work Tier 1 #4: physical storage
    // for the double-buffer, backing the `ram`/`ramBuff` pointer pair below
    // instead of being the write/read buffers directly -- read_road_control()
    // (hwroad.cpp) flips the two pointers instead of exchanging every
    // element, so a frame's write-then-swap costs O(1) instead of O(RAM
    // size). Indices into ram_store are otherwise never used directly.
    uint16_t ram_store[2][ROAD_RAM_SIZE / 2];
    // Writes (write16/write32 below) always target `ram`; every render_*
    // function and the public road_ram_buffer()/foreground_row_mask/
    // check_foreground_rows accessors always read `ramBuff` -- exactly the
    // roles the two names had when they were the arrays themselves.
    // read_road_control() swaps which physical buffer each name points at.
    uint16_t* ram     = ram_store[0];
    uint16_t* ramBuff = ram_store[1];

    void decode_road(const uint8_t*);
    // S1 (per-frame memory-work Tier 1 #1): fg_rows[yy] (S16_HEIGHT entries,
    // lo-res line indexed) is road_fg_rows as computed by
    // HWRoad::foreground_row_mask -- 1 iff the foreground pass will fully
    // repaint lo-res line yy this frame, in which case the sky fill below is
    // skipped for that line (proved safe by check_foreground_rows/unit s3:
    // whenever the mask is set, render_foreground overwrites the line in
    // full, so whatever the background leaves there is never seen). Always a
    // valid S16_HEIGHT array from every real call site (video.cpp computes
    // it unconditionally, all-zero when the foreground is disabled).
    void render_background_lores(uint16_t* pixels, const uint8_t* fg_rows);
    void render_foreground_lores(uint16_t*);
    void render_background_hires(uint16_t* pixels, const uint8_t* fg_rows);
    void render_foreground_hires(uint16_t*);

    // S6.2: run-length row kernel used by render_foreground_hires's four
    // cases. pairs = config.s16_width >> 1; mode = road_control & 3; src0/src1
    // are the line base pointers computed by the (untouched) setup code;
    // t0/t1 are (hposN - (s16_x + x_offset)) & 0xfff exactly as computed
    // at each call site.
    void render_row_runs(uint32_t* out32, int pairs, unsigned mode,
                          const uint8_t* src0, const uint8_t* src1,
                          uint32_t t0, uint32_t t1, const uint16_t* color_table);

#ifdef CB_RENDER_TEST
    // S0.3: verbatim reference twins of the four functions above, as they
    // stood before any optimisation section was applied. Not compiled into
    // the game; every future section is checked against these.
    void render_background_lores_ref(uint16_t*);
    void render_foreground_lores_ref(uint16_t*);
    void render_background_hires_ref(uint16_t*);
    void render_foreground_hires_ref(uint16_t*);
#endif
};

extern HWRoad hwroad;
