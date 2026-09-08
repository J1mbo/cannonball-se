# ntsc_bench

Minimal throughput/correctness harness for the Blargg SNES NTSC hires
blitters (`src/main/sdl2/snes_ntsc.{cpp,h}`). See "Why the harness is built
this way" below for the design rationale, and "See also" at the end for the
rest of the documentation set.

## Current state (post-Stage 4)

The NTSC filter re-architecture (`analysis/ARCHITECTURE.md`) is complete through Stage 4.
This section is the quick-reference summary; the full per-stage narrative (with the
measurements and bugs behind every decision) follows below, and `analysis/ARCHITECTURE.md`
section 13 lists where the shipped code deviates from the original proposal.

**Functions.** `snes_ntsc_blit` (lores, untouched by any stage) and
`snes_ntsc_blit_hires_vec` (hires - the *only* hires blitter as of Stage 4: the old per-ISA
`snes_ntsc_blit_hires_fast` and the separate scalar `snes_ntsc_blit_hires` have both been
deleted from the working tree). `snes_ntsc_blit_hires_vec` is built on a portable v4u32
SIMD layer (`src/main/sdl2/snes_ntsc_simd.h`, NEON/SSE2/RVV backends plus a plain-C
fallback selected automatically, or forced with `-DSNES_NTSC_FORCE_SCALAR_SIMD`), so it is
available and correct on every platform, including ones with no native SIMD ISA at all.
Inside it, a fused, lookahead-free run-length classifier (no row-wide pre-pass, no
`std::vector`) picks per 6-pixel chunk between four paths - steady run (free), uniform
chunk (`plane_ck`), identical pair (`plane_pk`, off by default - see below), and the
general per-pixel kernel (`plane`) - all four producing the exact same 32-bit wrap-around
sum. `SNES_NTSC_LADDER_MASK` (default **3** = flat/skip + uniform-chunk, pair off) selects
which levels are compiled in; see "Ladder default" below for why.

**Tables**, all in `snes_ntsc_t` (`snes_ntsc.h`), phase-major (`[burst 0..2][slot
0..8193][...]`) and regenerated per slot by `gen_slot()`/`snes_ntsc_update_slot(s)()`:

| Table | Contents | Block shape | Size |
|---|---|---|---|
| `plane` | base 14-tap kernel, 3 alignments | 20 words (3 lead + 14 real + 3 trail) | 5.9 MB |
| `plane_pk` | pair sums, 3 alignments | 24 words (3 lead + 15 real + 6 trail) | 7.1 MB |
| `plane_ck` | uniform-chunk sums, all 3 alignments combined | 28 words (3 lead + 19 real + 6 trail) | 2.75 MB |
| `plane_flat` | steady-run constant, pre-clamped/converted | 8 words | 0.79 MB |

`sizeof(snes_ntsc_t)` is **16,527,360 bytes** (~15.76 MiB / 16.5 MB) - confirmed by the
bench's `sizeof_ntsc=` field on every run below.

**Checksum references** (640x448 unless noted; table built like the game - composite
preset, `merge_fields` cleared, exactly as `RenderSurface::init_blargg_filter()` does).
These are unchanged since Stage 2 (Stage 3 and 4 are both bit-identical to it) and hold on
every build tested - host SSE2/SSE4, host plain-C fallback, Pi NEON, Pi plain-C fallback -
and at every `SNES_NTSC_LADDER_MASK` value (0/1/2/3/7):

| Input | Checksum |
|---|---|
| `--pattern random --colours 256` (default) | `0x95f5c92e642a686f` |
| `--pattern random --colours 65536` | `0x50df72f5a78f9e6d` **(no longer reproducible - see note)** |
| `--pattern mixed --colours 256` | `0x2921e1e8ab232056` |
| `--pattern runs --colours 256` | `0x0f9860292b10dd73` |
| `--input tools/ntsc_bench/frames` (24 captured frames, all 3 phases) | `0xe12371d9049c3d61` |
| `--width 808 --pattern random --colours 256` | `0xea6c5d4426a031f3` |
| `--threads 2` (any of the above) | same as `--threads 1` |
| `--lores` (unaffected by every stage; regression-only) | `0x18b1960769cc63dd` |

The captured-frame corpus is **not distributed** - the frames are rendered OutRun graphics
and so are SEGA's copyright, exactly like the ROMs. Regenerate it from your own revision B set
with `tools/ntsc_bench/capture_frames.sh`, which verifies the result against the committed
per-frame reference list `frames/frames.sha256`. That list and the checksum above were produced
at commit `180c093` and confirmed byte-identical on x86-64 and 32-bit ARMv7 (Cortex-A7).
Deliberate changes to engine output re-baseline them - `0xe3557fa06310379d` was the value before
`7100ec0` restored exact ROM sprite sizes.

Two caveats on this table, both verified 2026-09-08:

- **The synthetic-pattern rows are specific to an FMA-capable build.** They were produced
  with `-march=native`. A baseline build (`-march=x86-64`), or `-ffp-contract=off` on a
  native build, produces different values for `random`, `mixed` and `runs` - because the
  floating-point *table* generation contracts differently, not because the blitter differs.
  The captured-frames row is unaffected by any of this and is the row to compare when
  checking portability across platforms or ISA levels. Full detail and figures:
  `analysis/ARCHITECTURE.md` section 17.1.
- **`--colours 65536` can no longer be requested.** The slot-indexed table introduced in
  Stage 1 caps capacity at `snes_ntsc_slot_count` (8192 in the current tree); the bench
  clamps the value and prints a warning, so this row's checksum cannot be reproduced as
  written. It is retained as a record of the pre-Stage-1 table. Use `--colours 8192` for a
  full-table run today.

**Comparing against the pre-Stage-4 code (a6dfa98, "Stage 3" in this file's history):**
`run_host.sh`/`run_pi.sh` stage whichever revision's own `snes_ntsc.{cpp,h}` alongside the
*current* `ntsc_bench.cpp`, so the retired functions remain buildable and checksummable for
reference even though they no longer exist in the working tree:

```sh
tools/ntsc_bench/run_host.sh a6dfa98 snes_ntsc_blit_hires_fast --pattern random --colours 256
tools/ntsc_bench/run_host.sh a6dfa98 snes_ntsc_blit_hires      --pattern random --colours 256   # scalar, -DBENCH_SCALAR auto-added
tools/ntsc_bench/run_pi.sh   a6dfa98 snes_ntsc_blit_hires_fast --input tools/ntsc_bench/frames --seconds 5
tools/ntsc_bench/run_host.sh WT      snes_ntsc_blit_hires_vec  --pattern random --colours 256   # current code
```

All four must (and do) reproduce the checksums above for the inputs they share.

**Pi timings** (test mule, `--seconds 5`, median of 3, performance governor, pinned core 3) -
`vec` (Stage 4, default mask) vs. the retired `_fast` and scalar, on the four inputs that
matter (random 256 = raw kernel cost floor, mixed/runs = synthetic ladder exercise, `--input`
= real game content, the corpus the whole ladder is designed around):

| Function | random 256, ms | mixed, ms | runs, ms | `--input`, ms | `--input` filter-only, ms |
|---|---|---|---|---|---|
| `snes_ntsc_blit_hires_vec` (Stage 4) | 14.27 | 9.17 | 12.30 | **7.72** | **5.23** |
| `snes_ntsc_blit_hires_fast` (retired, a6dfa98) | 16.65 | 13.34 | 13.65 | 8.52 | 6.03 |
| `snes_ntsc_blit_hires` (retired scalar, a6dfa98) | 18.61 | 14.05 | 14.97 | 13.87 | 11.38 |

Stage 4's fused classifier (no row-wide pre-pass) improves on Stage 3's own `--input`
figures (9.26 ms / 6.77 ms filter-only) by a further 17% / 23%, and for the first time
`vec` **beats** the retired `_fast` on real content (7.72 vs 8.52 ms, 9.4% faster) rather
than trailing it - the goal Stage 3's review called "nearly but not fully realised" is met
here. Filter-only (5.23 ms) is also now clearly inside `analysis/ARCHITECTURE.md` section
11's "at or under 6 ms" gate (Stage 3 was 6.77 ms, "close but not quite under").

L1-resident (`--colours 16`, single 5 s run, cycles/output = `ms * 1e6 / (448 * out_width)`
at the confirmed 1 GHz Cortex-A53 clock): `vec` 8.23 ms (**24.4** cycles/output), retired
`_fast` 10.82 ms (32.1), retired scalar 11.10 ms (33.1, `out_width` 749) - `vec` remains the
fastest of the three on every metric measured except real-content total time in Stage 3,
which Stage 4 has now also closed (see the table above).

**Ladder default.** `SNES_NTSC_LADDER_MASK` defaults to **3** (flat/skip + uniform-chunk),
not 7 (the Stage 3 default, which also enabled the identical-pair path). The Stage 3 mask
experiment (further down) measured the pair level giving no measurable benefit on real
frames while regressing transition-heavy synthetic content (`--pattern runs`) by 32%. The
pair table (`plane_pk`) and code (`SNES_NTSC_VEC_PK`) are both kept - only the default
changed - so `BENCH_CXXFLAGS=-DSNES_NTSC_LADDER_MASK=7` (or any other value) still builds
and still passes every checksum above.

## Where the per-stage narrative moved

The chronological Stage 0-4 development log that used to follow "Current state" here -
including the three real bugs the checksum gate caught along the way (the
`ROTATE_OUT_SSE4` lane-swap, the Stage 3 double-clamp/convert cost bug, and the Stage 4
retro-add double-counting bug) and the working-set/page-spread memory experiments - has
moved to `analysis/ARCHITECTURE.md`: sections 14 (reviewer's-eye "what changed and
why"), 15 ("Notable bugs found via this harness") and 16 (the memory experiments,
general Cortex-A53 findings rather than just evidence for this filter).

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
  run-length ladder (`snes_ntsc_blit_hires_vec`'s uniform/pair/general chunk
  classification - see `analysis/ARCHITECTURE.md` section 6). Each row is a sequence of runs
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
tools/ntsc_bench/run_pi.sh   <rev|WT> <function> [bench args...]  # test mule (ARM)
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

**This table is a historical record of the Stage 3/4 rework (2026-09-03) and is not
reproducible at HEAD.** "working tree" means the tree as it stood then, and its captured-frame
column is the pre-`7100ec0` corpus (`0xe3557fa06310379d`); today's corpus checksums
`0xe12371d9049c3d61`. The table is kept because it is the evidence for the `ROTATE_OUT_SSE4`
diagnosis below, not as a gate to run. The runnable references are the ones at the top of this
file.

| Function (rev) | random, 256 colours | random, 65536 colours | mixed | 24 captured frames, host/SSE4 (`--input`) | 24 captured frames, Pi/NEON (`--input`) |
|---|---|---|---|---|---|
| snes_ntsc_blit_hires_fast (086591c) | 0x95f5c92e642a686f | 0x50df72f5a78f9e6d | 0x2921e1e8ab232056 | 0xf977fe072096daea | 0xe3557fa06310379d **(MISMATCH, see below)** |
| snes_ntsc_blit_hires_fast (working tree, fixed - see `analysis/ARCHITECTURE.md` section 15.1) | 0x95f5c92e642a686f (unchanged) | 0x50df72f5a78f9e6d (unchanged) | 0x2921e1e8ab232056 (unchanged) | 0xe3557fa06310379d | 0xe3557fa06310379d (matches) |
| snes_ntsc_blit_hires_fast2 (working tree) | 0x95f5c92e642a686f | same as _fast | 0x7d137397ad358e78 (MISMATCH: repeat-block bug) | not measured (an early re-architecture stage measured `_fast`/scalar only) | not measured |
| snes_ntsc_blit_hires scalar (086591c), out_width 749 | 0x8a54cf7b7096697e | 0x5afd2d5d92a24084 | 0x8390b47c580c191e | 0xa01757db54ee3c6f | 0xa01757db54ee3c6f (matches) |

SSE4 (host) and NEON (Pi) builds of the same source produce identical
checksums on every column above **except** `snes_ntsc_blit_hires_fast (086591c)`
on the 24 captured frames, which differs between host and Pi (both
stable/reproducible on their own platform). This was the `ROTATE_OUT_SSE4`
lane-order bug (root-caused and fixed in the working tree; see
`analysis/ARCHITECTURE.md` section 15.1 for the full symptom/root-cause/fix
write-up): the host/SSE4 checksum on captured frames changes from
`0xf977fe072096daea` to `0xe3557fa06310379d`, which now matches the Pi/NEON
figure bit-for-bit (the Pi/NEON side was already correct and is unchanged by
the fix). Every other checksum in this table - both synthetic patterns, on
both platforms - is unchanged by the fix, as expected. The scalar path
agrees cross-platform on captured frames just as it always has on the
synthetic patterns. `snes_ntsc_blit_hires_fast` and the scalar
`snes_ntsc_blit_hires` disagree with each other on every full-buffer
checksum, as expected: they use different `out_width` (752 vs 749), so even
identical per-pixel math produces a different byte stream.

`--lores` reference (`snes_ntsc_blit`, synthetic 320x224, random 256 colours,
086591c): checksum `0x18b1960769cc63dd`. Not comparable to the hires rows
above (different blitter, different geometry) - it exists to regress the
lores path in later stages once it's touched by the re-architecture.

## Ladder-level experiment (`SNES_NTSC_LADDER_MASK`, test mule, `--seconds 5`, median of 3)

`run_pi.sh` now passes `BENCH_CXXFLAGS` through exactly like `run_host.sh`
(e.g. `BENCH_CXXFLAGS=-DSNES_NTSC_LADDER_MASK=1 tools/ntsc_bench/run_pi.sh WT
snes_ntsc_blit_hires_vec ...`), forcing a rebuild whenever it is set. Bit0 = flat/skip,
bit1 = CK (uniform chunk), bit2 = PK (pair); a cleared bit routes that case to the next
path down rather than deleting it (mask 0 = pure general path via the ladder's own
fallback; mask 7 = the full ladder, the shipped default **at the time of this experiment -
see "Stage 4" below, this table is exactly what motivated changing the default to 3**).
Checksums matched the full reference set at every mask value (all four below reproduce
`0xe3557fa06310379d` on `--input` and `0x0f9860292b10dd73` on `--pattern runs`):

| Mask | Levels enabled | `--input`, ms | `--pattern runs`, ms |
|---|---|---|---|
| 0 | none (general path only) | 12.36 | 12.06 |
| 1 | flat/skip | 9.92 | 12.40 |
| 3 | flat/skip + CK (**Stage 4's shipped default**) | **9.26** | **11.99** |
| 7 | flat/skip + CK + PK (Stage 3's shipped default) | **9.26** | 15.81 |

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
  `ROTATE_OUT_SSE4` bug (`analysis/ARCHITECTURE.md` section 15.1), since it
  excludes the trailing columns where the two blitters' end-of-line handling
  legitimately differs. Without `--cols`, defaults to the smaller of the two
  dumps' `out_width`. Reports total/per-image diff counts, the first 20
  differences as `(frame, phase, row, col, valueA, valueB)`, histograms of
  differing columns mod 28 (the `_fast` unroll period: 4 chunks x 7 outputs)
  and mod 7, the set of rows affected, and a run-length summary of
  consecutive differing columns.

`ntsc_bench --input frames/` (or a single file, or a comma-separated list -
see Usage above) benches and checksums this corpus directly; `run_pi.sh`
ships `frames/*.bin` to the Pi and translates a local `--input` path under
this directory automatically (see comment at the top of `run_pi.sh`).

## Why the harness is built this way

`ntsc_bench.cpp` is deliberately minimal - one `.cpp`, no SDL, no CMake, no game
headers, linking only `snes_ntsc.cpp` - so it builds with one `g++`/`cl` line against
any revision of the filter, and the same source can measure a function that no longer
exists in the working tree by staging that revision's own header alongside the current
bench (see "Comparing against the pre-Stage-4 code" above).

- **One binary, one function under test, selected at compile time**
  (`-DBENCH_FN=<function>`). Any function with the blitter signature works, and the
  same bench source builds unmodified against commit `086591c` (`_fast` only), the
  working tree, or a future rewrite - the preprocessor never has to know which
  functions exist. This is what makes the bench revision-agnostic.
- **`--colours 256` vs `--colours 65536` isolate arithmetic cost from gather/cache-miss
  cost.** A limited palette (default 256) keeps the table working set realistic
  (256 x 512 B = 128 KB) while spatially random placement still defeats the
  repeat-block shortcut; the fully random 65536-colour case is the table-thrashing
  worst case. The gap between the two numbers says how much of the frame time is
  memory stall versus pure arithmetic - the question any rewrite has to answer before
  choosing where to spend effort.
- **A 64-bit FNV-1a checksum over three extra frames (phases 0, 1, 2) is the
  bit-exactness gate.** Identical input and identical semantics must produce an
  identical checksum on any machine, any ISA, any revision - this is what makes
  "compare the checksum against `086591c`" a one-line check for every rewrite, and is
  how every bug in `analysis/ARCHITECTURE.md` section 15 ("Notable bugs found via this
  harness") was caught: a checksum mismatch on a specific input pattern, localised
  with `diff_output.py`.
- **Driver scripts (`run_pi.sh`/`run_host.sh`) own staging, build, and run**, including
  governor and `taskset` handling on the Pi and a `vcgencmd get_throttled` check before
  and after so a thermally-throttled run is visible - see "Build"/"Usage" above for the
  details.
- **Budget context.** At 60 fps with the 2-thread split, each render thread has
  16.7 ms for 224 rows and also performs the palette conversion and scanline pass in
  the same time, so a full 448-row frame single-threaded needs to come in well under
  about 25 ms for the filter not to be the bottleneck on this class of hardware.

## See also

- `analysis/ARCHITECTURE.md` - the re-architecture's design (Parts A-D), the deviations
  from the original proposal (section 13), the reviewer's-eye "what changed and why"
  (section 14), the three notable bugs this harness caught (section 15), and the
  working-set/page-spread memory experiments (section 16).
- `analysis/ALGORITHM.md` - the formal spec of the scalar filter's table layout, packed
  word format and closed-form output formula that every rewrite is checked against.
- `../GRAPHICS_SETUP.md` - the surrounding renderer/palette pipeline (slot
  buffers, `convert_palette()`, palette-write frequency) that the slot-indexed table
  design depends on.
