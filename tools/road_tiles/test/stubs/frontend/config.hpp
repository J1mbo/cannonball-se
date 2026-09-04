#pragma once
// Minimal stand-in for src/main/frontend/config.hpp (IMPLEMENTATION_SPEC.md S0.6).
//
// hwroad.cpp and hwtiles.cpp reference only config.s16_width, config.s16_height
// and config.s16_x_off (verified by grep -n "config\\." over both files before
// writing this stub). This directory is placed FIRST on the include path by
// tools/road_tiles/test/CMakeLists.txt, so this header shadows the real one and
// the road_tiles_test driver never needs SDL2/TinyXML2. If a later section adds
// another config member to either .cpp, add it here too.
#include "stdint.hpp"
struct Config { uint16_t s16_width, s16_height, s16_x_off; };
extern Config config;
