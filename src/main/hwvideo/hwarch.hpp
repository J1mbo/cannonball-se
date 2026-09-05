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
// ---------------------------------------------------------------------------
