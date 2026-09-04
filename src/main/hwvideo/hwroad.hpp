#pragma once

#include "stdint.hpp"
#include <vector>

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
    void (HWRoad::*render_background)(uint16_t*);
    void (HWRoad::*render_foreground)(uint16_t*);

    // S0.4: unconditional accessors (needed by the game's future -dumpram, S0.5,
    // not yet wired up here). Harmless in the shipping game either way.
    const uint16_t* road_ram_buffer() const { return ramBuff; }   // 0x800 halfwords
    uint8_t  get_road_control() const { return road_control; }

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

    // Two halves of RAM
    uint16_t ram[ROAD_RAM_SIZE / 2];
    uint16_t ramBuff[ROAD_RAM_SIZE / 2];

    void decode_road(const uint8_t*);
    void render_background_lores(uint16_t*);
    void render_foreground_lores(uint16_t*);
    void render_background_hires(uint16_t*);
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
