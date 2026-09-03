# ntsc_bench: minimal throughput harness for the Blargg hires NTSC filter

Target: `src/main/sdl2/snes_ntsc.{cpp,h}` hires blitters, as called by
`RenderSurface::blargg_filter()` in hires mode. Primary test mule: `sl-d303`
(Raspberry Pi Zero 2 W: 4x Cortex-A53 @ 1 GHz, 512 MB, Debian 13, GCC 14.2,
reachable from WSL as `ssh sl-d303`).

## 1. What the game actually does (the thing we must reproduce)

| Item | Value in hires mode | Source |
|---|---|---|
| Input frame | 640 x 448 `uint16_t` (808 x 448 in wide mode) | video.cpp: s16 dims doubled when hires |
| Input pixel value | direct index into the 32 MiB kernel table (`table[0x10000][128]` of uint32) | snes_ntsc.h `SNES_NTSC_S16` |
| Input value range | 15-bit RGB + shadow bit (bit 15), produced by `rgb_blargg[]` palette lookup; at most 8192 distinct per frame, typically a few hundred | renderbase.hpp/cpp |
| Output | 32-bit RGBA, width `SNES_NTSC_OUT_WIDTH_SIMD(640)` = 752 (948 for 808), pitch = width*4 | rendersurface.cpp:181,993 |
| Call | `snes_ntsc_blit_hires_fast(ntsc, in, 640, phase, 640, 224, out, 752*4, 255)` | rendersurface.cpp:1007 |
| Threading | frame split top/bottom (224 rows each) across 2 render threads when `threads > 2` | main.cpp:491, blargg_filter(section) |
| Phase | `(phase+1)%3` each frame at 60 fps | rendersurface.cpp:1287 |
| Setup | `snes_ntsc_composite` preset, `merge_fields=0`, all sliders at default | rendersurface.cpp:196-219 |
| OpenMP | used only in `snes_ntsc_init()` table build, not in blitters | snes_ntsc.cpp:119 |

The kernel is a **table gather**: each input pixel selects a 512-byte kernel
entry and each output pixel sums 12 uint32 loads from 12 different entries.
So the working set is `distinct_colours * 512 B`. The Pi L2 is 512 KB.

## 2. Requirements

1. Minimal: one `.cpp`, no SDL, no CMake, no game headers. Links only
   `snes_ntsc.cpp`. Builds with one `g++` line on any target the filter supports.
2. Revision-agnostic: the same bench source must build against commit
   `086591c` (has `_fast` only), the working tree (`_fast` + `_fast2`), and any
   future rewrite, without editing the bench.
3. Reproducible and comparable across revisions and machines: deterministic
   input, deterministic checksum of the output, so bit-exactness between
   variants is a one-line comparison of two runs.
4. Measures sustained frame rate of the filter alone.

## 3. Design

### 3.1 One binary = one function under test

The function is selected at compile time:

    g++ -O3 -mcpu=cortex-a53 -std=c++17 -DBENCH_FN=snes_ntsc_blit_hires_fast2 \
        ntsc_bench.cpp snes_ntsc.cpp -o bench_fast2

`BENCH_FN` defaults to `snes_ntsc_blit_hires_fast`. Any function with the
blitter signature works. The scalar reference `snes_ntsc_blit_hires` is
included via `-DBENCH_SCALAR`, which also switches the output width to
`SNES_NTSC_OUT_WIDTH(320)` = 749 as the non-SIMD game build does. This is
what makes the bench revision-agnostic: the preprocessor never has to know
which functions exist.

### 3.2 Input buffer

Filled once, deterministically (fixed-seed xorshift32), then reused every frame.

- `--pattern random` (default): every pixel is spatially random, but drawn from
  a palette of `--colours N` distinct values (default 256, valid 15-bit RGB
  plus a random shadow bit). Spatial randomness defeats the "24 pixels equal"
  repeat-block shortcut so the full kernel runs on every chunk. The limited
  palette keeps the table working set realistic (256 * 512 B = 128 KB).
  `--colours 65536` gives the fully random, table-thrashing worst case.
  Both numbers are worth having: the gap between them says how much of the
  frame time is gather/cache-miss cost versus arithmetic, which is exactly
  the question a SIMD rewrite has to answer.
- `--pattern mixed`: top third flat colour, middle third horizontal
  gradient, bottom third noise. Exercises the repeat-block path the way real
  OutRun frames (sky, road) do.

### 3.3 Frame geometry

Default 640 x 448 (`--width 808` for wide). Row pitch equals width, as in
the game. Output buffer is 64-byte aligned, height x (out_width*4) bytes.
Buffers sit outside L2 (input 573 KB, output 1.35 MB) as they do in the game.

### 3.4 Measurement loop

1. `snes_ntsc_init(ntsc, &snes_ntsc_composite)` once (this is the slow,
   OpenMP-parallel table build; excluded from timing).
2. Warm-up: 10 frames.
3. Timed: loop until `--seconds S` (default 5) elapses, calling `BENCH_FN`
   once per frame with `phase = frame % 3`, counting frames and tracking
   min/max frame time with `steady_clock`.
4. Report: frames, mean ms/frame, fps, min/max ms.

`--threads T` (default 1): splits the frame into T horizontal bands, each
processed by its own `std::thread` per frame, mirroring the game's 2-thread
split. Use `--threads 2` to see whether two cores sharing the 512 KB L2 and
the memory bus scale, or whether the filter is bandwidth-bound.

### 3.5 Correctness checksum

After timing, three more frames are run at phases 0, 1, 2 and a 64-bit
FNV-1a is accumulated over the full output buffer after each. Printed as hex.
Identical input + identical semantics => identical checksum on any machine,
any revision. This is the bit-exactness gate for `_fast2` and for any
rewrite: compare the checksum against the `_fast` binary built from `086591c`.

### 3.6 Output format

Human-readable header, then one greppable line:

    RESULT fn=snes_ntsc_blit_hires_fast rev=086591c simd=NEON w=640 h=448 \
      pattern=random colours=256 threads=1 frames=312 ms=16.03 fps=62.4 \
      min=15.8 max=17.1 checksum=0x1a2b3c4d5e6f7081

`rev` comes from `-DBENCH_REV="..."` supplied by the build script.

## 4. Driving it on the Pi: `run_pi.sh` (run from WSL)

    tools/ntsc_bench/run_pi.sh <rev|WT> <function> [bench args...]

1. Stage: extracts the four filter files (`snes_ntsc.cpp/.h`,
   `snes_ntsc_config.h`, `snes_ntsc_impl.h`) with `git show <rev>:path`, or
   copies them from the working tree for `WT`, into `tools/ntsc_bench/stage/<rev>/`
   together with `ntsc_bench.cpp`.
2. Ship: `rsync` the stage dir to `sl-d303:~/ntsc_bench/<rev>/`.
3. Build: `g++ -O3 -mcpu=cortex-a53 -std=c++17 -DBENCH_FN=<fn> -DBENCH_REV=<rev>`
   (about 25 s on the Pi; binaries are cached per rev/fn).
4. Run: governor set to `performance` for the run, `taskset -c 3` to pin away
   from the resident services on that box (owntone, autostream), print the
   RESULT line, restore governor. Check `vcgencmd get_throttled` before and
   after so a thermally throttled run is flagged.

Cross-check on the host (x86 SSE path, WSL): `run_host.sh` does the same
build locally. Checksums must match the Pi: the SSE and NEON paths are
supposed to be bit-identical, and this catches a lane-order or clamp bug on
either side.

## 5. Test matrix for this exercise

| Build | Function | Purpose |
|---|---|---|
| 086591c | `_fast` | baseline: shipped code, reference checksum |
| WT | `_fast` | confirms the uncommitted header edits did not change `_fast` |
| WT | `_fast2` | the A7-tuned variant: checksum must equal baseline; timing vs baseline |
| 086591c | `snes_ntsc_blit_hires` (scalar) | how much the current SIMD is actually buying on an A53 |

Each at `--colours 256` and `--colours 65536`, `--threads 1` and `2`. Three
runs each, report the median.

## 6. What the numbers mean

At 60 fps with the 2-thread split, each render thread has 16.7 ms for 224
rows, and that thread also does the palette conversion and scanline pass.
So a full 448-row frame single-threaded must come in well under about 25 ms
for the filter not to be the bottleneck on this class of hardware. The
`--colours 256` vs `65536` gap plus the `--threads 2` scaling tell us whether
a rewrite should attack arithmetic (wider SIMD, fewer lane moves) or the
gather (kernel table layout, entry size, prefetch), before any code is written.

## 7. Existing `bench.cpp`

The untracked `tools/ntsc_bench/bench.cpp` compares `_fast` and `_fast2`
inside one binary, so it cannot build against `086591c` and cannot be reused
for a rewrite without editing. `ntsc_bench.cpp` supersedes it; `bench.cpp` is
left in place for now and can be deleted once the new harness is accepted.
