# ntsc_bench

Minimal throughput/correctness harness for the Blargg SNES NTSC hires
blitters (`src/main/sdl2/snes_ntsc.{cpp,h}`). Full rationale in `DESIGN.md`.

## Build

One `.cpp`, no SDL/CMake/game headers - links only against `snes_ntsc.cpp`:

```sh
g++ -O3 -mcpu=cortex-a53 -std=c++17 \
    -DBENCH_FN=snes_ntsc_blit_hires_fast2 -DBENCH_REV=WT \
    ntsc_bench.cpp snes_ntsc.cpp -o bench_fast2 -lpthread
```

- `-DBENCH_FN=<function>` - blitter under test (default `snes_ntsc_blit_hires_fast`).
- `-DBENCH_SCALAR` - add this when `BENCH_FN` is the scalar reference
  `snes_ntsc_blit_hires`; it switches the output-width formula to match the
  non-SIMD game build.
- `-DBENCH_REV=<token>` - free-form revision tag, printed in the `RESULT` line.

The same source builds unmodified against commit `086591c` (`_fast` only),
the working tree (`_fast` + `_fast2`), or a future rewrite.

## Usage

```sh
./bench_fast2 [--pattern random|mixed] [--colours N] [--width W] [--height H] \
              [--seconds S] [--threads T] [--palette random|contiguous] [--hugepages]
```

Defaults: `random`, 256 colours, 640x448, 5 seconds, 1 thread, `--palette
random`, `--hugepages` off. The input frame is filled once with a
deterministic xorshift32 RNG (fixed seed) from a palette of `--colours`
distinct values, then reused unchanged every frame.

- `--palette random|contiguous` - `random` (default) keeps the original
  pseudo-random 16-bit palette values, bit-identical to the checksums above.
  `contiguous` instead sets the palette to values `0..N-1`, so the ntsc table
  rows the blit touches (512 B/row) are the first N of them - contiguous in
  memory, spanning N/8 4 KB pages - instead of scattered across the full
  32 MiB table. Spatial *placement* of pixels (which palette slot lands
  where) uses the same RNG sequence either way.
- `--hugepages` - allocate the 32 MiB ntsc table 2 MiB-aligned and
  `madvise(MADV_HUGEPAGE)` it before `snes_ntsc_init`. Prints whether the
  `madvise` call succeeded and, after init, the process's `AnonHugePages`
  figure from `/proc/self/smaps_rollup` so you can see whether the kernel
  actually backed the table with transparent huge pages. Linux only; prints
  `unsupported` and falls back to a normal allocation elsewhere. Default off.

Driver scripts (run from WSL, from the repo root):

```sh
tools/ntsc_bench/run_pi.sh   <rev|WT> <function> [bench args...]  # sl-d303 (ARM)
tools/ntsc_bench/run_host.sh <rev|WT> <function> [bench args...]  # WSL host (x86)
```

Both stage the four filter files (`snes_ntsc.cpp/.h`, `snes_ntsc_config.h`,
`snes_ntsc_impl.h`) plus `ntsc_bench.cpp`, build only if the binary is older
than its sources, and run. `run_pi.sh` additionally sets the CPU governor to
`performance` and pins the run with `taskset` (one core per `--threads`,
ending at core 3, away from resident services) for the duration of the run,
restoring `ondemand` afterwards, and prints `vcgencmd get_throttled` before
and after so a thermally-throttled run is visible.

## RESULT line

```
RESULT fn=snes_ntsc_blit_hires_fast rev=086591c simd=NEON w=640 h=448 \
  pattern=random colours=256 threads=1 frames=312 ms=16.03 fps=62.4 \
  min=15.8 max=17.1 checksum=0x1a2b3c4d5e6f7081 palette=random hugepages=0
```

`fn`/`rev` identify what was built; `simd` is the ISA actually compiled in
(`NEON`/`SSE2`/`SSE4`/`RVV`/`none`). `w`/`h`/`pattern`/`colours`/`threads`
echo the input configuration. `frames`/`ms`/`fps`/`min`/`max` summarize the
timed loop (mean frame time, derived fps, and the fastest/slowest single
frame) after a 10-frame warm-up. `checksum` is a 64-bit FNV-1a accumulated
over the output buffer across three extra frames at burst phases 0, 1, 2,
run after timing - it is the bit-exactness gate: the same input and the same
filter semantics must produce the same checksum on any machine or SIMD ISA
(SSE and NEON are supposed to agree), and across any two blitters that claim
equivalent output (e.g. `_fast` vs `_fast2`). `palette`/`hugepages` echo the
`--palette` and `--hugepages` settings used for the run (see Usage above).

## Reference checksums (640x448, table built like the game: composite preset, merge_fields = 0)

The bench clears `merge_fields` exactly as `RenderSurface::init_blargg_filter()` does.
Earlier runs (before 2026-09-02 evening) used the raw preset, whose literal has
`merge_fields = 1`, so their checksums differ from the ones below. Timings are unaffected.

| Function (rev) | random, 256 colours | random, 65536 colours | mixed |
|---|---|---|---|
| snes_ntsc_blit_hires_fast (086591c) | 0x95f5c92e642a686f | 0x50df72f5a78f9e6d | 0x2921e1e8ab232056 |
| snes_ntsc_blit_hires_fast2 (working tree) | 0x95f5c92e642a686f | same as _fast | 0x7d137397ad358e78 (MISMATCH: repeat-block bug) |
| snes_ntsc_blit_hires scalar (086591c), out_width 749 | 0x8a54cf7b7096697e | 0x5afd2d5d92a24084 | 0x8390b47c580c191e |

SSE4 (host) and NEON (Pi) builds of the same source produce identical checksums.

## Working-set sweep (sl-d303, 640x448, 1 thread, performance governor, core 3, 2 s runs)

ms per frame versus number of distinct colours N (table working set = N x 512 B):

| N | 1 | 16 | 64 | 256 | 1024 | 4096 | 65536 |
|---|---|---|---|---|---|---|---|
| scalar snes_ntsc_blit_hires (086591c) | 10.98 | 13.01 | 19.03 | 22.20 | 47.79 | 83.95 | 93.53 |
| snes_ntsc_blit_hires_fast (086591c) | 1.95 (repeat-block path) | 11.89 | 15.41 | 19.60 | 48.23 | 82.97 | 95.08 |

Reading: N=1 is the pure compute floor (about 33 cycles per output for scalar at 1 GHz).
The step 16 -> 64 is the table outgrowing L1 (32 KB); 256 -> 1024 is it outgrowing L2 (512 KB).
Page size on the target is 4 KB. Transparent huge pages are NOT available on its kernel (6.18.39+rpt-rpi-v8 has no /sys/kernel/mm/transparent_hugepage and no HugePages in /proc/meminfo); madvise(MADV_HUGEPAGE) fails there.
Produced with tools/ntsc_bench/sweep_pi.sh (runs on the Pi against a prebuilt binary).

## Page-spread vs cache-line experiment (sl-d303, scalar snes_ntsc_blit_hires @ 086591c, 3 s runs, core 3, performance governor)

`--palette contiguous` keeps exactly the same N table rows touched, with
exactly the same RNG-driven pixel placement, as `--palette random` - only
*where in the 32 MiB table* those N rows live changes: `random` scatters
them pseudo-randomly across all 65536 rows (up to N distinct 4 KB pages),
`contiguous` packs them into the first N rows (N/8 4 KB pages). So any timing
difference between the two isolates page/TLB locality from cache-line
traffic, which is identical in both modes (same N x 512 B fetched per frame).

Correction to the sweep above: this kernel (`6.18.39+rpt-rpi-v8`) has no
`/sys/kernel/mm/transparent_hugepage` at all - `CONFIG_TRANSPARENT_HUGEPAGE`
is not built in, not just disabled. `madvise(MADV_HUGEPAGE)` therefore failed
(`EINVAL`) on every `--hugepages` run below, and `/proc/self/smaps_rollup`
read back `AnonHugePages: 0 kB` every time - THP was never actually granted
on this box, contrary to what the "madvise mode" note above assumed. The
"on" column differs from "off" only in that the table was allocated with
`posix_memalign(2 MiB)` instead of `malloc()`.

ms per frame:

| N | random, THP off | random, THP on | contiguous, THP off | contiguous, THP on |
|---|---|---|---|---|
| 64   | 18.59 | 18.56 | 13.27 | 13.11 |
| 256  | 22.10 | 22.01 | 20.29 | 19.90 |
| 1024 | 48.46 | 45.02 | 37.86 | 34.21 |

Repeat of N=256/random/THP off, run last: 22.26 ms (vs 22.10 ms first; +0.7%,
well inside the ~4% min/max jitter seen within a single run) - the sweep is
run-to-run stable. `vcgencmd get_throttled` read `0x0` before and after the
whole sequence (no thermal throttling).

Interpretation: since `hugepages` never actually took effect here (THP
unsupported by the kernel), those two columns are noise around the same
allocation strategy and should be read as such, not as a THP effect. The
real signal is `random` vs `contiguous`: at identical cache-line traffic,
packing the same N touched rows into N/8 pages instead of scattering them
over up to N pages is 9-29% faster (largest relative gain at N=64, where the
touched set - one page's worth of TLB reach at 8 rows/page - fits the
Cortex-A53's small L1 dTLB reach when contiguous but not when scattered
across 64 separate pages). That the *same bytes moved* run measurably faster
purely from being packed into fewer pages says a real share of the scalar
filter's stall time at these working sizes is page/TLB-spread overhead
(TLB misses, page-table walks) rather than pure cache-line-fetch traffic -
cache-line volume alone cannot explain the gap, since both modes fetch
identical volume.

## Captured game frames (-dump option)

The game accepts `-dump <dir> [interval] [count]` (defaults 30 and 60): every interval-th
rendered frame it writes `<dir>/frame_NNNNNN.bin` and a manifest line, then quits cleanly.
The file holds a 64-byte header (magic CBSEFRM1, width, height, frame number, burst phase,
hires, wide, palette entries), the raw slot buffer, the rgb_blargg table and the converted
filter input; the exact layout is documented at write_dump_frame() in
src/main/sdl2/rendersurface.cpp. Run with `-perftest -t 1` and an interval that is not a
multiple of 3 so all burst phases are sampled.

- `frame_stats.py <dir>`: validates every file (colour == table[raw]) and prints per-frame
  distinct colours, flat 6-pixel chunks at the blitter alignment, identical even/odd pairs,
  vertical repetition and shadow use, plus a summary line.
- `frame_to_png.py <file.bin|--batch dir outdir> [--downscale]`: PNG previews, standard
  library only.
- `frames/`: 24 frames selected from a 150-frame attract-mode capture (frames 61..9150, all
  three phases), with PNGs in `frames/png/`. Averages over the full capture: 100 distinct
  colours, 65% flat chunks, 91% identical even/odd pairs, 81% vertical repetition.

Not yet available: an `--input frame.bin` mode for ntsc_bench itself.
