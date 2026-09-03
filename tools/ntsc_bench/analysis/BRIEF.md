# Brief: SIMD strategy study for the Blargg SNES NTSC hires filter

This brief is given to independent reviewers who have NOT seen any prior
SIMD attempt. It contains only objective facts about the game, the target
hardware, and the scalar reference implementation. Please reason from the
scalar code and these facts alone.

## 1. What the filter does in this game

CannonBall-SE is an OutRun (Sega System 16) re-implementation. In "hires"
mode the game renders a 640 x 448 frame of 16-bit palette values once per
frame, and the NTSC filter turns it into a 32-bit RGBA frame with composite
video artefacts (colour bleed, dot crawl, chroma fringing) for display.

The scalar reference is `snes_ntsc_blit_hires()` in `scalar_ref/snes_ntsc.cpp`.
It is Shay Green's snes_ntsc 0.2.2 library, modified by the game author to
take Sega S16 palette input (`SNES_NTSC_S16` format: 15-bit RGB plus a shadow
bit in bit 15, used directly as a table index) and emit 32-bit RGBA.

Call contract per frame (from the renderer):

    snes_ntsc_blit_hires(ntsc, in, in_row_width=640, burst_phase, in_width=640,
                         in_height, out, out_pitch=out_width*4, Alevel=255)

- Input: `uint16_t` pixels, row pitch 640 (808 in wide-screen mode).
- Output: 32-bit RGBA, `out_width = SNES_NTSC_OUT_WIDTH(in_width/2)` = 749 for
  640 input, pitch = `out_width*4`. Each pair of hires input pixels drives one
  "lores" output position of 7/3 output pixels: 6 input pixels -> 7 outputs.
- `burst_phase` cycles 0,1,2 per frame at 60 fps; inside the blitter it also
  advances by one (mod 3) per row.
- The renderer splits the frame into a top and bottom half of 224 rows and
  runs the two halves on two threads concurrently (each thread calls the
  blitter once with in_height=224).
- Setup: the `snes_ntsc_composite` preset, `merge_fields = 0`, other
  parameters at their defaults. `snes_ntsc_init()` builds the lookup table
  once at start-up (and again only when the user changes a slider), so init
  cost is not important and the table layout can be redesigned freely.

## 2. Table facts

- `snes_ntsc_t` is `uint32_t table[0x10000][128]` = 32 MiB, one 512-byte
  entry per possible 16-bit input value. In practice a frame uses a few
  hundred distinct input values (the S16 palette has 8192 entries and only a
  fraction are on screen at once).
- Each table entry is organised as 3 burst phases of 42 values (see the code
  and `snes_ntsc.txt` for the exact layout; `snes_ntsc_burst_size = 128/3`).
- Memory budget: the target board has 512 MB RAM total with roughly 100 MB
  free while the game runs. The table may not grow significantly; shrinking
  it is welcome.

## 3. Target hardware

Primary: Raspberry Pi Zero 2 W / Raspberry Pi 3 class.
- 4x ARM Cortex-A53, aarch64, 1.0 GHz. In-order, dual-issue. NEON (ASIMD)
  128-bit, 32 vector registers. No hardware gather.
- L1D 32 KB per core, L2 512 KB shared by all four cores, LPDDR2 memory.
- Compiler: GCC 14, `-O3 -mcpu=cortex-a53`. Intrinsics are acceptable;
  inline assembly is not wanted.

Secondary (must remain possible, via a thin macro layer): x86-64 SSE2 and
SSE4.1, and RISC-V RVV 1.0. An approach that maps naturally onto generic
128-bit SIMD is preferred over one that relies on an ARM-only instruction.

## 4. Measured behaviour of the scalar reference (full 640 x 448 frame)

Measured on the primary target with a fixed synthetic frame of spatially
random pixels drawn from N distinct values, median of 3 runs:

| Case | ms per frame | fps |
|---|---|---|
| N = 256 distinct colours, 1 thread | 22.7 | 44 |
| N = 65536 distinct colours, 1 thread | 92.2 | 10.8 |
| N = 256, frame split over 2 threads | 12.0 | 83 |

Observations you may rely on: performance depends very strongly on how much
of the 32 MiB table is touched (working set = distinct colours x 512 B); two
threads scale almost linearly, so at realistic palette sizes the work is
per-core latency/throughput bound rather than DRAM-bandwidth bound.

## 5. Goal and constraints for a new implementation

- Goal: at least 2x faster than the scalar reference single-threaded on the
  256-colour case, and no cliff on large palettes.
- Budget context: at 60 fps each render thread has 16.7 ms for its 224-row
  half, and it also performs a palette conversion pass and a scanline pass in
  the same time, so the filter should take well under 8 ms per half frame.
- Correctness: output must be bit-identical to the scalar reference on all
  interior pixels for the same input and phase. Small differences in the
  first/last few output pixels of a row (edge padding) are tolerable if
  documented. A change of table layout is fine as long as the summed values
  are the same.
- The 16-bit input format, 32-bit RGBA output format, and the 2-thread
  half-frame split are fixed.
- No implementation is wanted at this stage: ideas, analysis and estimates.

## 6. What a strategy proposal must contain

For each proposed approach:
1. Name and one-paragraph core idea.
2. Which axis of parallelism it exploits (lanes = adjacent output pixels,
   lanes = rows, lanes = burst phases, lanes = colour channels, etc.).
3. How the table lookups (the gather) are handled, and the resulting memory
   access pattern per output pixel compared with scalar.
4. Any change to the table layout or size, with the new size in bytes.
5. Estimated per-output-pixel cost on Cortex-A53 in loads, vector ops and
   scalar ops, and the resulting estimated speedup over scalar, with the
   reasoning shown.
6. Bit-exactness: exact, or where and why it differs.
7. Portability to SSE2/SSE4.1/RVV.
8. Main risks and unknowns, and what small experiment would resolve them.
9. Rough implementation effort (days) and complexity.
