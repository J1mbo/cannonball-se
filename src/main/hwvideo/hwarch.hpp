#pragma once
/***************************************************************************
    Architecture detection and low-level primitives for the road/tile
    renderers (IMPLEMENTATION_SPEC.md S0.2).

    One header, included by hwroad.cpp, hwtiles.cpp and the road_tiles_test
    driver. Follows the pattern of tools/ntsc_bench/stage/WT/snes_ntsc_simd.h:
    compile-time ISA detection, a force-scalar override, every primitive
    present (as a no-op fallback where needed) on every target, so calling
    code never has to #ifdef around a missing symbol.

    Copyright (c) 2025 James Pearce.
***************************************************************************/

#include <cstdint>
#include <cstddef>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

// CB_FORCE_SCALAR: build every optimised path with its portable fallback (any host).
// Set by the road_tiles_test_scalar driver target; never defined by the game build.
#if !defined(CB_FORCE_SCALAR)
  #if defined(__aarch64__) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    #include <arm_neon.h>
    #define CB_HAVE_NEON 1               // AArch64 NEON dialect only (vzip1q etc.)
  #endif
#endif

#if defined(CB_HAVE_NEON)
  #define CB_ARCH_NAME "NEON"
#else
  #define CB_ARCH_NAME "scalar"
#endif

// ---------------------------------------------------------------------------
// SPRITE_IMPLEMENTATION_SPEC.md S0.1/S0.2: portable byte-swap and prefetch
// primitives, present (as a no-op fallback where needed) on every target so
// hwsprites.cpp never has to #ifdef around a missing symbol. cb_bswap32 is
// used by nibrev() (S0.4) on every build; cb_prefetch_r/w are unused until
// S1/S3 but declared here now so those stages need no further hwarch.hpp
// changes.
// ---------------------------------------------------------------------------
static inline uint32_t cb_bswap32(uint32_t v)
{
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
#endif
}

// Best-effort read/write prefetch hints. No-op wherever the compiler offers
// nothing (MSVC has no portable prefetch intrinsic usable from plain C++
// without target-specific headers, so it is a no-op there).
static inline void cb_prefetch_r(const void* p)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 0, 3);
#else
    (void)p;
#endif
}

static inline void cb_prefetch_w(const void* p)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 1, 3);
#else
    (void)p;
#endif
}

// Rules: 32-bit ARM (ARMv6/ARMv7, __ARM_NEON under -mfpu=neon) is deliberately
// *not* given NEON, because the tile kernel uses AArch64-only intrinsics; it
// takes the scalar path. MSVC ARM64 (_M_ARM64) is not a supported target and
// takes the scalar path. x86-64 takes the scalar path for everything (no SSE2
// port is specified; the scalar code is what MSVC ships).

// ---------------------------------------------------------------------------
// Test-only macros (documented here for reference; defined by CMake, not by
// this header):
//
//   CB_RENDER_TEST   - compiles the _ref twins and declares
//                       "friend struct RoadTilesTestAccess;" in HWRoad and
//                       hwtiles. Defined only by tools/road_tiles/test's
//                       CMakeLists.txt; never by the game.
//   CB_RENDER_CHECKS - enables the (future, S3) debug assertions in
//                       Video::prepare_frame. CMake option RENDER_CHECKS,
//                       default OFF for the game; the driver always defines it.
//   CB_RAM_DUMP      - compiles the (future, S0.5) -dumpram capture. CMake
//                       option RAM_DUMP, default OFF.
//   CB_FRAME_CKSUM   - already exists (video.cpp, main.cpp); the whole-frame
//                       smoke test.
//
// Sprite renderer (SPRITE_IMPLEMENTATION_SPEC.md), also declared only by
// CMake, never hardcoded here:
//   CB_RENDER_TEST      - also compiles hwsprites' render_ref twin/render_px
//                          oracle test-only members and declares
//                          "friend struct SpriteTestAccess;". Defined only by
//                          tools/sprite_bench/test's CMakeLists.txt.
//   CB_SPR_DUMP         - compiles the -dumpspr capture (S0.6). CMake option
//                          SPRITE_DUMP, default OFF.
//   CB_SPRITE_REF_PX    - Video::prepare_frame() routes the sprite pass
//                          through hwsprites::render_px instead of render()
//                          (S0.8 whole-game reference gate). CMake option
//                          SPRITE_REF_PX, default OFF.
//   CB_SPRITE_STATS     - per-frame sprite counters (S0.9). Driver always;
//                          game via -DCB_SPRITE_STATS=1.
// ---------------------------------------------------------------------------
