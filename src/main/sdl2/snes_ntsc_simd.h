/* Portable 128-bit-wide (4 x uint32) SIMD layer for the NTSC filter re-architecture.

Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.

Part D of tools/ntsc_bench/analysis/ARCHITECTURE.md (section 7): a small header giving
snes_ntsc_blit_hires_vec() (see snes_ntsc.cpp) one v4u32 type and a dozen operations on
it - unaligned/aligned load, aligned/unaligned store, splat, add, sub, and, or, andnot,
shift-by-immediate, zero - with NEON, SSE2 and RVV backends plus a plain-C fallback used when none
of those is available, or when SNES_NTSC_FORCE_SCALAR_SIMD is defined (forces the
fallback even on a machine that has real SIMD, for testing it anywhere).

This header is C++-only (the shift-by-immediate operations are templates, since several
of the underlying intrinsics require an actual compile-time-constant immediate operand,
not just a runtime value that happens to be constant-propagated). It is included only
from snes_ntsc.cpp, never from snes_ntsc.h, so the public C-linkage header stays free of
this file's C++-only content; snes_ntsc.h only forward-declares
snes_ntsc_blit_hires_vec().

Note this layer's ISA detection is independent of SNES_NTSC_HAVE_SIMD /
SNES_NTSC_X86_LEVEL in snes_ntsc.h: it always resolves to *something* (falling back to
plain C when no vector ISA is available), which is what makes
snes_ntsc_blit_hires_vec() buildable and correct on every target, including ones where
snes_ntsc_blit_hires_fast() does not exist at all (e.g. ARMv6 / Pi Zero W). Unlike
snes_ntsc.h's x86 detection, this layer never distinguishes an SSE4.1-capable host from a
plain SSE2 one - the operations it needs (unaligned load, add/sub/and/or, shift-by-
immediate) are all baseline SSE2, so SNES_NTSC_SIMD_BACKEND_NAME reports "SSE2" on every
x86-64 host regardless of -march. */

#ifndef SNES_NTSC_SIMD_H
#define SNES_NTSC_SIMD_H

#include <stdint.h>

/* ---- ISA detection (mirrors snes_ntsc.h's, but layer-local and SSE2-only on x86) ---- */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
    #define SNES_NTSC_SIMD_HAVE_NEON 1
#elif defined(_M_X64) || defined(__x86_64__)
    #if defined(_MSC_VER)
        #include <immintrin.h>
    #else
        #include <emmintrin.h>
    #endif
    #define SNES_NTSC_SIMD_HAVE_SSE2 1
#elif defined(__i386__) || defined(_M_IX86)
    #if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        #if defined(_MSC_VER)
            #include <immintrin.h>
        #else
            #include <emmintrin.h>
        #endif
        #define SNES_NTSC_SIMD_HAVE_SSE2 1
    #endif
#elif defined(__riscv) && defined(__riscv_v)
    #include <string.h> /* memcpy: RVV backend loads/stores */
    #define SNES_NTSC_SIMD_HAVE_RVV 1
#endif

/* SNES_NTSC_FORCE_SCALAR_SIMD overrides whatever was detected above, so the plain-C
   fallback can be built and benchmarked on any machine regardless of its real ISA
   (see run_host.sh / run_pi.sh's "_vec_scalar" function-name mapping). */
#if defined(SNES_NTSC_FORCE_SCALAR_SIMD)
    #undef SNES_NTSC_SIMD_HAVE_NEON
    #undef SNES_NTSC_SIMD_HAVE_SSE2
    #undef SNES_NTSC_SIMD_HAVE_RVV
#endif

/* ==================================================================================== */

#if defined(SNES_NTSC_SIMD_HAVE_NEON)

#define SNES_NTSC_SIMD_BACKEND_NAME "NEON"
typedef uint32x4_t v4u32;

static inline v4u32 snes_v4_zero(void)                        { return vdupq_n_u32(0); }
static inline v4u32 snes_v4_splat(uint32_t x)                  { return vdupq_n_u32(x); }
static inline v4u32 snes_v4_load_u(const uint32_t* p)          { return vld1q_u32(p); }
static inline v4u32 snes_v4_load_a(const uint32_t* p)          { return vld1q_u32(p); }
static inline void  snes_v4_store_a(uint32_t* p, v4u32 v)       { vst1q_u32(p, v); }
static inline void  snes_v4_store_u(uint32_t* p, v4u32 v)       { vst1q_u32(p, v); } /* NEON has no unaligned-store penalty/requirement */
static inline v4u32 snes_v4_add(v4u32 a, v4u32 b)               { return vaddq_u32(a, b); }
static inline v4u32 snes_v4_sub(v4u32 a, v4u32 b)               { return vsubq_u32(a, b); }
static inline v4u32 snes_v4_and(v4u32 a, v4u32 b)               { return vandq_u32(a, b); }
static inline v4u32 snes_v4_or(v4u32 a, v4u32 b)                { return vorrq_u32(a, b); }
static inline v4u32 snes_v4_andnot(v4u32 a, v4u32 b)            { return vbicq_u32(b, a); } /* (~a) & b */
template<int N> static inline v4u32 snes_v4_shr_imm(v4u32 v)   { return vshrq_n_u32(v, N); }
template<int N> static inline v4u32 snes_v4_shl_imm(v4u32 v)   { return vshlq_n_u32(v, N); }

#elif defined(SNES_NTSC_SIMD_HAVE_SSE2)

#define SNES_NTSC_SIMD_BACKEND_NAME "SSE2"
typedef __m128i v4u32;

static inline v4u32 snes_v4_zero(void)                        { return _mm_setzero_si128(); }
static inline v4u32 snes_v4_splat(uint32_t x)                  { return _mm_set1_epi32((int)x); }
static inline v4u32 snes_v4_load_u(const uint32_t* p)           { return _mm_loadu_si128((const __m128i*)p); }
static inline v4u32 snes_v4_load_a(const uint32_t* p)           { return _mm_load_si128((const __m128i*)p); }
static inline void  snes_v4_store_a(uint32_t* p, v4u32 v)       { _mm_store_si128((__m128i*)p, v); }
static inline void  snes_v4_store_u(uint32_t* p, v4u32 v)       { _mm_storeu_si128((__m128i*)p, v); }
static inline v4u32 snes_v4_add(v4u32 a, v4u32 b)               { return _mm_add_epi32(a, b); }
static inline v4u32 snes_v4_sub(v4u32 a, v4u32 b)               { return _mm_sub_epi32(a, b); }
static inline v4u32 snes_v4_and(v4u32 a, v4u32 b)               { return _mm_and_si128(a, b); }
static inline v4u32 snes_v4_or(v4u32 a, v4u32 b)                { return _mm_or_si128(a, b); }
static inline v4u32 snes_v4_andnot(v4u32 a, v4u32 b)            { return _mm_andnot_si128(a, b); } /* (~a) & b */
template<int N> static inline v4u32 snes_v4_shr_imm(v4u32 v)   { return _mm_srli_epi32(v, N); }
template<int N> static inline v4u32 snes_v4_shl_imm(v4u32 v)   { return _mm_slli_epi32(v, N); }

#elif defined(SNES_NTSC_SIMD_HAVE_RVV)

#define SNES_NTSC_SIMD_BACKEND_NAME "RVV"
/* Not vuint32m1_t: RVV intrinsic types are sizeless, so they cannot be array elements
   or struct members, and snes_ntsc_blit_hires_vec() keeps an accumulator array
   (v4u32 A[10]). A fixed 16-byte GCC/Clang vector is a normal sized type; with the V
   extension enabled the compiler lowers it to fixed-length RVV code (vle32/vadd/vsrl/
   vse32 with vl=4). It needs VLEN >= 128, which V itself guarantees (Zvl128b). */
typedef uint32_t v4u32 __attribute__((vector_size(16)));

static inline v4u32 snes_v4_zero(void)                        { v4u32 r = {0u, 0u, 0u, 0u}; return r; }
static inline v4u32 snes_v4_splat(uint32_t x)                  { v4u32 r = {x, x, x, x}; return r; }
static inline v4u32 snes_v4_load_u(const uint32_t* p)           { v4u32 r; memcpy(&r, p, sizeof r); return r; }
static inline v4u32 snes_v4_load_a(const uint32_t* p)           { return snes_v4_load_u(p); }
static inline void  snes_v4_store_a(uint32_t* p, v4u32 v)       { memcpy(p, &v, sizeof v); }
static inline void  snes_v4_store_u(uint32_t* p, v4u32 v)       { memcpy(p, &v, sizeof v); }
static inline v4u32 snes_v4_add(v4u32 a, v4u32 b)               { return a + b; }
static inline v4u32 snes_v4_sub(v4u32 a, v4u32 b)               { return a - b; }
static inline v4u32 snes_v4_and(v4u32 a, v4u32 b)               { return a & b; }
static inline v4u32 snes_v4_or(v4u32 a, v4u32 b)                { return a | b; }
static inline v4u32 snes_v4_andnot(v4u32 a, v4u32 b) /* (~a) & b */ { return ~a & b; }
template<int N> static inline v4u32 snes_v4_shr_imm(v4u32 v)   { return v >> N; }
template<int N> static inline v4u32 snes_v4_shl_imm(v4u32 v)   { return v << N; }

#else /* plain-C fallback: no vector ISA detected, or SNES_NTSC_FORCE_SCALAR_SIMD */

#define SNES_NTSC_SIMD_BACKEND_NAME "scalar"

struct v4u32 { uint32_t v[4]; };

static inline v4u32 snes_v4_zero(void) { v4u32 r; r.v[0]=r.v[1]=r.v[2]=r.v[3]=0u; return r; }
static inline v4u32 snes_v4_splat(uint32_t x) { v4u32 r; r.v[0]=r.v[1]=r.v[2]=r.v[3]=x; return r; }
static inline v4u32 snes_v4_load_u(const uint32_t* p) {
    v4u32 r; r.v[0]=p[0]; r.v[1]=p[1]; r.v[2]=p[2]; r.v[3]=p[3]; return r;
}
static inline v4u32 snes_v4_load_a(const uint32_t* p) { return snes_v4_load_u(p); }
static inline void  snes_v4_store_a(uint32_t* p, v4u32 v) {
    p[0]=v.v[0]; p[1]=v.v[1]; p[2]=v.v[2]; p[3]=v.v[3];
}
static inline void  snes_v4_store_u(uint32_t* p, v4u32 v) { snes_v4_store_a(p, v); }
static inline v4u32 snes_v4_add(v4u32 a, v4u32 b) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]+b.v[i]; return r; /* wraps mod 2^32, same as scalar */
}
static inline v4u32 snes_v4_sub(v4u32 a, v4u32 b) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]-b.v[i]; return r;
}
static inline v4u32 snes_v4_and(v4u32 a, v4u32 b) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]&b.v[i]; return r;
}
static inline v4u32 snes_v4_or(v4u32 a, v4u32 b) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]|b.v[i]; return r;
}
static inline v4u32 snes_v4_andnot(v4u32 a, v4u32 b) { /* (~a) & b */
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=(~a.v[i])&b.v[i]; return r;
}
template<int N> static inline v4u32 snes_v4_shr_imm(v4u32 a) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]>>N; return r;
}
template<int N> static inline v4u32 snes_v4_shl_imm(v4u32 a) {
    v4u32 r; for (int i=0;i<4;++i) r.v[i]=a.v[i]<<N; return r;
}

#endif

/* ==================================================================================== */
/* Clamp + convert, generic in terms of the v4u32 primitives above only. Mirrors
   SNES_NTSC_CLAMP_ (shift 0) followed by the scalar SNES_NTSC_RGB_OUT_ 24-bit sequence in
   snes_ntsc.h bit for bit - but not via the same route any more (Stage 3 review): instead
   of building an ABGR word and then bswap32'ing it (four masked shifts/ors for the swap
   alone), the final little-endian RGBA word is produced directly from the clamped value
   `io` in one pass:
       rgba = ((io >> 21) & 0xFF) | ((io >> 3) & 0xFF00) | ((io << 15) & 0xFF0000) | alpha_vec
   This is the same bit-for-bit result as the old route (verified by tracing each byte
   through the old shift-mask-or-then-bswap sequence: the old R byte, after the swap,
   equals bits [21:28] of the clamped `io`, i.e. exactly (io>>21)&0xFF; likewise G is
   (io>>3)&0xFF00 and B is (io<<15)&0xFF0000) but needs 3 shifts + 3 ands + 3 ors instead
   of the old ~9 (3 shifts + 3 ands + 3 ors for the ABGR build) + ~11 (4 ands + 4 shifts +
   3 ors for the bswap) = ~20 total. `alpha_vec` is Alevel already shifted into bit
   31:24 and splat, i.e. snes_v4_splat((uint32_t)Alevel << 24) - hoisted out of the
   caller's row loop (it is a per-call constant), along with `clamp_mask_vec`/
   `clamp_add_vec` (also per-call constants, previously re-splatted on every single
   invocation). Alevel is not masked here either (matches the scalar macro) - so, as
   with the scalar macro, Alevel must fit in the low 8 bits (<= 255): a wider Alevel
   would corrupt the R/G/B bits through this same unmasked-OR mechanism, in either the
   old route or this one, since `alpha_vec`'s only guaranteed-zero bits are 23:0. */
static inline v4u32 snes_ntsc_simd_clamp_convert(v4u32 io, v4u32 clamp_mask_vec, v4u32 clamp_add_vec,
                                                  v4u32 alpha_vec)
{
    /* ---- clamp (SNES_NTSC_CLAMP_, shift 0) ---- */
    v4u32 sub   = snes_v4_and(snes_v4_shr_imm<9>(io), clamp_mask_vec);
    v4u32 clamp = snes_v4_sub(clamp_add_vec, sub);
    io = snes_v4_or(io, clamp);
    clamp = snes_v4_sub(clamp, sub);
    io = snes_v4_and(io, clamp);

    /* ---- convert clamped value directly to little-endian RGBA (see comment above) ---- */
    v4u32 r = snes_v4_and(snes_v4_shr_imm<21>(io), snes_v4_splat(0x000000FFu));
    v4u32 g = snes_v4_and(snes_v4_shr_imm<3>(io),  snes_v4_splat(0x0000FF00u));
    v4u32 b = snes_v4_and(snes_v4_shl_imm<15>(io), snes_v4_splat(0x00FF0000u));
    return snes_v4_or(snes_v4_or(r, g), snes_v4_or(b, alpha_vec));
}

#endif /* SNES_NTSC_SIMD_H */
