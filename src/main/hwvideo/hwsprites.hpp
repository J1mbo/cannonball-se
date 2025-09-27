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
    uint8_t read(const uint16_t adr) {
        uint8_t a = ram[adr >> 1];
        if ((adr & 1) == 1)
            return a & 0xff;
        else
            return a >> 8;
    }
    void write(const uint16_t adr, const uint16_t data) {
        ram[adr >> 1] = data;
    }
    void render(const uint8_t);

private:
    // Clip values.
    uint16_t x1, x2;

    // 128 sprites, 16 bytes each (0x400)
    static const uint16_t SPRITE_RAM_SIZE = 128 * 8;
    static const uint32_t SPRITES_LENGTH = 0x100000 >> 2;
    static const uint16_t COLOR_BASE = 0x800;

    alignas(64) uint32_t sprites[SPRITES_LENGTH]; // Converted sprites

    // Two halves of RAM
    alignas(64) uint16_t ram[SPRITE_RAM_SIZE];
    alignas(64) uint16_t ramBuff[SPRITE_RAM_SIZE];
};

