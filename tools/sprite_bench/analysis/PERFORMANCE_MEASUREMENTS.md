# Sprite & road/tiles performance measurements

This document is the consolidated, chronological record of every hardware measurement
taken during the sprite-kernel rewrite (S0-S4, R0-R2) and the road/tiles optimisation
that it redirected effort towards. It merges (and replaces) `MEASURED_A53_PI3.md`,
`PHASE_PROFILE.md`, `MEASURED_INSITU_SPRITE.md`, `MEASURED_S1_INSITU.md`,
`MEASURED_S4_INSITU.md`, `MEASURED_R0_ATTRIBUTION.md` and `MEASURED_R1_INSITU.md`.
Those filenames, and any others named below, no longer exist in the tree: they are
referred to as provenance only, and are recoverable from git history.
Every table below is reproduced numerically complete; narrative and reproduction detail
has been condensed. For the design/architecture that these numbers justify, see
`SPRITE_R1_LLD.md` (sprites) and `ROAD_TILE_RENDERER.md` (road/tiles).

The story, in one paragraph: an idle synthetic bench on a Pi 3 A+ found a new sprite
kernel shape (LINEBUF) that halved cycles/pixel (campaign 1). The same day, a phase
profile of the real prepare-thread found road+tiles cost 1.7x the sprite pass, which
redirected the project's second half (campaign 1, part B). Once LINEBUF was actually
built and measured in-game, it regressed 11-33% instead of improving (campaign 3) —
the idle bench had lied about real conditions. Adding NEON vectorisation to the span
copy did not fix it (campaign 4). Root-cause analysis found the deficit tracked source
rows, not pixels — i.e. per-row call/branch/reload overhead ("glue"), not per-pixel
compute (campaign 5). Removing that glue (R1a) fixed the regression outright, and a
cheaper per-nibble fill (R1b) turned it into a net win, confirmed in-game (campaign 6).

---

## Campaign 1: sprite kernel store-strategy bench + first in-situ frame split (2026-09-04)

**Machine/method:** Raspberry Pi 3 Model A+ (`pi3mule`), BCM2837, 4x Cortex-A53 at
1.4 GHz nominal, GCC 14.2, `-O3 -mcpu=cortex-a53`, `taskset -c 3`, governor
`performance`; counters read directly via `perf_event_open` (cycles, instructions,
L1D/L2D refill, branch mispredicts, A53 store-stall), fastest of 11 reps. Workload: 26
synthetic blob sprites, 310,955 output pixels, into a 640x448 16-bit buffer, zooms
0x80-0x1F8 (magnifying, matching shipped hi-res config), transparency present, shadow/
row-terminator/edge-clip excluded so the store pattern alone is measured. All four real
strategies produce identical checksums in every condition (correctness gate).

Strategies compared: **REF** (upstream shape: per-pixel accumulator test + transparency
test + store, source row re-decoded per destination row — no clip test, so flattered
slightly), **CF1** (closed-form run lengths, still per-pixel store), **FANOUT** (per
source pixel, one wide store of MAXRUN copies fanned to all `r_v` destination rows —
the shape `DESIGN_REVIEW_STAGING.md` recommended), **LINEBUF** (decode the source row
once into an L1 scratch line buffer with overlapping wide stores, then copy each
opaque span to each destination row in 16-byte chunks — the shape `DESIGN_FABLE.md`
recommended), **FLOOR** (writes every destination pixel with a constant, no decode/
transparency — upper bound on store traffic, lower bound on achievable time).

### Full 560 KB destination, idle (clock verified 1395-1412 MHz)

| strategy | cyc/px | ms/frame | insts/px | L1D rf/px | L2 rf/px | brmis/px | vs REF |
|---|---|---|---|---|---|---|---|
| REF      | 11.70 | 2.608 | 13.94 | 0.0216 | 0.0144 | 0.1174 | 1.00x |
| CF1      | 11.75 | 2.614 | 14.30 | 0.0230 | 0.0204 | 0.1090 | 1.00x |
| FANOUT   |  7.36 | 1.634 |  8.54 | 0.0216 | 0.0116 | 0.0214 | **1.59x** |
| LINEBUF  |  5.81 | 1.289 |  6.42 | 0.0247 | 0.0091 | 0.0393 | **2.01x** |
| LINEBUF, 32-byte chunks | 5.79 | 1.286 | 6.25 | 0.0263 | 0.0084 | 0.0384 | 2.02x |
| FLOOR    |  0.82 | 0.181 |  0.71 | 0.0234 | 0.0043 | 0.0059 | 14.3x |

### Full 560 KB destination, with a memory thrasher on another core (cycle counts valid; wall-clock is not — board throttled, see caveat)

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

**Findings.** (1) LINEBUF beats FANOUT in every condition (5.81 vs 7.36 cyc/px idle,
7.02 vs 8.27 under contention) — `DESIGN_REVIEW_STAGING.md`'s memory-op-count model
was wrong to prefer FANOUT because it did not weight frame-buffer stores (which miss a
contested L2) against line-buffer traffic (which stays in L1): counting only
frame-buffer stores, LINEBUF issues ~0.125 store/px against FANOUT's ~0.44. (2) The
closed form alone buys nothing — CF1 matches REF to within 0.5%; the gain comes from
wide stores and decode-once, not from removing the accumulator. (3) Branch mispredicts
fall 3-6x (0.117 -> 0.021 FANOUT / 0.039 LINEBUF), confirming the reference's per-pixel
accumulator branch was a major cost. (4) The kernel is compute-bound, not memory-bound:
the store-only floor is 0.82 cyc/px idle / 2.16 under contention, well below LINEBUF's
5.81/7.02 — contradicting `DESIGN_FABLE.md`'s expectation that the memory floor would
be reached early. (5) Wider (32-byte) replay chunks are a wash (5.79 vs 5.81 cyc/px).
(6) Expected whole-frame effect at 310,955px/1.4GHz: sprite pass 2.61ms -> 1.29ms idle,
2.75ms -> 1.56ms under contention — roughly 1.2-1.3ms of a 16.7ms frame, *assuming*
the synthetic pixel count matches real content (it did not — see campaign 2).

**Caveats that matter.** The Pi 3 A+ board used for the idle bench was under-voltage
throttled (`vcgencmd get_throttled` = `0x50005`); the ARM core ran at 600MHz rather
than 1400MHz during actual gameplay, so all wall-clock figures taken under load (and
whole-game frame rates) on this board are unreliable by roughly 2x. The idle-bench
cycle counts themselves are frequency-independent and are not affected. All later
in-game measurements in this document were retaken on properly-powered boards
(`sl-d303`, `pi2-local`) for exactly this reason.

### Part B: in-situ measurement under real concurrent load (same date, `sl-d303`)

The synthetic thrasher above is one thread doing strided read-modify-write over 16MB;
real gameplay is heavier and differently shaped (game logic, two NTSC/RGB conversion
threads, audio, and GPU compositing, all sharing one 512KB L2 and one memory bus), so
the sprite pass was timed inside the running game. Machine: `sl-d303`, Pi Zero 2 W,
4x Cortex-A53 at 1.0GHz, governor `performance`, `throttled=0x0` throughout (properly
powered, unlike the Pi 3 A+ above). Hi-res 640x448, Blargg NTSC filter on, widescreen
off, attract mode, headless KMSDRM. Instrumentation: three clock reads per frame around
the layer groups in `Video::prepare_frame()`.

**Frame budget at 60fps:**

| phase | ms per frame | share of 16.7ms |
|---|---|---|
| road + tiles | 2.20 | 13.2% |
| **sprites** | **0.95** | **5.7%** |
| text | 0.30 | 1.8% |
| everything else (game logic, NTSC threads, GPU) | remainder | |

The Zero 2 W holds 60fps with 0% frames dropped (81-82fps in `-perftest`, ~35% headroom).

**Contention, at an identical sprite pixel count (80,272px, like-for-like work):**

| phase | 1 thread | 4 threads + GPU | inflation |
|---|---|---|---|
| road + tiles | 1.43 ms | 2.17 ms | **+52%** |
| sprites | 0.81 ms | 1.05 ms | **+30%** |

**Calibration — the synthetic bench above was about 4x too heavy for typical content:**

| content | sprite pixels per frame |
|---|---|
| typical attract driving | 80,000 - 85,000 |
| busier attract scene (`-perftest`) | 165,000 - 224,000 |

The shipped renderer's real rate: 0.95ms for ~82,000px at 1.0GHz = **11.6 cyc/px**,
essentially matching the isolated bench's REF number (11.61 cyc/px) despite doing more
work (clipping, shadow) — nowhere near LINEBUF's 5.7-5.8 cyc/px.

**Revised rewrite expectation:**

| scene | sprite pixels | now, at 11.6 cyc/px | at 5.7 cyc/px | saved |
|---|---|---|---|---|
| typical | 82,000 | 0.95 ms | 0.47 ms | **0.48 ms** |
| busy | 224,000 | 2.60 ms | 1.28 ms | **1.32 ms** |

Against a 16.7ms budget that's 2.9% (typical) to 7.9% (busiest seen at the time) of the
frame — worth having, but **road and tiles, at 2.20ms against the sprite pass's
0.95ms, and suffering more from contention (+52% vs +30%), are the bigger target.**
This finding is what led directly to campaign 1's sibling study below and to the whole
road/tiles optimisation effort recorded in `ROAD_TILE_RENDERER.md`.

---

## Campaign 2: prepare-thread phase profile (2026-09-04, `sl-d303`)

**Machine/method:** `sl-d303`, Pi Zero 2 W, 4x Cortex-A53 @ 1.0GHz, governor
`performance`, `throttled=0x0` before/after every run. Hi-res 640x448, widescreen off,
Blargg NTSC + full CRT shader, audio on, attract mode, headless KMSDRM. Eight clock
reads per frame around the individual calls in `Video::prepare_frame()`, splitting the
2.20ms "road+tiles" figure from campaign 1 into its six constituent calls.

**Split at 60fps, four threads (0% frames dropped):**

| call | ms/frame | share of prepare | share of 16.7ms |
|---|---|---|---|
| `update_tile_values` | 0.001 | 0.0% | 0.0% |
| road background | 0.68 | 20% | 4.1% |
| tile background | 0.43 | 13% | 2.6% |
| tile foreground | 0.20 | 6% | 1.2% |
| **road foreground** | **0.88** | **26%** | **5.3%** |
| sprites | 0.93 | 27% | 5.6% |
| text | 0.31 | 9% | 1.9% |
| **total** | **3.44** | | **20.6%** |

(Road+tiles sums to 2.19ms here, matching the 2.20ms bundled figure from campaign 1 —
cross-check passes.)

**Road vs tiles:**

| layer group | ms/frame | share of the 2.19ms |
|---|---|---|
| **road (background + foreground)** | **1.56** | **71%** |
| tiles (background + foreground + values) | 0.63 | 29% |

Road alone costs 1.7x the sprite pass; tiles (despite the static analysis flagging its
four-separate-16-bit-stores-per-nibble hi-res pattern) total only 0.63ms.

**Contention (four threads vs single thread, both at `-60`):**

| call | 1 thread | 4 threads + GPU | inflation |
|---|---|---|---|
| **road background** | **0.265** | **0.682** | **+157%** |
| tile background | 0.34 | 0.43 | +27% |
| tile foreground | 0.182 | 0.203 | +11% |
| road foreground | 0.70 | 0.88 | +26% |
| sprites | 0.72 | 0.93 | +29% |

Road background — a pure `std::fill_n` streaming write with no per-pixel decisions —
inflates 2.6x under contention, far more than every compute-bound phase (11-29%): the
clearest evidence that shared L2/bus bandwidth, not instruction issue, is the limiting
factor for that specific phase. (Caveat: at `-t 1` the game runs at 26fps with 50% of
frames dropped, so it sits at a different point in the deterministic attract sequence;
the other four phases moved only 11-29% across the same pair of runs, which argues
against a pure content explanation for road background's 157% move, but a same-scene
comparison would settle it conclusively.) Under `-perftest` (81fps, flat out): road
foreground rises to 1.33-1.42ms, tile background to 0.64-0.82ms.

**Ranked recoverable time (estimate at the time, informing the plan that became
`ROAD_TILE_RENDERER.md`):**

| target | cost now | plausible saving | basis |
|---|---|---|---|
| road background overdraw | 0.68 ms | up to ~0.5 ms | eliminate writes later overwritten |
| sprite kernel rewrite | 0.93 ms | ~0.47 ms | measured 11.6 to 5.7 cycles/pixel |
| road foreground | 0.88 ms | ~0.15 ms | already near its memory-op floor |
| tile layers | 0.63 ms | ~0.3 ms | wider stores, fewer branches |

This table, and the road-background-is-overdraw / road-foreground-is-near-its-floor
findings, are the pivot point that directed the second half of the project at
road/tiles rather than further sprite work; see `ROAD_TILE_RENDERER.md` for what was
built from it.

---

## Campaign 3: pre-rewrite memory-behaviour study, M-A/M-B/M-C (2026-09-05, `sl-d303`)

**Machine/method:** `sl-d303`, Pi Zero 2 W, 4x Cortex-A53 @ 1.0GHz, governor
`performance`, `throttled=0x0` verified before every run. Hi-res, widescreen off,
Blargg filter, full CRT shader, audio on, attract mode, `-60 -t 4`. Run **before any
rewritten renderer code was written**, against the shipped (pre-LINEBUF) kernel, to fix
the rewrite's target and the S4-vs-S5 stage order. Two instrumented builds: **Build 1**
adds a global output-pixel counter plus a `perf_event_open` group (cycles, L1D/L2D
refill, A53 store-stall) around `sprite_layer->render()`; **Build 2** additionally
redirects all 13 destination-pointer initialisations to a 20KB L1-resident scratch
buffer (row index `& 15`), isolating the destination-memory-miss term (output is wrong
by design; no checksum gate active in this build). Two independent 300s captures (one
per build) plus a 75s `-perftest` capture, grouped into three pixel-count bands.

| band | px/frame | rationale |
|---|---|---|
| typical | 60,000 - 100,000 | matches campaign 1's 80,000-85,000 |
| moderate-busy | 150,000 - 300,000 | matches the previously assumed 165,000-224,000 busy ceiling |
| extreme | > 300,000, up to 704,796 (attract) / 973,716 (`-perftest`) | busier than previously assumed |

### M-A: exposed destination-memory term

| band | px/frame (avg) | Normal ms/frame | M-A ms/frame | **Δ ms/frame** | Normal wall cyc/px | M-A wall cyc/px | **Δ wall cyc/px** | Normal PMU cyc/px | M-A PMU cyc/px | Δ PMU cyc/px |
|---|---|---|---|---|---|---|---|---|---|---|
| typical | ~80,700 | 1.192 | 1.094 | **0.098** | 14.76 | 13.55 | **1.21** | 11.82 | 10.65 | 1.17 |
| moderate | ~213,000 | 2.355 | 2.193 | **0.163** | 11.01 | 10.36 | **0.65** | 9.13 | 8.47 | 0.66 |
| extreme | ~487,600 | 4.850 | 4.478 | **0.372** | 9.93 | 9.20 | **0.73** | 8.58 | 8.05 | 0.53 |
| whole run | ~293,000 | 3.058 | 2.842 | 0.216 | 10.46 | 9.69 | 0.77 | 8.87 | 8.27 | 0.60 |

Verdict thresholds used: <0.15ms/<1.8 cyc/px = compute-dominated; 0.15-0.30ms = memory
matters, do S4 before S5; >0.30ms = memory is 40%+, build the D/D' switch into stage 1.
**Typical load is compute-dominated** (0.098ms/1.2 cyc/px), matching the design's
assumption. **Extreme load (>300,000px, confirmed real and sustained, not assumed)
crosses the top threshold at 0.372ms/frame** even though cyc/px (0.53-0.73) reads as
still compute-dominated — the two metrics disagree because cyc/px's denominator grows
faster than its numerator as scenes get busier (see M-C). The ms/frame reading governs,
since that's what the 16.7ms budget is denominated in.

### M-B: load-side vs store-side L2 traffic

| band | Normal L2Dref/frame | M-A residual L2Dref/frame | destination-attributable | dest share | load share | Normal sstall (% of cyc) | M-A sstall (% of cyc) |
|---|---|---|---|---|---|---|---|
| typical | 2,702 | 1,073 | 1,629 | 60% | 40% | 4.1% | 0.5% |
| moderate | 4,531 | 1,600 | 2,931 | 65% | 35% | 2.1% | 0.4% |
| extreme | 10,464 | 2,109 | 8,355 | 80% | 20% | 1.4% | 0.3% |

Reference point: an independent 100-frame capture (`ROAD_TILE_RENDERER.md`'s data
study) measured 2,147 destination lines/frame for the road pass, none first-touch —
Build 1's typical-band 2,702 L2Dref/frame is 26% above that, consistent with almost
every destination line missing L2 plus a modest ROM/table overhead. **Verdict: reads
as destination-bound, more so as load rises** (60% typical -> 80% extreme), with a
real, roughly-constant (~1,000-2,100 lines/frame) load-side (ROM + shadow-info table)
contribution that does not disappear at any scene size.

### M-C: per-pixel rate vs load

| method | typical wall cyc/px | moderate wall cyc/px | extreme wall cyc/px | typical->extreme |
|---|---|---|---|---|
| paced attract, Build 1, `-60 -t 4` | 14.76 | 11.01 | 9.93 | **-33%** |
| uncapped, Build 1, `-perftest -t 4` | 16.01 | 11.39 | 9.50 | **-41%** |

Cyc/px *falls* as scene pixel count rises (opposite of the naive expectation) because
L2 refills/frame grow far slower than px/frame (roughly px^0.55) — busy attract scenes
are dominated by fewer, larger, more-zoomed sprites, which reuse each 64-byte
destination line across more output pixels and amortise the fixed per-source-pixel
decode/branch cost over more pixels. Incidental finding: real attract-mode content
sustains far busier scenes than previously assumed — both 300s captures spent over two
minutes above 300,000px/frame, peaking at 704,796 (attract) / 973,716 (`-perftest`),
3-4x campaign 1's 224,000 "busy" ceiling; frame drops of 1-10% appear specifically in
these windows (0% elsewhere), confirming real load rather than noise.

### What this meant for the rewrite

- Typical-scene behaviour is compute-dominated (target stands as designed); the
  busiest real scenes (300,000-975,000px/frame) carry a memory term growing to
  0.37ms/frame — the busy-scene target should expect this as a real, load-scaling
  cost, not a fixed small addition.
- At typical load, S5 (compute) stages remain the bigger lever; at the load levels
  that actually cause drops (extreme band), S4 (prefetch/replay memory work) should
  not be deferred behind all of S5.
- **Build the D/D' switch into stage 1** (decided at S1 under the checksum gate, not
  deferred to a later polish pass) — M-A crosses the "memory is 40%+" threshold at
  extreme load and M-B shows the destination side is the majority (60-80%) of L2
  traffic.
- Drop the `sprites_shadowinfo`/`sprites_flipped` side tables and add ROM row
  prefetch at stage 1 — M-B's measured load-side floor (1,073-2,109 lines/frame,
  roughly flat across scene size) reinforces dropping these tables, and this is now
  a measured, present-every-frame cost rather than a modelled one.

---

## Campaign 4: S1 (LINEBUF) in-situ regression (2026-09-05, `sl-d303`)

**Machine/method:** `sl-d303`, Pi Zero 2 W, 4x Cortex-A53 @ 1.0GHz, governor
`performance`, `throttled=0x0` verified before/after every run. Clean `git archive`
exports of commit `bdf7e9b` (shipped) and `24b8aac` (S1/LINEBUF), each instrumented
with a per-pixel counter and cyc/px reporting, run as three independent 240s `-60 -t 4`
attract captures (shipped; S1 `-spritereplay 0`=D; S1 `-spritereplay 1`=D'), banded by
the same three px/frame bands as campaign 3.

| band | shipped cyc/px | S1-D cyc/px | S1-D' cyc/px | S1-D / shipped | S1-D' / shipped |
|---|---|---|---|---|---|
| typical (60-100k) | 21.256 | 28.236 | 27.873 | **1.328x** | **1.311x** |
| moderate (150-300k) | 17.561 | 20.722 | 21.202 | **1.180x** | **1.207x** |
| extreme (>300k) | 13.712 | 15.241 | 16.182 | **1.111x** | **1.180x** |
| whole run (pooled, NOT scene-matched) | 16.267 | 20.125 | 20.635 | 1.237x | 1.269x |

Matched ms/frame, for reference:

| band | avg px/frame (ship / S1-D / S1-D') | shipped ms | S1-D ms | S1-D' ms |
|---|---|---|---|---|
| typical | 76,810 / 76,350 / 76,445 | 1.633 | 2.156 | 2.131 |
| moderate | 210,278 / 229,626 / 226,051 | 3.693 | 4.758 | 4.793 |
| extreme | 370,625 / 358,373 / 356,578 | 5.082 | 5.462 | 5.770 |

`fallback/frame` (the out-of-envelope `render_px` oracle) was 0.00000 in every band,
both replay modes — none of S1's measured cost is oracle fallback.

**Verdict: S1 is slower than shipped per pixel, scene-matched, in every band tested,
under both replay strategies — 11-33% more cycles per destination pixel**, worst at
typical (light) load, narrowing but not disappearing as scenes get busier. This
survives the scene-matching that the measurement was built to rule out, so it is a
real per-pixel regression, not a restatement of "S1 happened to draw a busier scene."
This is the finding that triggered the whole S4 -> R0 -> R1 diagnostic chain below.
**D beats D' overall** (span-copy memcpy vs load-chunk-once) at moderate/extreme load
(20.722 vs 21.202; 15.241 vs 16.182), about equal at typical load — recommendation was
to ship D (`replay_mode 0`) as the default, since D's edge grows, not shrinks, as
scenes get busier (the regime that matters most for frame budget).

---

## Campaign 5: S4 (NEON) in-situ — does vectorising the copy fix it? (2026-09-05, `pi2-local`)

**Machine/method:** `pi2-local`, Raspberry Pi 2 Rev 1.2 (BCM2837, 4x Cortex-A53,
aarch64, NEON active), ARM clock fixed 900MHz, governor `performance`,
`throttled=0x0` before/after every run, peak 66.1°C (chosen specifically to test
whether campaign 4's regression was a thermal artifact of the heatsink-less
`sl-d303`). `prof_ns_sprites` already exists per-frame in `video.cpp`, so only a
throwaway windowed-cumulative-time harness was added, identical across three clean
`git archive` trees (shipped `bdf7e9b`; S1 scalar `24b8aac`; S4 NEON span-replay
`73dc0c1`), four 9000-frame `-60 -t 4` attract captures.

Cumulative sprite time over the identical 9000-frame sequence:

| build | cum sprite-ns | vs shipped |
|---|---|---|
| shipped (`bdf7e9b`) | 16,682,637,760 | 1.000 |
| S1 scalar (`24b8aac`) | 22,116,424,398 | **1.326** |
| S4 NEON D (`73dc0c1`) | 20,533,907,017 | **1.231** |
| S4 NEON D' (`73dc0c1`) | 20,099,350,813 | **1.205** |

Load-resolved (terciles by shipped per-frame sprite cost):

| band | windows | shipped ns/frame | S1/ship | S4-D/ship | S4-D'/ship |
|---|---|---|---|---|---|
| light | 10 | 777,616 | 1.385 | 1.248 | 1.241 |
| moderate | 10 | 1,408,879 | 1.373 | 1.282 | 1.252 |
| heavy | 10 | 3,374,385 | 1.292 | 1.206 | 1.177 |
| ALL | 30 | 1,853,626 | 1.326 | 1.231 | 1.205 |

**Verdict A — thermal test: the S1 regression is real, not a thermal artifact.**
S1/shipped is 1.29-1.39x on `pi2-local` at 64-66°C with `throttled=0x0` throughout,
reproducing (slightly exceeding) the `sl-d303` finding — not caused by a missing
heatsink; inherent to the LINEBUF rewrite.

**Verdict B — S4-NEON does not clear the shipped renderer.** S4-D is a +21-28%
per-frame regression across every band (+23% overall); S4-D' is marginally better
(+18-25%) but still a clear regression. NEON recovered roughly 10 of S1's ~33
percentage points of deficit, not the rest.

**Root cause hint (consistent with the shape).** The regression is worst at light
load and narrows (but never closes) as scenes get busier — the signature of a fixed
per-frame/per-sprite overhead that per-pixel copy work only amortises when there are
many pixels. This pointed at the per-frame decode/classify recompute the rewrite does
in place of the shipped renderer's cached side tables, and directly predicted
campaign 6's finding. (Note: D' edges D here (1.205 vs 1.231), opposite to `sl-d303`
where D won — immaterial to the decision since both regress, but shows the D/D' choice
is not robust across A53 SKUs.)

---

## Campaign 6: R0 attribution — where the S4 deficit actually goes (2026-09-05, `sl-d303`)

**Machine/method:** `sl-d303`, Pi Zero 2 W, 4x Cortex-A53, governor `performance`
verified pinned immediately before the run set (all four cores fixed at 1,000,000kHz).
GCC 14.2.0, `-j1` sequential builds (512MB RAM). Two throwaway instrumentation layers:
a windowed harness (`src/main/main.cpp`) logging cumulative `prof_ns_sprites` every 300
frames; and, S4-tree-only, `cntvct_el0`-based phase timers (`CB_R0_PROF`) bracketing
every entry/exit of the five functions doing a sprite's work (`draw_sprite_fast`,
`decode_row`, `replay_colour_span`, `replay_shadow_span`), giving DECODE/COPY/OTHER
buckets per call (`OTHER = total - decode - copy`: call overhead, bounds/zoom
arithmetic, ROM/shadow prefetch scans, `glow_flush`).

### 6.1 — R0a: static analysis (objdump, S4 binary)

Confirmed by disassembly, functions bounds-checked against the full symbol table:

1. **`bl`/`blr` calls inside the hot paths — YES.** `decode_row` calls a shared
   `sched()`/`wsum()` helper twice per row-entry (not per nibble). `draw_sprite_fast`
   calls `decode_row` and `replay` via real `bl` — **the compiler did not inline
   either**; the source's single `while` loop is unrolled 3x, so this is 3 real call
   pairs per sprite. This is the fact `hwarch.hpp`'s `CB_ALWAYS_INLINE` comment (sec.
   2.1 of `SPRITE_R1_LLD.md`) cites: at both `-O2` (standalone driver) and
   `-O3 -Ofast` (the game), GCC declines to inline `decode_row`/`replay` on its own —
   confirmed here at the instruction level, which is why R1's `decode_row_k`/
   `replay_k` kernels are marked `CB_ALWAYS_INLINE` rather than left to compiler
   discretion.
2. **`sdiv`/`udiv` — NONE.** Zero occurrences in any of the four hot functions — the
   division-free closed-form design is confirmed at the instruction level.
3. **`mul`/`madd`/`smull` inside the per-nibble decode loop — YES.** 9 occurrences of
   `mul x_, x_, x13` inside `decode_row`'s unrolled per-nibble body, each computing
   `fill = (uint64_t)v * 0x0001000100010001ULL` fresh per nibble immediately before a
   store.
4. **Loads from a record-relative base inside the unrolled body — YES.** `r.color`
   (offset `#52`, invariant for the whole row) is reloaded from memory on successive
   nibble iterations instead of being kept live in a register.
5. **`bl memcpy` — YES, exactly once**, in `replay_colour_span`'s scalar D-path, once
   per destination row for a variable-length span (the decode-side per-nibble 8-byte
   memcpy was inlined to a plain store, so no `bl` there). `replay_shadow_span` has
   zero `bl`/`blr` and zero `memcpy` — fully inlined, call-free.

### 6.2 — Runs and phase-split results

Three 9000-frame `-60 -t 4` attract captures (shipped; S4 `-spritereplay 0`=s4glow;
S4 `-spritereplay 0 -noglow`=s4noglow), `throttled=0x0` at every checkpoint, peak
73.1°C (no thermal throttling), governor pinned+re-verified `performance`/1.0GHz at
the start of every run.

Cumulative sprite time, identical 9000-frame sequence:

| build | cum sprite-ns | vs shipped |
|---|---|---|
| shipped | 13,901,970,479 | 1.000 |
| S4 glow-ON (`-spritereplay 0`) | 19,375,577,155 | 1.394 |
| S4 glow-OFF (`-noglow`) | 18,765,547,594 | 1.350 |

Per-band summary (terciles by shipped ns/frame, 30 usable 300-frame windows):

| band | n windows | shipped ns/frame | S4-glow ns/frame | deficit ns/frame | ratio | glow cost ns/frame |
|---|---|---|---|---|---|---|
| light | 10 | 624,982 | 912,473 | 287,491 | 1.460 | 34,275 |
| moderate | 10 | 1,169,892 | 1,690,136 | 520,244 | 1.445 | 47,418 |
| heavy | 10 | 2,839,117 | 3,855,917 | 1,016,800 | 1.358 | 121,650 |

S4 phase split (fraction of `draw_sprite_fast`'s total cntvct ticks):

| band | decode/total | copy/total | other/total (glue) |
|---|---|---|---|
| light | 0.5537 | 0.1535 | 0.2928 |
| moderate | 0.6002 | 0.1183 | 0.2816 |
| heavy | 0.6000 | 0.1592 | 0.2408 |

S4 per-unit costs:

| band | decode ticks/srcrow | copy ticks/px | srcrows/frame | px/frame | sprites/frame |
|---|---|---|---|---|---|
| light | 28.44 | 0.0713 | 334.8 | 37,000 | 22.40 |
| moderate | 25.90 | 0.0722 | 740.8 | 52,372 | 41.98 |
| heavy | 37.05 | 0.0599 | 1,189.9 | 195,359 | 56.03 |

**Deficit-per-srcrow vs deficit-per-px, across bands** — the row-vs-pixel tracking test:

| band | deficit ns/frame | srcrows/frame | px/frame | deficit/srcrow (ns) | deficit/px (ns) |
|---|---|---|---|---|---|
| light | 287,491 | 334.8 | 37,000 | 858.63 | 7.7699 |
| moderate | 520,244 | 740.8 | 52,372 | 702.23 | 9.9336 |
| heavy | 1,016,800 | 1,189.9 | 195,359 | 854.56 | 5.2048 |

Coefficient of variation across bands: **deficit/srcrow = 0.0904**; **deficit/px =
0.2531** (2.8x higher). Lower CoV = more constant = what the deficit actually tracks.

**The two answers.** (1) *Phase split:* DECODE is the largest bucket (55.4-60.0% of
`draw_sprite_fast`'s time), not OTHER (24.1-29.3%); COPY (per-pixel span replay) is
smallest (11.8-15.9%) in every band — 84.1-88.2% of the time is DECODE+OTHER, neither
of which scales with output pixel count. (2) *What the deficit tracks:* deficit-per-
source-row is nearly constant across bands (CoV 0.090) while deficit-per-pixel varies
2.8x more (CoV 0.253, not even monotonic with load). **The S4-vs-shipped deficit
tracks source rows per frame, not pixels per frame** — i.e. it is fixed per-row glue
overhead (uninlined calls, per-nibble reloads, redundant schedule lookups, per-word
bounds), not vectorisable per-pixel work, which is exactly why R1 (de-glue) was the
right next move rather than more vectorisation.

---

## Campaign 7: R1 in-situ validation — did de-glue fix it? (2026-09-06, `pi2-local`)

**Machine/method:** `pi2-local`, Pi 2 Rev 1.2, A53 aarch64, 900MHz fixed, governor
`performance`, `throttled=0x0` before/after every run, peak 61.8°C. Same box, method
and 9000-frame deterministic-attract protocol as campaign 5, so directly comparable to
the S4 baseline measured there. Three clean `git archive` trees, `-j2`, CMake Release
defaults (`-O3 -Ofast -mcpu=cortex-a53+crc`): shipped `bdf7e9b`; R1a (de-glue)
`2f5e3eb`; R1b (templated fill + copy ladder) `59a5b6a`. Shipped reproduces campaign
5's shipped total (16.62G vs 16.68G ns over the same 9000 frames) — no box drift.

Cumulative sprite time, identical 9000-frame sequence:

| build | cum sprite-ns | vs shipped | vs S4 (20.53G) |
|---|---|---|---|
| shipped | 16,619,525,072 | 1.000 | 0.809 |
| R1a | 15,353,901,121 | **0.924** | 0.748 |
| R1b | 15,039,977,892 | **0.905** | 0.733 |

Load-resolved (30 windows, terciles by shipped per-window cost):

| band | shipped ms/frame | R1a/ship | R1b/ship | R1b ms/frame |
|---|---|---|---|---|
| light | 0.767 | 0.992 | 1.000 | 0.767 |
| moderate | 1.407 | 0.920 | 0.913 | 1.284 |
| heavy | 3.366 | 0.910 | 0.880 | 2.963 |
| ALL | 1.847 | 0.924 | 0.905 | 1.671 |

**Verdict: the S4 regression is eliminated and reversed.** R1b beats shipped overall
(0.905x) and in the moderate (0.913x) and heavy (0.880x) bands, dead-even in light
(1.000x) — while also drawing glow and carrying the F1/F2 correctness fixes shipped
does not have. Versus the S4 rewrite that's a 27% reduction in sprite time (20.53G ->
15.04G ns).

**Attribution held.** R1a (de-glue alone, fill unchanged) already flipped the
regression to 0.924x — confirming campaign 6's finding that the cost was row-
proportional glue, not per-pixel work. R1b's shift-OR fill + copy ladder added a
further ~2 points overall and the most in the heavy band (0.910 -> 0.880), where
per-pixel fill/copy work dominates — exactly where a cheaper fill should help most.

**Gate.** The target was <=0.90x in every band. R1b meets it in heavy (0.880), is
close in moderate (0.913), and only reaches parity in light (1.000) — light is the
known-weak case (the deficit was always worst at light load per campaign 6; a
direct-emit path for single-row sprites was the documented, conditional remedy,
see `SPRITE_REPLAN_FABLE.md`). Practically this does not matter much: light scenes
cost 0.77ms/frame with ample headroom; the win that matters for holding 60fps is the
0.40ms/frame saved in the heavy band. R1b is a strict improvement over shipped —
never slower, faster where it counts, plus glow + F1/F2. Deploy candidate: `59a5b6a`
(R1b).

---

## Consolidated summary table

A compact cross-campaign view (the numbers above are the source of record; this is
provided as a single quick-reference).

### Sprite kernel store-strategy bench (campaign 1, Pi 3 A+, idle, full 560KB dest)

| Strategy | cyc/px | vs REF |
|---|---|---|
| REF (upstream shape) | 11.70 | 1.00x |
| CF1 (closed-form, still per-pixel store) | 11.75 | 1.00x |
| FANOUT (per-source-pixel fan-out to r_v rows) | 7.36 | 1.59x |
| **LINEBUF (decode once, replay)** | **5.81** | **2.01x** |
| FLOOR (store-only, no decode) | 0.82 | 14.3x |

### Prepare-thread frame budget at 60fps (campaign 2, Zero 2 W, 4 threads)

| Phase | ms/frame | Share of 16.7ms |
|---|---|---|
| Road background | 0.68 | 4.1% |
| Tile background | 0.43 | 2.6% |
| Tile foreground | 0.20 | 1.2% |
| Road foreground | 0.88 | 5.3% |
| Sprites | 0.93 | 5.6% |
| Text | 0.31 | 1.9% |
| **Total** | **3.44** | **20.6%** |

### Road/tiles optimisation result (measured before/after; see `ROAD_TILE_RENDERER.md`)

| Phase | Baseline | After S1/S3/S5/S6/S7 |
|---|---|---|
| Road background | 0.68 | ~0.66 (untouched — bandwidth floor, DC ZVA killed) |
| Tile background | 0.43 | ~0.28 |
| Tile foreground | 0.20 | ~0.075 |
| Road foreground | 0.88 | ~0.26 |
| Text | 0.31 | ~0.08 |
| **Sum** | **~2.50** | **~1.35** |

### Sprite kernel in-situ regression-and-recovery arc (campaigns 4/5/7)

| Build | vs shipped (light) | vs shipped (moderate) | vs shipped (heavy) |
|---|---|---|---|
| S1 (LINEBUF, scalar) | 1.385x (regressed) | 1.373x | 1.292x |
| S4 (LINEBUF + NEON span) | 1.248x (still regressed) | 1.282x | 1.206x |
| **R1a (de-glue)** | 0.992x | 0.920x | 0.910x |
| **R1b (templated fill + copy ladder)** | 1.000x | 0.913x | **0.880x** |

### Sprite kernel exposed memory term by scene load (campaign 3, M-A)

| Band | px/frame | Exposed memory term (ms/frame) |
|---|---|---|
| Typical | ~80,700 | 0.098 (compute-dominated) |
| Moderate | ~213,000 | 0.163 |
| Extreme (real, sustained, up to 975k px/frame) | ~487,600 | 0.372 (memory is ~40%+) |

### R0 attribution of the S4 regression (campaign 6)

| Phase of `draw_sprite_fast` | Share of time (light/moderate/heavy bands) |
|---|---|
| DECODE (per-nibble work) | 55.4% / 60.0% / 60.0% |
| OTHER (glue: calls, `sched()`, bounds checks) | 29.3% / 28.2% / 24.1% |
| COPY (true per-pixel replay) | 15.4% / 11.8% / 15.9% |

Finding: the S4-vs-shipped deficit tracked source rows per frame (CoV 0.09), not
pixels per frame (CoV 0.25) — i.e. it was fixed per-row overhead, not vectorisable
per-pixel work, which is why R1 (de-glue) fixed what further vectorisation would not.
