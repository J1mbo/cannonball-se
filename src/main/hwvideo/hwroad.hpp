#pragma once

#include "stdint.hpp"

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

    // Two halves of RAM
    uint16_t ram[ROAD_RAM_SIZE / 2];
    uint16_t ramBuff[ROAD_RAM_SIZE / 2];

    void decode_road(const uint8_t*);
    void render_background_lores(uint16_t*);
    void render_foreground_lores(uint16_t*);
    void render_background_hires(uint16_t*);
    void render_foreground_hires(uint16_t*);

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
