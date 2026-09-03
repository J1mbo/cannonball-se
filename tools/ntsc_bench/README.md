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
the working tree (`_fast` + `_fast2` + Stage 2's `_vec`), or a future rewrite.

`run_host.sh` also honours a `BENCH_CXXFLAGS` environment variable, appended after
`-march=native` (last flag wins), so a more specific `-march` can override it - e.g.
`BENCH_CXXFLAGS=-march=x86-64 tools/ntsc_bench/run_host.sh WT snes_ntsc_blit_hires_vec`
builds and runs the vector kernel against a strictly-SSE2 target line even on an
SSE4/AVX2-capable host, to check its v4u32 layer really only needs SSE2. Both driver
scripts also recognise `<function>` ending in `_vec_scalar` as shorthand for "build
`snes_ntsc_blit_hires_vec` with `-DSNES_NTSC_FORCE_SCALAR_SIMD`" (see
`snes_ntsc_simd.h`), which forces the portable SIMD layer to its plain-C fallback even
on NEON/SSE2-capable hardware, e.g. `run_host.sh WT snes_ntsc_blit_hires_vec_scalar` or
`run_pi.sh WT snes_ntsc_blit_hires_vec_scalar`.

## Usage

```sh
./bench_fast2 [--pattern random|mixed|runs] [--colours N] [--width W] [--height H] \
              [--seconds S] [--threads T] [--palette random|contiguous] [--hugepages] \
              [--input <path>] [--lores] [--dump-output <file>]
```

Defaults: `random`, 256 colours, 640x448, 5 seconds, 1 thread, `--palette
random`, `--hugepages` off, no `--input` (synthetic input), `--lores` off.
The input frame is filled once with a deterministic xorshift32 RNG (fixed
seed) from a palette of `--colours` distinct values, then reused unchanged
every frame - unless `--input` is given (see below).

- `--pattern runs` - deterministic (fixed-seed) reference pattern for Part C's
  run-length ladder (Stage 3, `snes_ntsc_blit_hires_vec`'s uniform/pair/general
  chunk classification - see "Stage 3" below). Each row is a sequence of runs
  of random length 1-48 pixels and random colour; every 7th run is forced to a
  single pixel (a lone differing pixel breaks both chunk-uniformity and
  pair-equality right there, exercising the general per-pixel path and a
  boundary transition); of the rest, half are "pair-doubled" - each successive
  *pair* of pixels within the run gets its own fresh colour (`c0 c0 c1 c1 c2
  c2 ...`) rather than one flat run of a single colour, so the run is never
  chunk-uniform but every adjacent pair is identical, exercising the PK pair
  path specifically instead of letting a cheaper uniform-chunk/steady-run
  shortcut swallow it. The other half of non-single runs are flat, exercising
  uniform chunks and steady runs. Run boundaries land at essentially every
  phase relative to both the 6-pixel chunk grid and the 2-pixel pair grid over
  many runs/rows, so every alignment and residue the ladder distinguishes gets
  exercised at least once. Independent fixed-seed RNG stream from
  random/mixed. `--colours`/`--palette` still select the colour pool.
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
- `--input <path>` - replace the synthetic input with real captured game
  frames (see "Captured game frames" below for the file format and where
  `frames/` comes from). `<path>` may be one `frame_*.bin` file, a directory
  (all `frame_*.bin` files in it, sorted - the zero-padded frame number
  makes lexicographic order equal numeric order), or a comma-separated list
  of either. All loaded frames must share one width/height; mixed geometries
  are rejected with an error. Overrides `--width`/`--height`; `--pattern`,
  `--colours`/`--colors` and `--palette` are ignored. The timed loop cycles
  through the loaded frames in order, one frame per iteration, each at
  *that frame's own recorded* burst phase (mirroring what the game actually
  hands the blitter, rather than a fixed 0/1/2 cycle). The checksum instead
  runs *every* loaded frame at phases 0, 1 and 2 (frames outer, phases
  inner), one FNV-1a accumulated over all of it - this is the cross-build
  bit-exactness gate for the whole captured-frame corpus, independent of
  which phases happened to be recorded. Mutually exclusive with `--lores`.
- `--lores` - test the low-res blitter (`snes_ntsc_blit`) against a
  synthetic 320x224 input instead of the hires blitter under test; `BENCH_FN`
  is ignored (the RESULT line's `fn` reads `snes_ntsc_blit`). `out_width` is
  `SNES_NTSC_OUT_WIDTH(320)` = 749, independent of `-DBENCH_SCALAR` (the
  lores blitter has no separate SIMD/scalar path). This exists purely to
  regress the lores path in later re-architecture stages - it is not
  comparable to the hires numbers elsewhere in this file. Mutually exclusive
  with `--input`.
- `--dump-output <file>` - append every output frame produced during the
  correctness-checksum pass (the same frames/phases folded into `checksum` -
  see above) to `<file>`, tightly packed (height rows x `out_width` x 4
  bytes, row pitch == `out_width*4`), preceded by one 32-byte header: magic
  `CBSEOUT1`, `uint32 out_width`, `uint32 height`, `uint32 frame_count`,
  `uint32 phases_per_frame`, `uint32 reserved[2]`. Pure addition - writing
  the dump does not change `checksum` or any other RESULT field. Compare two
  dumps with `diff_output.py` (see "Captured game frames" below). Default off.

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
  min=15.8 max=17.1 checksum=0x1a2b3c4d5e6f7081 palette=random hugepages=0 \
  inputs=0 lores=0
```

`fn`/`rev` identify what was built; `simd` is the ISA actually compiled in
(`NEON`/`SSE2`/`SSE4`/`RVV`/`none`). When `fn` is Stage 2's `snes_ntsc_blit_hires_vec`
(any name containing `_vec`), `simd` instead reports the backend the portable v4u32
layer (`snes_ntsc_simd.h`) actually resolved to (`NEON`/`SSE2`/`RVV`/`scalar`), which
can differ from the host's own ISA under `SNES_NTSC_FORCE_SCALAR_SIMD` - see the
`_vec_scalar` convention above. `w`/`h`/`pattern`/`colours`/`threads`
echo the input configuration. `frames`/`ms`/`fps`/`min`/`max` summarize the
timed loop (mean frame time, derived fps, and the fastest/slowest single
frame) after a 10-frame warm-up. `checksum` is a 64-bit FNV-1a accumulated
over the output buffer across three extra frames at burst phases 0, 1, 2,
run after timing - it is the bit-exactness gate: the same input and the same
filter semantics must produce the same checksum on any machine or SIMD ISA
(SSE and NEON are supposed to agree), and across any two blitters that claim
equivalent output (e.g. `_fast` vs `_fast2`). `palette`/`hugepages` echo the
`--palette` and `--hugepages` settings used for the run (see Usage above).

`inputs` and `lores` are new fields for the real-frame and lores-regression
modes, always present (0/off by default, like `hugepages`):

- `inputs=<count>` - number of frames loaded by `--input` (0 when absent).
  When `--input` is active, `pattern` reads `input`, `colours` reads `0` and
  `palette` reads `n/a` (all three are ignored in this mode - see Usage);
  `w`/`h` are the loaded frames' own dimensions, and `checksum` is the
  frames-outer/phases-inner sweep described above, not the synthetic
  three-phase sweep.
- `lores=1` when `--lores` is active (0 otherwise). `fn` then reads
  `snes_ntsc_blit` regardless of `BENCH_FN`, and `w`/`h`/`out_width` are the
  fixed lores geometry (320x224 -> 749) rather than whatever the binary was
  built to test.

## Reference checksums (640x448, table built like the game: composite preset, merge_fields = 0)

The bench clears `merge_fields` exactly as `RenderSurface::init_blargg_filter()` does.
Earlier runs (before 2026-09-02 evening) used the raw preset, whose literal has
`merge_fields = 1`, so their checksums differ from the ones below. Timings are unaffected.

| Function (rev) | random, 256 colours | random, 65536 colours | mixed | 24 captured frames, host/SSE4 (`--input`) | 24 captured frames, Pi/NEON (`--input`) |
|---|---|---|---|---|---|
| snes_ntsc_blit_hires_fast (086591c) | 0x95f5c92e642a686f | 0x50df72f5a78f9e6d | 0x2921e1e8ab232056 | 0xf977fe072096daea | 0xe3557fa06310379d **(MISMATCH, see below)** |
| snes_ntsc_blit_hires_fast (working tree, fixed - see "Real-frame baseline") | 0x95f5c92e642a686f (unchanged) | 0x50df72f5a78f9e6d (unchanged) | 0x2921e1e8ab232056 (unchanged) | 0xe3557fa06310379d | 0xe3557fa06310379d (matches) |
| snes_ntsc_blit_hires_fast2 (working tree) | 0x95f5c92e642a686f | same as _fast | 0x7d137397ad358e78 (MISMATCH: repeat-block bug) | not measured (Stage 0 covers `_fast`/scalar only) | not measured |
| snes_ntsc_blit_hires scalar (086591c), out_width 749 | 0x8a54cf7b7096697e | 0x5afd2d5d92a24084 | 0x8390b47c580c191e | 0xa01757db54ee3c6f | 0xa01757db54ee3c6f (matches) |

SSE4 (host) and NEON (Pi) builds of the same source produce identical
checksums on every column above **except** `snes_ntsc_blit_hires_fast (086591c)`
on the 24 captured frames, which differs between host and Pi (both
stable/reproducible on their own platform). This has been root-caused and
fixed in the working tree (see "Real-frame baseline" below): the host/SSE4
checksum on captured frames changes from `0xf977fe072096daea` to
`0xe3557fa06310379d`, which now matches the Pi/NEON figure bit-for-bit (the
Pi/NEON side was already correct and is unchanged by the fix). Every other
checksum in this table - both synthetic patterns, on both platforms - is
unchanged by the fix, as expected (see below for why). The scalar path
agrees cross-platform on captured frames just as it always has on the
synthetic patterns. `snes_ntsc_blit_hires_fast` and the scalar
`snes_ntsc_blit_hires` disagree with each other on every full-buffer
checksum, as expected: they use different `out_width` (752 vs 749), so even
identical per-pixel math produces a different byte stream (the fixed working
tree's `_fast` now matches scalar exactly on columns 0-735 of every one of
the 72 captured-frame/phase combinations - see `diff_output.py` below).

`--lores` reference (`snes_ntsc_blit`, synthetic 320x224, random 256 colours,
086591c): checksum `0x18b1960769cc63dd`. Not comparable to the hires rows
above (different blitter, different geometry) - it exists to regress the
lores path in later stages once it's touched by the re-architecture.

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

- `diff_output.py A.bin B.bin [--cols N]`: pixel-by-pixel diff of two
  `--dump-output` dumps (standard library only; uses numpy when importable,
  for speed on ~100 MB dumps, with an equivalent pure-stdlib fallback
  otherwise). `--cols N` limits the comparison to each row's first N columns
  - needed to compare a SIMD `_fast` dump (`out_width` 752) against a scalar
  dump (`out_width` 749); `--cols 736` is what was used to localise the
  `ROTATE_OUT_SSE4` bug below, since it excludes the trailing columns where
  the two blitters' end-of-line handling legitimately differs. Without
  `--cols`, defaults to the smaller of the two dumps' `out_width`. Reports
  total/per-image diff counts, the first 20 differences as `(frame, phase,
  row, col, valueA, valueB)`, histograms of differing columns mod 28 (the
  `_fast` unroll period: 4 chunks x 7 outputs) and mod 7, the set of rows
  affected, and a run-length summary of consecutive differing columns.

`ntsc_bench --input frames/` (or a single file, or a comma-separated list -
see Usage above) benches and checksums this corpus directly; `run_pi.sh`
ships `frames/*.bin` to the Pi and translates a local `--input` path under
this directory automatically (see comment at the top of `run_pi.sh`). See
"Real-frame baseline" below for the Stage 0 numbers this produced.

## Real-frame baseline (24 captured frames)

Stage 0 of the NTSC filter re-architecture (see `analysis/ARCHITECTURE.md`
section 11): baseline timing and checksums for the shipped `086591c` code on
the 24 captured frames above, gating every later stage's real-content claims.
`sl-d303`, performance governor, pinned core 3, `--input tools/ntsc_bench/frames
--seconds 5`, median of three runs; `vcgencmd get_throttled` read `0x0`
before and after every run below (no thermal throttling, all runs).

| Function (rev) | ms/frame (median of 3) | fps | checksum (24 frames x 3 phases) | update_ms | filter-only (ms - update_ms) |
|---|---|---|---|---|---|
| snes_ntsc_blit_hires_fast (086591c) | 5.43 | 184.1 | 0xe3557fa06310379d | n/a | n/a |
| snes_ntsc_blit_hires scalar (086591c) | 11.33 | 88.3 | 0xa01757db54ee3c6f | n/a | n/a |
| snes_ntsc_blit_hires_fast (WT, slot-indexed table - Stage 1) | 7.72 | 129.5 | 0xe3557fa06310379d | 2.09 | 5.63 |
| snes_ntsc_blit_hires scalar (WT, slot-indexed table - Stage 1) | 13.41 | 74.6 | 0xa01757db54ee3c6f | 2.10 | 11.31 |

The Stage 1 rows are `run_pi.sh WT`, same host/settings, `--seconds 5`, median of 3.
`ms` is unchanged in meaning (t0..t1 around `sync_to_frame()` + the blit, per frame);
`update_ms` is `sync_to_frame()`'s own share of that, timed separately with
`steady_clock` and printed by the bench as `update_ms=<avg ms/frame>`
(`snes_ntsc_update_slots()` in bulk for whatever slots differ between one loaded
frame's table and the next - never per-slot); `filter-only` is `ms - update_ms`,
comparable to the 086591c baseline since it excludes the table-regeneration work
that baseline never had to do. Filter-only lands within 0.2 ms of the 086591c
baseline for both blitters (5.63 vs 5.43, 11.31 vs 11.33 ms) - the blit itself is
unaffected by the slot-indexed table, as expected. `update_ms` (~2.1 ms) is the
cost of `sync_to_frame()` diffing all 8192 slots and regenerating the ones that
differ every time the timed loop switches to a different loaded frame
(`slot_updates` in the RESULT line is 100k-180k over a 5 s run, i.e. hundreds of
slots per frame); this is a worst case of the mechanism, not of real gameplay -
the 24 files are frames sampled far apart from one attract-mode capture, so
consecutive *loaded* frames differ far more than consecutive *in-game* frames
ever would (ARCHITECTURE.md section 4 estimates under 1 ms for the worst
per-frame case actually seen in the game, e.g. sky-cycling).

**The scalar checksum matches the host (WSL/SSE4) figure in the
reference-checksums table above bit-for-bit, as expected - but the `_fast`
(SIMD) checksum did not** (host: `0xf977fe072096daea`, Pi: `0xe3557fa06310379d`).
This was reproducible and stable (identical on all 3 Pi runs above, and on two
independent host runs), and it was not a transfer problem - the 24 frame files
on the Pi are byte-identical to the local copies (`md5sum` matches every
file). It was also new information: the existing random/mixed synthetic
benchmarks have always agreed bit-for-bit between SSE and NEON builds of
`_fast` (see the reference-checksums table), so this was the first input that
exposed a real SSE4-vs-NEON divergence in the shipped `_fast` blitter, and it
only showed up on real game content - exactly the gap `--input` was added to
close.

### Root cause and fix

The divergence has since been root-caused and fixed (`src/main/sdl2/snes_ntsc.h`,
`ROTATE_OUT_SSE4`). Diffing raw `--dump-output` pixel dumps with
`diff_output.py` (see below) showed that on the host, `snes_ntsc_blit_hires_fast`
disagreed with `snes_ntsc_blit_hires` (scalar, ground truth) on 1671 of the
23,740,416 pixels compared (columns 0-735, the region shared by both
`out_width`s) - and that the Pi's `_fast` output matched its own scalar output
on **every** compared pixel (0 diffs), while host `_fast` vs Pi `_fast` on all
752 columns reproduced the exact same 1671-pixel set, byte-for-byte, as the
host `_fast` vs host scalar diff. That three-way match pinned the bug
squarely on the host/SSE4 build of `_fast`: the Pi/NEON+AArch64 build was
already correct.

The affected pixels were small (mostly 1-a-few-LSB) errors confined to 23
scanlines out of 448 across 11 of the 24 frames, clustered in runs of 11-15
(never a full 28-wide unrolled output block), and landing at `col % 28`
values 0-11 and 25-27 but never 12-24 - consistent with a shortcut
*occasionally* misfiring on nearly-but-not-exactly-flat content (anti-aliased
text edges, gentle gradients) rather than a wholesale wrong-value
substitution, which is why the errors are small in magnitude but real.

The shortcut in question is exactly the one `analysis/ARCHITECTURE.md`
section 6 already calls fragile: `snes_ntsc_blit_hires_fast` checks whether
all 24 input pixels about to be processed are identical
(`CHECK_ALL_EQUAL_SSE4`/`_AARCH64`/`_NEON` in `snes_ntsc.cpp`) and, if so,
skips the maths and reuses the previous iteration's last output sub-vector.
That check needs the input pixels at lanes 2-3 of `xmm0` (the tail of the
*previous* 4-pixel load) and lanes 0-1 of `xmm6` (the head of the *next*
4-pixel load) gathered into one 4-lane vector - `ROTATE_OUT(xmm0, xmm6, 2)` -
because those are the two pixels that straddle the 4-pixel load boundary at
the start of this iteration's 24-pixel block. `ROTATE_OUT_NEON`
(`vext_u16(A, B, 2)`) gets this right: `[A[2], A[3], B[0], B[1]]`.
`ROTATE_OUT_SSE4(A, B, N)` had the two byte-shifts swapped - it did
`slli(A, N*2) | srli(B, 8-N*2)`, which for `N=2` produces `[B[2], B[3], A[0],
A[1]]` instead: the wrong two pixels from *each* side of the boundary
(`xmm6`'s lanes 2-3, which belong to the *next* iteration's block, and
`xmm0`'s lanes 0-1, which belong to the *previous* one). So the SSE4 build's
"is this whole 24-pixel block flat" check was silently testing four pixels
that are not even part of the current block, on every single invocation.
The fix (in `ROTATE_OUT_SSE4`) swaps which operand gets `slli` vs `srli` so
it reads `srli(A, N*2) | slli(B, 8-N*2)`, matching `vext_u16`'s semantics for
any `N` in 0-4, not just the `N=2` this code happens to call it with.

Random and mixed synthetic test patterns never exposed this because they
have essentially no chance of an accidental 4-pixel coincidence spanning a
block boundary being uniform when the wrong-but-adjacent 4 pixels are
checked instead of the right ones. Real game content is the opposite case:
65% flat 6-pixel chunks and 91% identical even/odd pairs mean long runs of
identical pixels are the norm, so the *wrong* 4-pixel sample is very often
just as uniform as the *right* one would have been - except right at the
edges of a run, exactly where the two samples can disagree, which is what
produced the small, sparse, edge-clustered error pattern above.

Rerunning after the fix: `snes_ntsc_blit_hires_fast`'s host/SSE4 checksum on
the 24 captured frames changes from `0xf977fe072096daea` to
`0xe3557fa06310379d`, now identical to the (unchanged) Pi/NEON figure; the
scalar checksum on both platforms is unchanged (`0xa01757db54ee3c6f`); and
all three synthetic `_fast` checksums (random 256, random 65536, mixed) are
unchanged on both platforms, as expected since the fix only changes behaviour
on the input pattern the bug depended on. `diff_output.py A.bin B.bin --cols
736` on fresh `--dump-output` dumps confirms 0 pixel differences for fixed
host `_fast` vs host scalar, for Pi `_fast` vs Pi scalar, and (all 752
columns) for fixed host `_fast` vs Pi `_fast`.

`_fast` is now self-consistent across SIMD ISAs on both real and synthetic
content, and matches the scalar reference on every shared column - no
platform-choice decision is needed before Stage 1.

For reference, the same two functions with the default synthetic random
256-colour input, one run each (compare against the working-set sweep table
above, whose N=256 row was measured before `merge_fields` was corrected but
is timing-equivalent - see the note on that table):

| Function (rev) | ms/frame | fps |
|---|---|---|
| snes_ntsc_blit_hires_fast (086591c) | 19.11 | 52.3 |
| snes_ntsc_blit_hires scalar (086591c) | 23.67 | 42.3 |

Both are within a few percent of the working-set sweep table's N=256 row
(19.60 ms / 22.20 ms), as expected for the same function and table size on
the same box.

Real game content (mostly flat 6-pixel runs and repeated even/odd pairs, and
averaging ~100 distinct colours against the table's 32 MiB - see the frame
statistics above) is **3.5x faster for `_fast`** (5.43 ms vs 19.11 ms) and
**2.1x faster for scalar** (11.33 ms vs 23.67 ms) than the random-256-colour
synthetic benchmark, even though neither function has an explicit run-length
shortcut for the general case today (`_fast`'s repeat-block detector aside) -
most of this gap is real content touching far fewer distinct table rows,
i.e. a smaller working set, than the synthetic 256-colour fill. This is
exactly the gap Part C of the re-architecture (the run-length ladder,
`analysis/ARCHITECTURE.md` section 6) is designed to widen further.

## Stage 2: `snes_ntsc_blit_hires_vec` and the portable v4u32 SIMD layer

Part B and Part D of the re-architecture (`analysis/ARCHITECTURE.md` sections 5 and 7):
a new blitter, `snes_ntsc_blit_hires_vec`, computing four adjacent output pixels per
128-bit register from contiguous 16-byte window loads, bit-identical to
`snes_ntsc_blit_hires_fast` on every input. Unlike `_fast`, it is built on a small
portable SIMD header (`src/main/sdl2/snes_ntsc_simd.h`, type `v4u32`) with NEON, SSE2
and RVV backends plus a plain-C fallback, so it compiles - and is bit-exact - on every
target, including ones with no `_fast` at all. The game (`RenderSurface::blargg_filter`)
now calls it in place of `_fast` under the existing `SNES_NTSC_HAVE_SIMD` branch.

### Layout change (prerequisite, landed before any new kernel code)

`snes_ntsc.h`'s per-slot alignment block grew from 16 to 20 words: `snes_ntsc_block_words
= 20`, a new `snes_ntsc_block_lead = 3` (leading zero pad words; the block is now 3 lead +
14 real taps + 3 trailing zero words), `snes_ntsc_slot_words = 60` (3 blocks x 20 words),
and `SNES_NTSC_KW(w) = ((w)/14)*20 + 3 + ((w)%14)`. This gives the vector kernel's
compile-time-constant 16-byte window loads - some of which start before the first real
tap or end after the last - somewhere valid to land without a bounds check, in exchange
for more padding (60 words allocated per slot per phase vs 42 used, table now
`sizeof(snes_ntsc_t)` = 5,907,904 B, about 5.9 MB). `_fast` and the scalar blitter are
unaffected: both address every word through `SNES_NTSC_KW`, which folds to the new
physical offsets transparently. The full Stage 1 checksum set (`_fast` and scalar,
random/mixed/`--input`/lores) was re-run against the layout change alone, before writing
any kernel code, and reproduced every reference value unchanged.

### The kernel

One row is processed in groups of 24 input pixels / 28 output pixels, using 10
accumulator vectors `A[0..9]`: `A[0..6]` are the group's own 28 outputs, `A[7..9]`
accumulate the "spill" into the next group's first 12 outputs (a pixel's 14-tap
contribution can reach up to 3 vectors past its own group's 7). Each group seeds
`A[0..2]` from the previous group's spill, runs its 24-pixel body (a per-pixel table of
`{alignment, first accumulator index v0, intra-block residue r}`, `M = (r==3) ? 4 : 3`
extra loads, fully unrolled via macro so every load offset is a compile-time immediate),
then clamps/converts/stores `A[0..6]` and rotates `A[7..9]` into the next group's
`A[0..2]`. A virtual group -1 (22 black pixels then `in[0]`, `in[1]`, from zeroed
accumulators) seeds group 0, reproducing `_fast`'s black-padded prologue; the final
group produces real sums for only its first 14 pixels (the row's last 14) and stores a
precomputed "raw zero, clamped" vector for the last 4 outputs, reproducing `_fast`'s
two-pixel-short, black-padded tail exactly (752 outputs for a 640-wide row, 948 for
808). Clamp+convert is generic `v4u32` code (`snes_ntsc_simd_clamp_convert` in
`snes_ntsc_simd.h`) mirroring the scalar `SNES_NTSC_CLAMP_`/`RGB_OUT_` sequence bit for
bit, including the final `bswap32` (reproduced as masked shifts/ors, since the v4u32
layer has no dedicated lane-wise byte-reverse primitive - not needed with one).

**Register-allocation fix found during validation.** The 24-pixel body was first written
as a `static inline` C++ function taking `v4u32* A`; on the Cortex-A53 build this
compiled to a real (non-inlined) subroutine, and because `A`'s address was passed
across that call boundary, the compiler could not keep the 10 accumulators in NEON
registers - every one of them round-tripped through the caller's stack array on every
pixel, roughly doubling memory traffic for no algorithmic reason (confirmed with
`objdump` on the Pi binary: loads/stores through the incoming pointer parameter,
interleaved with the kernel-table loads). Rewriting the body as a macro
(`SNES_NTSC_VEC_GROUP_BODY()`, textually inlined at each of the three call sites so `A`
is an ordinary local array with only compile-time-constant indices and no address ever
taken) let the compiler scalar-replace it into registers instead; re-checking the
disassembly afterwards showed the stack traffic gone (only the normal callee-saved
register save/restore at function entry/exit remained). This took the Pi/NEON
random-256 time from 16.1-16.2 ms to 13.6-13.9 ms (about 15-18% faster) with the
checksums unchanged throughout.

### Checksums (all must, and do, match)

| Build | random 256 | mixed | `--input` frames | 808x448 random 256 | threads 2 (= threads 1) |
|---|---|---|---|---|---|
| Host, `-march=native` (SSE2 backend*) | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | 0xea6c5d4426a031f3 | 0x95f5c92e642a686f |
| Host, `-march=x86-64` (pure SSE2) | 0xc928ffe2395b1fa7** | 0x0616580b4a8a3d6e** | 0xe3557fa06310379d | - | - |
| Host, `_vec_scalar` (plain-C fallback, native build) | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | - | - |
| Pi, NEON | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | 0xea6c5d4426a031f3 | 0x95f5c92e642a686f |
| Pi, `_vec_scalar` (plain-C fallback) | 0x95f5c92e642a686f | - | 0xe3557fa06310379d | - | - |

\* the v4u32 layer only ever needs SSE2 on x86 (see `SNES_NTSC_SIMD_BACKEND_NAME` in
`snes_ntsc_simd.h`); "SSE2 backend" here just notes that the bench's `simd=` field for
`_vec` reports the *layer's* choice, which is SSE2 regardless of `-march=native` giving
the host SSE4/AVX2.

\*\* **Deviation from the brief, with root cause.** The brief expected every host build
to reproduce the reference checksums above regardless of `-march`. It doesn't: building
with `-march=x86-64` instead of `-march=native` changes the *table-generation* float
math (`gen_kernel`/`init` in `snes_ntsc.cpp`, unchanged by Stage 2) because the host has
AVX2/FMA (`-march=native` enables `-mfma`, fusing some `a*b+c` into a single rounding
step; `-march=x86-64` doesn't - exactly the risk flagged in `analysis/ALGORITHM.md`
section 7.9). This is **not a vec-kernel bug**: rebuilding the unmodified
`snes_ntsc_blit_hires_fast` with `-march=x86-64` reproduces the *same* two changed
checksums (`0xc928ffe2395b1fa7` on random 256, `0x0616580b4a8a3d6e` on mixed) bit for
bit, and the unmodified scalar blitter also changes (`0x8a54cf7b7096697e` ->
`0x4b1acfda629211f9`) under the same flag - i.e. every blitter that reads the table is
affected identically, because the table itself differs, not because of anything in the
new kernel. `vec` matches `_fast` exactly under every build configuration tested (native
vs native, `-march=x86-64` vs `-march=x86-64`), which is the actual bit-exactness bar
("bit-identical to `_fast`"). Tellingly, the `--input` checksum is untouched by this
effect on either blitter, on either flag - real captured-frame colours apparently never
land on an FMA-vs-no-FMA rounding boundary, only the wide-gamut synthetic random/mixed
palettes do.

### Pi timings (sl-d303, `--seconds 5`, median of 3, performance governor, pinned core 3)

| Function | random 256, ms | fps | `--input` (24 frames), ms | fps | update_ms |
|---|---|---|---|---|---|
| `snes_ntsc_blit_hires_vec` | 13.61 | 73.5 | 9.65 | 103.6 | 2.14 |
| `snes_ntsc_blit_hires_fast` | 16.64 | 60.1 | 8.02 | 124.7 | 2.14 |
| `snes_ntsc_blit_hires` (scalar) | 18.87 | 53.0 | 13.50 | 74.1 | 2.13 |

`vec` is 1.22x faster than `_fast` and 1.39x faster than scalar on the random-256
benchmark (no blitter has a working shortcut on uncorrelated random colour, so this
isolates the raw kernel cost Part B targets) - inside, if at the high end of, the
brief's 8-13 ms / "2 to 2.5x over scalar" expectation (measured 1.39x; reported
honestly as asked). On captured real frames, `vec` (9.65 ms) is *slower* than `_fast`
(8.02 ms): `_fast`'s `CHECK_ALL_EQUAL` repeat-block shortcut exploits the very high
redundancy real content has (91% identical even/odd pairs, 65% flat 6-pixel chunks per
`frame_stats.py`) and `vec` has no equivalent yet - that is Part C's run-length ladder
(Stage 3), explicitly out of scope here. `vec` still clearly beats scalar on real
content (1.40x). `update_ms` (slot-table sync cost) is identical across all three, as
expected - it is Stage 1 machinery, untouched by Stage 2.

L1-resident case (`--colours 16`, single 5 s run, same box/settings):

| Function | ms | fps | cycles/output @ 1 GHz |
|---|---|---|---|
| `snes_ntsc_blit_hires_vec` | 7.03 | 142.3 | 20.9 |
| `snes_ntsc_blit_hires_fast` | 10.42 | 96.0 | 30.9 |
| `snes_ntsc_blit_hires` (scalar) | 11.19 | 89.3 | 33.4 |

(`snes_ntsc_blit_hires_vec_scalar` / `_fast` share `out_width` 752; scalar's is 749 -
both checksums confirmed the standard scalar-vs-SIMD width difference, nothing new.)
Cycles/output = `ms * 1e6 / (448 rows * out_width)` at the confirmed 1 GHz Cortex-A53
clock (`vcgencmd measure_clock arm` on sl-d303). The same formula on the random-256
table above gives `vec` 40.4, `_fast` 49.4, scalar 56.3 cycles/output. Against
`analysis/ARCHITECTURE.md` section 11's Stage 2 gate (16-colour row at or under 22
cycles/output, 256-colour at or under 34): `vec` **meets** the 16-colour gate (20.9) and
**falls short** of the 256-colour one (40.4 vs 34) - reported honestly; closing that gap
is exactly what Part C's run-length ladder (Stage 3, out of scope here) is for, since it
removes most of the table traffic the 256-colour case is dominated by.

### Game build and run (WSL, `/home/pearc/cannonball-se-dump`)

`cmake --build build -j16` with `CPATH=~/localdeps/usr/include/x86_64-linux-gnu:~/localdeps/usr/include`
produced no new warnings (the one pre-existing `-Wcomment` warning at `snes_ntsc.h:427`,
in commented-out dead code untouched by this stage, is unchanged). Both perftest+dump
invocations exited 0 and produced 6 valid frames each, verified by `frame_stats.py`
(`verify=OK` on all 5 checked, 0 FAILED):

```
./build/cannonball-se -cfgfile res/config-dump.xml -perftest -t 1 -dump .../frames_stage2    61 5   # exit 0
./build/cannonball-se -cfgfile res/config-dump.xml -perftest -t 4 -dump .../frames_stage2_t4 61 5   # exit 0
```

## Stage 3: the run-length ladder (Part C)

Part C of the re-architecture (`analysis/ARCHITECTURE.md` section 6): inside
`snes_ntsc_blit_hires_vec`, a row-wide classification pre-pass (one compare per pixel)
picks, for every 6-pixel chunk, the cheapest of four paths that all sum to exactly the
same 32-bit wrap-around total the Stage 2 general kernel would have produced - so the
result is still bit-identical to `_fast` on every input, gated by the same checksums as
every earlier stage. The general kernel (Stage 2, `SNES_NTSC_VEC_GROUP_BODY`) is
untouched and still handles every pixel the ladder doesn't shortcut, plus the row's
prologue and tail groups in full (see "Scope" below).

### Derived tables

Three more tables per slot per phase, all exact integer sums of `plane`'s existing
14-real-word alignment blocks (computed in `gen_slot()` right after the base scatter, so
`snes_ntsc_update_slot(s)()` keeps them coherent automatically - no separate dirty
tracking needed):

| Table | Contents | Block shape | Size |
|---|---|---|---|
| `plane_pk` | `PK_a[n] = K_a[n] + K_a[n-1]`, n=0..14 (a matching even/odd pixel pair's exact sum) | 3 alignments x 24 words (3 lead + 15 real + 6 trail) | 7.08 MB |
| `plane_ck` | `CK[n] = PK_0[n] + PK_1[n-2] + PK_2[n-4]`, n=0..18 (a uniform 6-pixel chunk's exact sum) | 1 x 28 words (3 lead + 19 real + 6 trail) | 2.75 MB |
| `plane_flat` | `raw[x] = CK[x] + CK[x+7] + (x<=4 ? CK[x+14] : 0)`, x=0..6, stored **raw** (pre-clamp; Alevel is a blitter argument, not known at generation time) | 1 x 8 words (7 real + 1 pad) | 0.79 MB |

`sizeof(snes_ntsc_t)` grows from Stage 2's 5,907,904 B to **16,527,360 B** (about 15.76
MiB / 16.5 MB), matching the brief's estimate almost exactly (5.9 + 7.1 + 2.75 + 0.8 =
16.5 MB); confirmed by the bench's own `sizeof_ntsc=` field. Both PK and CK use wider
trailing padding than a K block (6 words, not 3) because a residue-2 window load reaches
word 20 of a PK block and word 24 of a CK block - one word further each time the real-tap
span grows, exactly as the brief predicted.

**FLAT constancy check.** A debug-only check (`#ifndef NDEBUG`, so compiled out of the
`-DNDEBUG` Release game build but present in every bench build here) verifies, every time
`gen_slot()` regenerates a slot, that the 7 raw FLAT values it just computed are equal to
each other and identical across all 3 burst phases - the property the whole FLAT
shortcut depends on (`ARCHITECTURE.md` section 12, risk 1: "derived, not yet dumped").
It held for **every slot checked across every test in this stage**: 8194/8194 on every
synthetic-palette run (random/mixed/runs), and on the captured frames' actual per-frame
palettes - 479,941 slot-generations on the host build's full `--input tools/ntsc_bench/frames`
run, 457,059 after the fix below, and equivalent counts (29,903-37,552, fewer because the
Pi runs used shorter `--seconds`) on the Pi. Zero failures anywhere. This is purely
informational, exactly as specified - the blitter never reads these counters and does
not rely on the check having run.

### The ladder

Per 6-pixel chunk `C` (classified via `chunk_slot[C]` = the common slot if all 6 pixels
are equal, else a sentinel), with `R(C)` = "continues a uniform run" and `emit_flat(C) =
R(C) && R(C-1)` (three uniform same-slot chunks in a row):

| Path | Condition | Table | Cost |
|---|---|---|---|
| Steady run (skip) | `emit_flat(C) && emit_flat(C+1) && emit_flat(C+2)` | none - overwritten by FLAT | 0 loads |
| Uniform chunk | `chunk_slot[C]` is one slot | `plane_ck` | 5-6 loads |
| Pair | an even/odd pair is equal | `plane_pk` | 4-5 loads |
| General | neither | `plane` (Stage 2 per-pixel) | unchanged |

After a group's normal clamp/convert/store, every chunk with `emit_flat(C)` true gets its
7 outputs unconditionally overwritten with the clamp+converted FLAT constant (two
overlapping unaligned stores via the new `snes_v4_store_u` primitive added to
`snes_ntsc_simd.h`) - correct regardless of which path that chunk's own accumulation took,
which is what makes `skip_kernel` safe: everything a skipped chunk would have contributed
lands in a 7-output range that one of these three overwrites (its own, or `C+1`'s, or
`C+2`'s) covers unconditionally.

**Scope decision (deviation from the brief).** The ladder is applied only to a row's `G`
main groups, not to the virtual prologue group or the tail group, which keep Stage 2's
unmodified general kernel verbatim. Those two groups are at most 2 of a row's `G+2` groups
(26-33 for the widths this filter supports), so shortcutting them saves negligible time;
excluding them removes a large class of edge-case risk (black-padding chunks, the tail's
short/spill semantics interacting with `skip_kernel`'s cross-group safety argument).
**This exact interaction is what the first implementation attempt got wrong**: `skip_kernel`
for the last main group's final chunk can rely on `emit_flat` of chunks 1-2 positions
ahead, and when those neighbours fall in the tail group, nothing ever overwrites them there
- an unconditional 12-column (3-vector) corruption at the very start of the tail's output,
found immediately by the `mixed`/`--input` checksums (random essentially never triggers the
ladder, so it passed throughout and gave no warning). The fix adds one extra guard,
`C + 2 <= 4*G - 1`, to `skip_kernel` so the last two chunks of the last main group never
skip (they still use the cheaper uniform/pair paths when applicable, just never the
zero-cost skip) - see the `SNES_NTSC_VEC_CHUNK` macro in `snes_ntsc.cpp`. Diagnosed with
`--dump-output` + `diff_output.py` exactly as the brief prescribes: the diff was 12
columns wide, starting at column `28*G` (the tail's first column) in every affected row,
with rows restricted to the mixed pattern's flat "sky" band - both facts pinpointed the
missing spill into the tail's seed immediately.

### Bench: `--pattern runs`

A deterministic (fixed-seed) sequence of runs of random length 1-48 pixels: every 7th run
is a single pixel (exercises the general path and a boundary transition at every
alignment/residue); half the rest are "pair-doubled" (`c0 c0 c1 c1 c2 c2 ...`, exercising
the pair path specifically rather than letting a longer flat run's cheaper uniform/steady
path swallow it); the other half are flat runs (uniform chunks, steady runs). See
`ntsc_bench.cpp`/README "Usage" above for the exact algorithm. `snes_ntsc_blit_hires_fast`
(WT) reference: **`0x0f9860292b10dd73`**.

### Checksums (all match; host and Pi)

| Build | random 256 | mixed | `--input` frames | 808x448 random | threads 2 | `--pattern runs` |
|---|---|---|---|---|---|---|
| `_fast` (WT, reference) | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | 0xea6c5d4426a031f3 | 0x95f5c92e642a686f | 0x0f9860292b10dd73 |
| Host `_vec`, `-march=native` (SSE2 layer) | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | 0xea6c5d4426a031f3 | 0x95f5c92e642a686f | 0x0f9860292b10dd73 |
| Host `_vec`, `-march=x86-64` | 0xc928ffe2395b1fa7\* | 0x0616580b4a8a3d6e\* | 0xe3557fa06310379d | - | - | - |
| Host `_vec_scalar` (plain-C layer) | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | - | - | 0x0f9860292b10dd73 |
| Pi `_vec`, NEON | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | 0xea6c5d4426a031f3 | 0x95f5c92e642a686f | 0x0f9860292b10dd73 |
| Pi `_vec_scalar` | 0x95f5c92e642a686f | 0x2921e1e8ab232056 | 0xe3557fa06310379d | - | - | 0x0f9860292b10dd73 |

\* Same pre-existing `-march=x86-64` (no FMA) table-generation float-rounding difference
documented in Stage 2 - reproduced bit-for-bit by the unmodified `_fast` under the same
flag there, not a Stage 3 regression (see Stage 2's checksum table above). `--input`
frames are unaffected on either blitter/flag, as in Stage 2.

### Pi timings, first attempt (sl-d303, `--seconds 5`, median of 3, performance governor, pinned core 3)

| Function | random 256, ms | mixed, ms | `--pattern runs`, ms | `--input` (24 frames), ms | update_ms |
|---|---|---|---|---|---|
| `snes_ntsc_blit_hires_vec` (Stage 3, first attempt) | 15.65 | 13.54 | 18.80 | 12.10 | 2.51 |
| `snes_ntsc_blit_hires_fast` | 17.00 | 13.23 | 13.85 | 8.55 | 2.51 |
| `snes_ntsc_blit_hires` (scalar) | 18.28 | 14.13 | 15.13 | 13.88 | 2.52 |

Filter-only (`ms - update_ms`) on `--input`: `vec` 9.59 ms, `_fast` 6.04 ms, scalar 11.36 ms.

The first attempt missed the brief's target ("real frames well under Stage 2's 9.65 ms
total / about 7.5 ms filter-only", `ARCHITECTURE.md`'s gate "at or under 6 ms"): `vec` on
`--input` measured 12.10 ms - slower than both Stage 2's own 9.65 ms baseline and `_fast`'s
8.55 ms - despite being fully bit-exact and despite the per-path statistics confirming the
ladder was exercising exactly the shortcuts it was designed to exercise (77% of chunks on
the free/cheap skip-or-uniform paths). Diagnosis at the time named `ARCHITECTURE.md`
section 12's own risk ("branchy classification on an in-order core") and two fixes were
tried (hoisting the classification buffers out of the per-row loop; converting `CS`/
`isFlat` from lambdas to macros to rule out accumulator spills) - both real, correct
improvements (re-verified against every checksum), both measurable on the host's x86/SSE2
build, but neither moved the Pi/NEON number outside noise. That result is superseded below.

### Stage 3 review: three fixes that closed the gap

Root cause, found on review: `snes_ntsc_simd_clamp_convert` is roughly half of the Stage 2
kernel's per-output cost (a clamp plus a 20-op ABGR-build-then-`bswap32` convert), and the
first-attempt ladder made a flat/steady chunk pay it *twice* - once when the group's normal
path clamped/converted/stored all 7 accumulators, and again when the FLAT overwrite pass
clamped/converted 2 more vectors on top - so a steady-run chunk, the ladder's whole reason
to exist, cost at least as much as the general path it was meant to avoid. Three changes:

1. **FLAT stored pre-clamped/converted.** `gen_slot()` now runs the same
   `SNES_NTSC_CLAMP_AND_CONVERT_SCALAR` sequence on each of the 7 raw FLAT values with
   `Alevel` forced to 0, storing the finished little-endian RGBA word (alpha byte zero)
   instead of the raw pre-clamp sum. At emit time the real Alevel is folded in as a plain
   `snes_v4_or` against a precomputed `snes_v4_splat((uint32_t)Alevel << 24)` - exact
   because Alevel only ever contributes to bits 31:24 of the final word and clamp+convert-
   with-Alevel=0 leaves those bits zero, *provided* Alevel fits in a byte (<=255, true for
   the game's 255 and already an implicit requirement of the unmasked scalar macro). A
   flat chunk's overwrite is now 2 unaligned loads + 2 ORs + 2 unaligned stores - no clamp,
   no convert.
2. **Group-level fast path.** Classify all 4 chunks of a 24-pixel group before touching
   any accumulator. If all 4 are `skip_kernel` (common well inside a sky/road run), the
   group's entire 28 outputs are guaranteed covered by the 4 chunks' own FLAT overwrites,
   so there is no accumulation and no clamp/convert/store at all - just 4 FLAT emits, and
   `A[0..9]` (the carry into the next group) is set to zero: chunk `4g+3` being
   `skip_kernel` guarantees `emit_flat(4g+4)` and `emit_flat(4g+5)` (the next group's first
   two chunks), so whatever the carry would have fed there is overwritten unconditionally
   regardless - zero is exact, not just convenient. Otherwise, the chunk ladder runs as
   before, but only the output vectors *not* entirely covered by an `emit_flat` chunk get
   clamped/converted/stored (a 7-bit coverage mask from the 4 chunks' flags: vector `v0`
   needs chunk0 flat, `v1` straddles chunk0/chunk1 and needs both, `v2` needs chunk1, `v3`
   straddles chunk1/chunk2, `v4` needs chunk2, `v5` straddles chunk2/chunk3, `v6` needs
   chunk3) - a straddled vector that is only partially covered is still stored normally,
   then correctly overwritten in the covered part, never skipped.
3. **Cheaper clamp+convert everywhere.** `snes_ntsc_simd_clamp_convert` now produces the
   final word directly from the clamped value: `rgba = ((io>>21)&0xFF) | ((io>>3)&0xFF00) |
   ((io<<15)&0xFF0000) | alpha_vec` - 3 shifts, 3 ands, 3 ors instead of the previous ~20
   (build ABGR, then 4 masked shifts/ands/ors to `bswap32` it). Verified bit-for-bit
   equivalent by tracing each byte through the old route: the R byte after the old
   bswap equals bits 21:28 of the clamped value, G equals bits 11:18 shifted to 15:8, B
   equals bits 1:8 shifted to 23:16 - exactly the three terms above. `clamp_mask_vec`/
   `clamp_add_vec`/`alpha_vec` are also now splatted once per blit call and passed in,
   instead of every one of the thousands of per-call invocations re-splatting them.

All three were verified against the full checksum set below before being kept.

### Pi timings, after the review fixes (same methodology)

| Function | random 256, ms | mixed, ms | `--pattern runs`, ms | `--input` (24 frames), ms | update_ms |
|---|---|---|---|---|---|
| `snes_ntsc_blit_hires_vec` (Stage 3, after review) | 14.84 | 11.29 | 15.81 | **9.26** | 2.49 |
| `snes_ntsc_blit_hires_fast` (unchanged) | 17.00 | 13.23 | 13.85 | **8.55** | 2.51 |
| `snes_ntsc_blit_hires` (scalar, unchanged) | 18.28 | 14.13 | 15.13 | 13.88 | 2.52 |

Filter-only on `--input`: `vec` **6.77 ms**, `_fast` 6.04 ms, scalar 11.36 ms. `vec` on
`--input` improved 23% (12.10 -> 9.26 ms total, 9.59 -> 6.77 ms filter-only), now **beats**
Stage 2's own 9.65 ms baseline and sits inside the brief's "about 7.5 ms filter-only"
target and right at `ARCHITECTURE.md`'s "at or under 6 ms" gate (6.77 ms, close but not
quite under); it still trails `_fast` on real content (9.26 vs 8.55 ms, a much narrower gap
than the first attempt's 12.10 vs 8.55) rather than beating it, so Part C's promise of
"faster than `_fast` on real content by exploiting the same redundancy" is nearly but not
fully realised on this hardware. `mixed` and `random 256` also both improved substantially
(13.54->11.29 ms, 15.65->14.84 ms) even though neither exercises the ladder as heavily as
`--input`, confirming changes 1 and 3 (cheaper FLAT emit, cheaper convert everywhere) help
independently of how often the ladder fires. `--pattern runs` improved too (18.80->15.81 ms)
but far less, and the mask experiment below explains why.

### Ladder-level experiment (`SNES_NTSC_LADDER_MASK`, sl-d303, `--seconds 5`, median of 3)

`run_pi.sh` now passes `BENCH_CXXFLAGS` through exactly like `run_host.sh`
(e.g. `BENCH_CXXFLAGS=-DSNES_NTSC_LADDER_MASK=1 tools/ntsc_bench/run_pi.sh WT
snes_ntsc_blit_hires_vec ...`), forcing a rebuild whenever it is set. Bit0 = flat/skip,
bit1 = CK (uniform chunk), bit2 = PK (pair); a cleared bit routes that case to the next
path down rather than deleting it (mask 0 = pure general path via the ladder's own
fallback, mask 7 = the full ladder, the shipped default). Checksums matched the full
reference set at every mask value (all four below reproduce `0xe3557fa06310379d` on
`--input` and `0x0f9860292b10dd73` on `--pattern runs`):

| Mask | Levels enabled | `--input`, ms | `--pattern runs`, ms |
|---|---|---|---|
| 0 | none (general path only) | 12.36 | 12.06 |
| 1 | flat/skip | 9.92 | 12.40 |
| 3 | flat/skip + CK | **9.26** | **11.99** |
| 7 | flat/skip + CK + PK (shipped default) | **9.26** | 15.81 |

Flat/skip (bit0) is almost the whole win on `--input` (12.36 -> 9.92 ms, 20%); CK (bit1)
adds a further 7% (9.92 -> 9.26 ms). **PK (bit2, pairs) adds nothing measurable on
`--input`** (9.26 ms either way) **and actively regresses `--pattern runs` by 32%** (11.99
-> 15.81 ms): `runs` deliberately has a large share of chunks that are neither flat nor
uniform (every 7th run a lone pixel, half the rest pair-doubled with a fresh colour every 2
pixels), so the extra pair-equality branch and `plane_pk` table lookup is frequently
attempted and wasted, whereas mask 3 sends those chunks straight to the general path with
no extra branch. On real game content specifically (`--input`, the corpus the whole ladder
is designed around) PK is simply not pulling its weight on the Cortex-A53 - it is free to
leave enabled for correctness (it never costs anything on `--input`) but is not delivering
the "~5 loads instead of ~9" benefit the design brief predicted once dispatch overhead on
an in-order core is accounted for; a build tuned specifically for content resembling
`--pattern runs` (busy, low-redundancy scenes) would be measurably better off with mask 3.

### Register-spill check (`objdump`, Pi binary, default mask)

`objdump -d --disassemble=snes_ntsc_blit_hires_vec` on the Pi binary: the function
(unmangled symbol, as expected for `extern "C"`) is 1483 instructions. Searching for
NEON/ASIMD 128-bit register (`q0`-`q31`) loads/stores addressed off `sp` (the stack
pointer, i.e. genuine spills - as opposed to the many `str qN, [x0, #...]` stores to the
output-buffer pointer, which are the kernel's actual pixel output, not spills) found
exactly **3 `str` + 3 `ldr`**, all in two small clusters (`0xde00`-`0xde7c` and
`0xe5d4`-`0xe874`). That is consistent with the 3 per-call hoisted constants
(`clamp_mask_vec`, `clamp_add_vec`, `alpha_vec`/`fixed_tail`) being spilled once each near
the top of this very large, heavily-inlined function and reloaded a handful of times where
needed - not the "every accumulator round-trips through the stack on every pixel" pattern
Stage 2's own register-allocation fix diagnosed and fixed (see above): 6 spill-related
instructions out of 1483 (0.4%) is a rounding error, confirming the `CS`/`isFlat`
macro-not-lambda design is not reintroducing that problem at this larger function size.

Measurement note: the Pi (`sl-d303`) was found to be shared with another concurrent SSH
session during part of this stage's testing (a second `sysadmin` login independently
running the same kind of NTSC benchmark, visible via `ps`/`who` mid-session) - readings
taken during that window were discarded and re-measured with the machine otherwise idle;
every table in this file is from idle-machine runs.

### Per-path statistics (host, `-O3 -march=native`, `-DSNES_NTSC_VEC_STATS`)

Chunk-granularity for skip/uniform, pair-decision-granularity (3 per non-skip/non-uniform
chunk) for pair/general:

| Input | Steady run (skip) | Uniform chunk (CK) | Pair (PK) | General (K) |
|---|---|---|---|---|
| `--input` frames (24 captured, all 3 phases) | 59.2% of chunks | 17.8% of chunks | 69.5% of remaining pair-decisions | 30.5% of remaining pair-decisions |
| `--pattern runs` | 5.5% of chunks | 22.3% of chunks | 40.4% of remaining pair-decisions | 59.6% of remaining pair-decisions |

On real content, skip+uniform already cover 77% of all chunks before pairs are even
considered, and of the chunks that do fall through, 69.5% of pairs still match - both
numbers are consistent with `frame_stats.py`'s "65% flat chunks, 91% identical pairs"
(counted differently: per-6-pixel-chunk here vs `frame_stats.py`'s own methodology) and
confirm the ladder is exercising exactly the shortcut real game content was designed
around. `--pattern runs` is deliberately far more balanced across all four paths, as
intended - it is a ladder *stress test*, not a real-content simulation. Re-run after the
Stage 3 review's group-level fast path (which bypasses the per-chunk `SNES_NTSC_VEC_CHUNK`
macro's own stat counters for an all-skip group, compensated with equivalent counter
increments in that fast path - see `snes_ntsc.cpp`): both percentages above are unchanged
to one decimal place, confirming the fast path classifies the same chunks the same way,
just processes them more cheaply.

### FLAT constancy check debug output

Every checksum run above prints a line like:

```
snes_ntsc: FLAT constancy check: 8194/8194 slot-generations held (7 stored values equal within each phase, and equal across all 3 phases)
```

from the debug-only (`#ifndef NDEBUG`) check added to `gen_slot()`. Since the Stage 3
review's first change, the 7 values it compares are the stored, already clamped+converted
(Alevel=0) FLAT words rather than raw pre-clamp sums - the property under test (would the
blitter emit the same output at every position) is unchanged, and checking it on the
values actually stored is, if anything, more direct. It held on every one of the thousands
of slot-generations run across every synthetic pattern and every captured frame's actual
palette in this stage - 8194/8194 on every synthetic-palette run, and up to
853,125/853,125 on a single `--input` checksum+timed run (each palette-table diff during
that run regenerates the changed slots, so the count reflects total regenerations over the
run, not just the 8192 distinct slots). Zero failures observed anywhere, before or after
the review's changes. `-DNDEBUG` (the game's Release build) compiles this check out
entirely, exactly as specified; the game build below reproduces the game's actual flags
and therefore does not print it.

### Game build and run (WSL, `/home/pearc/cannonball-se-dump`)

`cmake --build build -j16` with `CPATH=~/localdeps/usr/include/x86_64-linux-gnu:~/localdeps/usr/include`
produced no new warnings after one fix: `snes_ntsc_pk_slot_words`'s enum-times-enum
multiplication initially triggered `-Wdeprecated-enum-enum-conversion` (two different
unnamed enum types), fixed by casting both operands to `int` before multiplying. After that
fix, the only warnings are the same pre-existing ones Stage 2 already documented
(`-Wcomment` on the commented-out dead code block in `snes_ntsc.h`, now at line 474; a
handful of unrelated warnings elsewhere in `gl_backend.hpp`/`video.cpp`/`rendersurface.cpp`
untouched by this stage).

Both perftest+dump invocations exited 0 and produced 5 valid frames each, verified by
`frame_stats.py` (`verify=OK` on all 5, 0 FAILED), re-run a third time after the Stage 3
review's three changes (FLAT stored pre-clamped, the group-level fast path, the cheaper
clamp+convert) to confirm they changed nothing observable - identical stats and `verify=OK`
every time:

```
SDL_VIDEODRIVER=offscreen ./build/cannonball-se -cfgfile res/config-dump.xml -perftest -t 1 -dump .../frames_stage3_t1 61 5   # exit 0
SDL_VIDEODRIVER=offscreen ./build/cannonball-se -cfgfile res/config-dump.xml -perftest -t 4 -dump .../frames_stage3_t4 61 5   # exit 0
```

`SDL_VIDEODRIVER=offscreen` was needed in this WSL session specifically (the default
driver hung waiting on a compositor with no frames ever produced; `dummy` failed outright
needing OpenGL the dummy driver doesn't provide) - an environment quirk of this sandbox,
unrelated to the NTSC filter change; `wayland`/the default driver is what a real desktop
session should use per the game's own startup hint. Frame stats on the 5 dumped frames
(both `-t 1` and `-t 4`, identical): avg 200 distinct raw slots, 91 distinct colours, 83.5%
flat 6-pixel chunks, 95.7% identical even/odd pairs, 89.6% vertical repetition - consistent
with the `frames/` corpus statistics this filter is designed around.
