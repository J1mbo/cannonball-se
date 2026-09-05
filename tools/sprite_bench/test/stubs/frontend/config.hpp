#pragma once
// Minimal stand-in for src/main/frontend/config.hpp
// (SPRITE_IMPLEMENTATION_SPEC.md S0.7, verbatim from that section).
//
// hwsprites.cpp/.hpp reference exactly config.s16_width, config.s16_x_off,
// config.s16_height, config.video.hires and config.video.hiresprites
// (verified by grep -n "config\\." over both files before writing this
// stub -- see the S0 task report). This directory is placed FIRST on the
// include path by tools/sprite_bench/test/CMakeLists.txt, so this header
// shadows the real one and the sprite_test driver never needs SDL2/
// TinyXML2. If a later stage adds another config member to either file,
// add it here too.
#include "stdint.hpp"
struct Config { uint16_t s16_width, s16_height, s16_x_off; struct { int hires; int hiresprites; } video; };
extern Config config;
