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
              [--seconds S] [--threads T] [--palette random|contiguous] [--hugepages] \
              [--input <path>] [--lores] [--dump-output <file>]
```

Defaults: `random`, 256 colours, 640x448, 5 seconds, 1 thread, `--palette
random`, `--hugepages` off, no `--input` (synthetic input), `--lores` off.
The input frame is filled once with a deterministic xorshift32 RNG (fixed
seed) from a palette of `--colours` distinct values, then reused unchanged
every frame - unless `--input` is given (see below).

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
