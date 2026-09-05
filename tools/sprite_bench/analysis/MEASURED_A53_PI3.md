# Measured on a real Cortex-A53: store strategies for the sprite kernel

Date: 2026-09-04. Machine: Raspberry Pi 3 Model A+ (`pi3`, 10.240.1.84), BCM2837,
4x Cortex-A53 (CPU part 0xd03), 1.4 GHz nominal, 32 KB L1D/L1I per core, 512 KB
shared L2, 512 MB RAM, Debian 13, GCC 14.2, built `-O3 -mcpu=cortex-a53`.
Benchmark pinned with `taskset -c 3`, governor `performance`.

Harness: `scratchpad/pi3/sprite_store_bench.c`. Counters read directly through
`perf_event_open` (no `perf` tool on the box): cycles, instructions, L1D refill
(0x03), L2D refill (0x17), branch mispredicts (0x10), A53 store-stall cycles
(0xE8). The reported row is the fastest of 11 repetitions.

Workload: 26 synthetic blob-shaped sprites, 310,955 output pixels, drawn into a
640x448 16-bit buffer (560 KB) already filled with a background. Zooms drawn
from the hi-res default range 0x80..0x1F8, so every sprite magnifies, matching
the shipping configuration. Transparency is present; **shadow pixels, the row
terminator and edge clipping are excluded** so that the store pattern is what is
being measured.

**Correctness gate:** the checksum covers the whole allocation including the
leading pad and the tail, so any store landing outside its intended span shows
up. All four real strategies produce identical checksums in every condition.

## The four strategies

| | Description |
|---|---|
| REF | The upstream shape. Per output pixel: accumulator test, transparency test, one 16-bit store. Source row re-decoded for every destination row. No clip test, so this is the fork's no-clip fast path and REF is flattered slightly. |
| CF1 | Closed-form run lengths from a per-sprite precomputed table, per source pixel, exact-width stores, one destination row at a time. |
| FANOUT | Per source pixel, one wide store of MAXRUN copies with overlap, exact store at a run end via one-pixel lookahead, fanned to all `r_v` destination rows from hoisted row pointers. MAXRUN follows the zoom class (4/2/1). This is the shape `DESIGN_REVIEW_STAGING.md` recommended. |
| LINEBUF | Decode the source row once into an L1 scratch line buffer with overlapping wide stores, record opaque spans, then copy each span to each of the `r_v` destination rows in 16-byte chunks. This is the shape `DESIGN_FABLE.md` recommended. |
| FLOOR | Not a real strategy. Writes every destination pixel of every sprite with a constant, no decode and no transparency. An upper bound on store traffic and therefore a lower bound on achievable time. |

## Results

### Full 560 KB destination, idle (clock verified at 1395-1412 MHz)

| strategy | cyc/px | ms/frame | insts/px | L1D rf/px | L2 rf/px | brmis/px | vs REF |
|---|---|---|---|---|---|---|---|
| REF      | 11.70 | 2.608 | 13.94 | 0.0216 | 0.0144 | 0.1174 | 1.00x |
| CF1      | 11.75 | 2.614 | 14.30 | 0.0230 | 0.0204 | 0.1090 | 1.00x |
| FANOUT   |  7.36 | 1.634 |  8.54 | 0.0216 | 0.0116 | 0.0214 | **1.59x** |
| LINEBUF  |  5.81 | 1.289 |  6.42 | 0.0247 | 0.0091 | 0.0393 | **2.01x** |
| LINEBUF, 32-byte chunks | 5.79 | 1.286 | 6.25 | 0.0263 | 0.0084 | 0.0384 | 2.02x |
| FLOOR    |  0.82 | 0.181 |  0.71 | 0.0234 | 0.0043 | 0.0059 | 14.3x |

### Full 560 KB destination, with a memory thrasher on another core

Cycle counts are valid; wall-clock is not, because the board throttled (below).

| strategy | cyc/px | vs REF |
|---|---|---|
| REF      | 12.39 | 1.00x |
| CF1      | 13.63 | 0.91x |
| FANOUT   |  8.27 | 1.50x |
| LINEBUF  |  7.02 | **1.76x** |
| FLOOR    |  2.16 | 5.7x |

### Small 15 KB destination, compute-bound

| strategy | cyc/px |
|---|---|
| REF | 12.14 |
| CF1 | 12.04 |
| FANOUT | 7.91 |
| LINEBUF | 6.08 |
| FLOOR | 0.72 |

## What the numbers say

**1. The line buffer wins, and `DESIGN_REVIEW_STAGING.md` was wrong to drop it.**
LINEBUF beats FANOUT in every condition: 5.81 against 7.36 cycles per pixel idle,
7.02 against 8.27 under contention. The review's model counted total memory
operations (0.55 per pixel for FANOUT against 0.85 for the line buffer) and
concluded FANOUT should win. It did not weight *frame-buffer* stores, which miss
a contested L2, against line-buffer traffic, which stays in L1. Counting only
frame-buffer stores: LINEBUF issues one 16-byte store per 8 output pixels, about
0.125 per pixel; FANOUT issues one store per source pixel per destination row,
about 0.44 per pixel at the mean magnification. FANOUT does roughly 3.5 times
more of the expensive kind. The line buffer earns its place by converting
scattered narrow destination writes into contiguous wide ones.

**2. The closed form alone buys nothing.** CF1 matches REF to within 0.5%. The
gain does not come from replacing the accumulator with a multiply and shift; it
comes from wide stores and from decoding a source row once instead of once per
destination row. Any staging that keeps a per-output-pixel store loop will land
on the reference's number.

**3. Branch mispredicts fall 3 to 6 times**, from 0.117 per pixel to 0.021
(FANOUT) and 0.039 (LINEBUF). That confirms the diagnosis that the reference's
per-pixel accumulator branch is a major cost, and it is removed by both new
shapes.

**4. The kernel is compute-bound, not memory-bound.** The pure-store floor is
0.82 cycles per pixel idle and 2.16 under contention, against LINEBUF's 5.81 and
7.02. Three to five cycles per pixel of compute sit above the floor even with a
thrasher running. This contradicts the expectation in `DESIGN_FABLE.md` that the
memory floor would be reached early and that effort should move to bytes
written. On this hardware there is real headroom left in the decode, and the
staged plan's later compute stages are worth doing rather than skipping.

**5. Wider replay chunks are a wash.** 32-byte chunks give 5.79 against 5.81
cycles per pixel. The replay is store-bound, not instruction-bound, so widening
it further is not the lever.

**6. Expected whole-frame effect.** At 310,955 output pixels and 1.4 GHz, the
sprite pass would move from 2.61 ms to 1.29 ms idle, or from 2.75 ms to 1.56 ms
under contention. That is roughly 1.2 to 1.3 ms returned to a 16.7 ms frame,
assuming the real sprite pixel count is close to the synthetic one, which has
not yet been confirmed against captured frames.

## Two caveats that matter

**The board is under-voltage throttled.** `vcgencmd get_throttled` reports
`0x50005`: under-voltage detected *now*, currently throttled, and both have
occurred before. Sampling the clock during a game run shows the ARM core sitting
at **600 MHz rather than 1400 MHz for most of the time**, while
`scaling_cur_freq` still claims 1400000 and the temperature is only 45 C, so
this is the power supply and not thermal. A Pi 3 A+ wants a 2.5 A supply.

Consequences: the idle benchmark rows above held 1400 MHz throughout and are
sound, and all cycle counts are frequency-independent and therefore valid. But
every wall-clock figure taken under load, and every whole-game frame rate below,
is depressed by roughly a factor of two.

**Whole-game figures are not yet meaningful.** With ROMs deployed and the game
run headless through KMSDRM in `-perftest`, it reports 46 fps with the default
four threads and about 43 fps single-threaded, at 640x448 with the Blargg filter
on. Four threads being barely faster than one is itself a symptom of the clock
starvation rather than a real pipeline result. These numbers should be retaken
on a properly powered board before anything is concluded from them, and the
phase split between game logic, road, tiles, sprites and text still needs the
instrumentation described in the measurement plan.

## Reproducing

```
rsync -a sprite_store_bench.c pi3mule:~/spritebench/
ssh pi3mule 'cd ~/spritebench && gcc -O3 -mcpu=cortex-a53 -pthread -o ssb sprite_store_bench.c'
ssh pi3mule 'taskset -c 3 ~/spritebench/ssb --reps 11'            # idle, full buffer
ssh pi3mule 'taskset -c 3 ~/spritebench/ssb --reps 11 --thrash'   # with contention
ssh pi3mule 'taskset -c 3 ~/spritebench/ssb --reps 11 --small'    # compute-bound
```

---

# In-situ measurement under real concurrent load (2026-09-04)

The synthetic thrasher used above is one thread doing strided read-modify-write
over 16 MB. Real gameplay is heavier and differently shaped: the game logic and
canvas rendering on one thread, the NTSC/RGB conversion of the previous frame on
two more, audio on another, and the GPU compositing the frame before that with
GLSL filters, all sharing one 512 KB L2 and one memory bus. So the sprite pass
was timed inside the running game instead.

Machine: `sl-d303`, Raspberry Pi Zero 2 W, 4x Cortex-A53 at 1.0 GHz, governor
`performance`, **`vcgencmd get_throttled` = 0x0 throughout** (unlike the Pi 3 A+,
this board is properly powered, so wall-clock here is trustworthy). Hi-res
640x448, Blargg NTSC filter on, widescreen off, attract mode, headless via
KMSDRM. Instrumentation is commit 957d35c: three clock reads per frame around
the layer groups in `Video::prepare_frame()`, plus a throwaway per-destination-
pixel counter in the sprite macros for the calibration run.

## Frame budget at 60 fps

| phase | ms per frame | share of 16.7 ms |
|---|---|---|
| road + tiles | 2.20 | 13.2% |
| **sprites** | **0.95** | **5.7%** |
| text | 0.30 | 1.8% |
| everything else (game logic, NTSC on other threads, GPU) | remainder | |

**The Zero 2 W holds 60 fps with 0% frames dropped.** In `-perftest` it reaches
81-82 fps, so there is roughly 35% headroom above 60 in this content.

## Contention is real, and quantified

Comparing four-thread against single-thread operation **at an identical sprite
pixel count of 80,272** (the same attract scene, so this is like-for-like work):

| phase | 1 thread | 4 threads + GPU | inflation |
|---|---|---|---|
| road + tiles | 1.43 ms | 2.17 ms | **+52%** |
| sprites | 0.81 ms | 1.05 ms | **+30%** |

So the hunch that memory or the shared L2 is a limiting factor has real support:
concurrency costs about a third of the sprite pass and half again on road and
tiles. It is a tax on top of the work, not the thing that breaks 60 fps in this
content. Both figures come from the pixel-counter build, which inflates the
sprite number slightly; the uninstrumented four-thread sprite figure is 0.95 ms.

## Calibration: the synthetic benchmark was about 4x too heavy

Real sprite load, counted directly as destination pixels advanced per frame:

| content | sprite pixels per frame |
|---|---|
| typical attract driving | 80,000 - 85,000 |
| busier attract scene (seen in `-perftest`) | 165,000 - 224,000 |

The isolated benchmark above used 310,955 pixels, so its absolute times are
pessimistic for typical content and roughly right for the busy end. Sprite load
varies by about 2.7x across scenes, which matters more than the average: the
worst case sets whether frames drop.

## What the shipped renderer actually achieves

0.95 ms for about 82,000 destination pixels at 1.0 GHz is **11.6 cycles per
destination pixel**, which is essentially the same rate as the REF model in the
isolated benchmark (11.61 cycles per pixel). The shipped renderer does more than
REF does, since it also handles clipping and shadow, so it is somewhat more
efficient than that equality suggests. But it is nowhere near the 5.69 cycles per
pixel the line-buffer design reaches.

## Revised expectation from a rewrite

| scene | sprite pixels | now, at 11.6 cyc/px | at 5.7 cyc/px | saved |
|---|---|---|---|---|
| typical | 82,000 | 0.95 ms | 0.47 ms | **0.48 ms** |
| busy | 224,000 | 2.60 ms | 1.28 ms | **1.32 ms** |

Against a 16.7 ms budget that is 2.9% of the frame in typical content and 7.9% in
the busiest content seen. Worth having, particularly for the busy scenes that
decide whether the frame rate holds, but it is not the dominant term.

## The bigger target is road and tiles

Road and tiles cost 2.20 ms against the sprite pass's 0.95 ms, and they suffer
more from contention (+52% against +30%). If the goal is headroom on the prepare
thread rather than the sprite renderer specifically, that is where the time is.
The same techniques may apply: both write the same 16-bit buffer, and the road
layer in particular fills every pixel of every row.
