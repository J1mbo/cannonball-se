#pragma once
/***************************************************************************
    Architecture detection and low-level primitives for the road/tile
    renderers (IMPLEMENTATION_SPEC.md S0.2).

    One header, included by hwroad.cpp, hwtiles.cpp and the road_tiles_test
    driver. Follows the pattern of tools/ntsc_bench/stage/WT/snes_ntsc_simd.h:
    compile-time ISA detection, a force-scalar override, every primitive
    present (as a no-op fallback where needed) on every target, so calling
    code never has to #ifdef around a missing symbol.

    Modifications for Cannonball-SE are Copyright (c) 2026 James Pearce.
***************************************************************************/

#include <cstdint>
#include <cstddef>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

// CB_FORCE_SCALAR: build every optimised path with its portable fallback (any host).
// Set by the road_tiles_test_scalar driver target; never defined by the game build.
//
// R2 (SPRITE_REPLAN_FABLE.md sec. 5.2): two-tier NEON detection.
//   CB_HAVE_NEON      - the portable ARMv7/AArch64 NEON subset. Defined whenever the
//                        compiler offers NEON at all: AArch64 always (when not force-
//                        scalar); AArch32 only when built with -mfpu=neon* or an
//                        -march=...+simd that turns NEON on (Raspberry Pi OS armhf
//                        defaults to ARMv6 with no NEON, so this is off there unless
//                        asked for). Code under plain CB_HAVE_NEON must compile on
//                        both dialects.
//   CB_HAVE_NEON_A64  - the AArch64-only dialect on top of CB_HAVE_NEON (vzip1q_u16
//                        etc.). Anything gated on this needs a CB_HAVE_NEON-only
//                        alternative beside it (see the cb_zip* shims below).
#if !defined(CB_FORCE_SCALAR)
  #if defined(__ARM_NEON) || defined(__ARM_NEON__)   // AArch64: always. AArch32: only
    #include <arm_neon.h>                            //   with -mfpu=neon* or -march=...+simd
    #define CB_HAVE_NEON 1               // portable ARMv7/AArch64 subset only
    #if defined(__aarch64__)
      #define CB_HAVE_NEON_A64 1         // AArch64-only dialect, if ever needed
    #endif
  #endif
#endif

#if defined(CB_HAVE_NEON_A64)
  #define CB_ARCH_NAME "NEON-A64"
#elif defined(CB_HAVE_NEON)
  #define CB_ARCH_NAME "NEON-A32"
#else
  #define CB_ARCH_NAME "scalar"
#endif

#if defined(CB_HAVE_NEON)
// ---------------------------------------------------------------------------
// Zip shims for the three AArch64-only intrinsics hwtiles.cpp's tile_row_hires
// uses (SPRITE_REPLAN_FABLE.md sec. 5.2). On A64 these resolve to the direct
// AArch64 zip instructions (identical codegen to before this header changed);
// on A32 they fall back to the combined vzipq/vzip result-pair form, which
// AArch32 NEON offers instead of separate zip1/zip2 instructions.
// ---------------------------------------------------------------------------
static inline uint16x8_t cb_zip1q_u16(uint16x8_t a, uint16x8_t b)
{
#if defined(CB_HAVE_NEON_A64)
    return vzip1q_u16(a, b);
#else
    return vzipq_u16(a, b).val[0];
#endif
}

static inline uint16x8_t cb_zip2q_u16(uint16x8_t a, uint16x8_t b)
{
#if defined(CB_HAVE_NEON_A64)
    return vzip2q_u16(a, b);
#else
    return vzipq_u16(a, b).val[1];
#endif
}

static inline uint8x8_t cb_zip1_u8(uint8x8_t a, uint8x8_t b)
{
#if defined(CB_HAVE_NEON_A64)
    return vzip1_u8(a, b);
#else
    return vzip_u8(a, b).val[0];
#endif
}
#endif // CB_HAVE_NEON

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

// ---------------------------------------------------------------------------
// SPRITE_R1_LLD.md sec. 2.1: force-inline and loop-unroll hints. Present on
// every target (no-op fallback where the compiler offers nothing) so calling
// code never has to #ifdef around a missing symbol. always_inline is needed
// because R1's decode_row_k/replay_k kernels must not become out-of-line
// calls from the per-sprite row loop -- both -O2 (the standalone driver) and
// -O3 -Ofast (the game) decline to inline them on their own (measured, see
// PERFORMANCE_MEASUREMENTS.md campaign 6.1, R0a static analysis: objdump on
// the pre-R1 S4 binary showed decode_row/replay called via real bl, not
// inlined). CB_UNROLL(n) keeps the driver's -O2 build
// structurally aligned with the game's -O3 build for the fixed 8-nibble
// decode loop; GCC 14 does not unroll it at -O2 without the pragma.
// ---------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
  #define CB_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
  #define CB_ALWAYS_INLINE __forceinline
#else
  #define CB_ALWAYS_INLINE inline
#endif
#define CB_STR_(x) #x
#if defined(__clang__)
  #define CB_UNROLL(n) _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
  #define CB_UNROLL(n) _Pragma(CB_STR_(GCC unroll n))
#else
  #define CB_UNROLL(n)
#endif

// Rules: 32-bit ARM (ARMv7 built with -mfpu=neon*/-march=...+simd, e.g. Cortex-A7)
// now gets CB_HAVE_NEON via the cb_zip* shims above (R2); ARMv6 with no NEON still
// takes the scalar path, as does any AArch32 build without __ARM_NEON defined.
// MSVC ARM64 (_M_ARM64) is not a supported target and takes the scalar path (MSVC
// does not define __ARM_NEON/__ARM_NEON__, so it falls through the same gate).
// x86-64 takes the scalar path for everything (no SSE2 port is specified; the
// scalar code is what MSVC ships).

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
