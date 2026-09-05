# scalar_ref — isolated scalar-only Blargg SNES NTSC filter

This directory is a sanitised, SIMD-free copy of the Blargg SNES NTSC video
filter as it existed in cannonball-se at commit `086591c`
("Update CANNONBALL_SE_VERSION to 1.41"). It exists so the scalar
implementation can be studied in isolation, with no SIMD (SSE/NEON/RVV) code
visible anywhere in the four filter files.

## Contents

- `snes_ntsc.h`, `snes_ntsc.cpp`, `snes_ntsc_config.h`, `snes_ntsc_impl.h` —
  the filter, extracted from `src/main/sdl2/` at commit `086591c` and then
  sanitised (see "What was removed" below). All scalar logic, macros, enums,
  structs and comments are byte-identical to the original; only SIMD-only
  regions and SIMD-related comment lines were deleted.
- `snes_ntsc.txt` — the original author's (Shay "blargg" Green) documentation,
  copied unchanged.
- `snes_ntsc_blit_hires.preprocessed.cpp` — a fully macro-expanded
  (`g++ -E -P -std=c++17`) view of `snes_ntsc_init` and `snes_ntsc_blit_hires`
  plus the static helpers they call (`init_filters`, `init`, `gen_kernel`,
  `merge_kernel_fields`, `correct_errors`) and the two small data tables they
  reference. This is a reading aid only, not a build target: it shows the
  literal per-pixel arithmetic with every `SNES_NTSC_*` macro expanded inline.
  clang-format was not available in the build environment, so formatting was
  done with a sed pass that puts each statement/brace on its own line rather
  than a proper formatter.

## What was removed

From `snes_ntsc.h`:
- The compile-time SIMD detection chain (`#if defined(__ARM_NEON) ...`
  through the RISC-V/`#else` branches), replaced with a single
  `#define SNES_NTSC_HAVE_SIMD 0`.
- The `#if SNES_NTSC_HAVE_SIMD` guarded declaration of
  `snes_ntsc_blit_hires_fast`.
- The `SNES_NTSC_OUT_WIDTH_SIMD` macro.
- The `SNES_NTSC_HIRES_OUT_SIMD_` and `SNES_NTSC_HIRES_OUT_SIMD` macros
  (both exclusively used by the now-removed fast blitter).
- The whole SIMD dispatch block (`SIMD_REGISTER_t`, `SIMD_INPUT_REGISTER_t`,
  `LOAD_SIMD_REGISTER`, `INSERT_/EXTRACT_SIMD_REGISTER_VALUE*`,
  `SET_SIMD_REGISTER`, `ZERO_SIMD_REGISTER`, `ROTATE_OUT*`,
  `SET_SNES_MASK_VECTORS*`, `SNES_NTSC_CLAMP_SSE4/NEON/RVV`,
  `SNES_NTSC_CLAMP_AND_CONVERT_SSE4/NEON/RVV`,
  `SNES_NTSC_RGB_OUT_STORE_SSE4/NEON/RVV`, `SNES_NTSC_RGB_STORE_SSE4/NEON/RVV`,
  and the RISC-V RVV 1.0 macro set) for SSE4, NEON and RVV.
- SIMD-related lines from the file header comment (the "SIMD optimised
  processing" bullet and the "SIMD dependent functions ... require buffers
  aligned" note).
- One residual comment referencing SIMD ("Generic (without SIMD) producing
  ABGR:") reworded to "Generic version producing ABGR:" since the SIMD
  alternative it contrasted with no longer exists in this copy.

From `snes_ntsc.cpp`:
- The `CHECK_ALL_EQUAL` dispatch `#if SNES_NTSC_HAVE_SIMD ... #endif` block
  and the `CHECK_ALL_EQUAL_SSE4/AARCH64/NEON/RVV` macro family.
- The entire `snes_ntsc_blit_hires_fast` function (`#if SNES_NTSC_HAVE_SIMD
  ... #endif`).
- SIMD-related "Updates" lines from the file header comment (the SSE/NEON
  SIMD blitters bullet, the Aug-25 AVX/AArch64/NEON note, and the Feb-26
  SSE2/SSE4.1/RVV note).

Everything else — `snes_ntsc_init`, `snes_ntsc_blit`, `snes_ntsc_blit_hires`,
`merge_kernel_fields`, `correct_errors`, all scalar macros
(`SNES_NTSC_HIRES_ROW`, `SNES_NTSC_HIRES_OUT`, `SNES_NTSC_COLOR_IN`,
`SNES_NTSC_RGB_OUT_`, `SNES_NTSC_CLAMP_`, `SNES_NTSC_CLAMP_AND_CONVERT_SCALAR`,
format macros, enums, structs) and all non-SIMD comments — is untouched.

`snes_ntsc_config.h` and `snes_ntsc_impl.h` contained no SIMD content at
commit `086591c` and are copied byte-for-byte unchanged.

The one intentional literal occurrence of the word "SIMD" left anywhere in
these files is the macro `#define SNES_NTSC_HAVE_SIMD 0` itself, kept (per
the sanitisation approach) as a hard-wired flag rather than deleted outright;
it is not referenced anywhere else in this directory.

## Verification

1. **Keyword scan** — `grep -n -i -E "simd|neon|sse|rvv|xmm|_fast|intrin|__m128|vld1|vst1" scalar_ref/*.h scalar_ref/*.cpp`
   returns only:
   - `#define SNES_NTSC_HAVE_SIMD 0` (the intentional flag, see above), and
   - false-positive substring matches of `sse` inside ordinary English words
     in comments/license text: `assert`, `Lesser` (as in "GNU Lesser General
     Public License"), `passed`, `processed`, `processes`. None of these are
     SIMD-related.

2. **Functional equivalence** — built the existing bench harness
   (`tools/ntsc_bench/ntsc_bench.cpp`) against these sanitised sources in
   scalar mode:
   ```
   g++ -O3 -march=native -std=c++17 -DBENCH_SCALAR \
       -DBENCH_FN=snes_ntsc_blit_hires -DBENCH_REV=scalar_ref \
       ntsc_bench.cpp snes_ntsc.cpp -o bench_scalar
   ```
   Results (WSL, x86_64, `g++ 13.3.0`):
   | run                        | expected (086591c)     | observed                | match |
   |----------------------------|-------------------------|--------------------------|-------|
   | `--seconds 2` (256 colours)| `0xa464e1588de67452`    | `0xa464e1588de67452`    | yes   |
   | `--seconds 2 --colours 65536` | `0x7caa74cc7e7ee935` | `0x7caa74cc7e7ee935`    | yes   |

   Both checksums match the unmodified commit `086591c` scalar build exactly,
   confirming no scalar logic was altered by the SIMD removal.

3. **Warnings** — compiling `snes_ntsc.cpp` with `-Wall` produces exactly one
   warning, `snes_ntsc.h:253: warning: multi-line comment [-Wcomment]`, on a
   block of pre-existing commented-out dead code that is present verbatim in
   the unmodified commit `086591c` source (there at line 337). The original,
   unmodified `086591c` build additionally warns about an unused variable
   (`iterations`) inside `snes_ntsc_blit_hires_fast` — that warning is gone
   here simply because the function itself is gone. No new warnings were
   introduced by the sanitisation.

This directory is a working copy for study purposes; it is not wired into
the CMake build and nothing under `src/` was modified to produce it.

Note (added later): the checksums quoted above were produced before ntsc_bench.cpp was
changed to clear merge_fields (matching the game). With the current bench the scalar
reference gives 0x8a54cf7b7096697e (256 colours) and 0x5afd2d5d92a24084 (65536 colours).
