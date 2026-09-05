# Measured in-situ: the shipped sprite renderer's memory behaviour (M-A/M-B/M-C)

Date 2026-09-05. Machine `sl-d303` (Raspberry Pi Zero 2 W, 4x Cortex-A53 at
1.0 GHz, governor `performance`, `vcgencmd get_throttled` = 0x0 verified before
every run). Game tree: `~/cbgame` on `sl-d303`, a plain rsync'd copy (no git),
matching the shipped `src/main/hwvideo/hwsprites.cpp` on `master`/`sprite-a53`
(the sprite renderer is unchanged on `sprite-a53`). Hi-res 640x448, widescreen
off, Blargg NTSC filter on, full CRT shader, audio on, attract mode, `-60 -t 4`
unless noted. This runs the three measurements `DESIGN_REVIEW_BANDWIDTH.md`
section 8 specifies, to fix the rewrite's target and the S4/S5 order before any
renderer code is written.

**All instrumentation described below is throwaway, lives only in `~/cbgame` on
`sl-d303`, and is not part of this commit.** Nothing in `src/main/hwvideo/`
changed on the Windows tree. Original files are backed up on the box as
`*.orig_backup` / `*.pre_mA_backup` alongside the edited copies.

## Method

Two instrumented builds, built and run entirely on `sl-d303`:

**Build 1 (serves M-B and M-C).** The shipped renderer's decode logic is
untouched. Added: (a) a global pixel counter `g_spr_px`, incremented next to
every `pPix1++`/`pPix2++`/`pPix3++` in all twelve `draw_pixel_*` macros in
`hwsprites.cpp` (by 1, 2 or 3 per macro) — one increment per destination-row
pixel position the DDA loop visits, whether or not it turns out opaque, which
is the "output pixel" definition used throughout this directory (`DESIGN_FABLE.md`,
`FABLE_ASSESSMENT.md`, `MEASURED_A53_PI3.md`); (b) a `perf_event_open` counter
group opened once, lazily, on whichever thread first calls
`Video::prepare_frame()` (the prepare thread under `-t 4`, pid=0 so only that
thread's active cycles are counted): `PERF_COUNT_HW_CPU_CYCLES` (leader, group
read via `PERF_FORMAT_GROUP`), raw `0x03` (L1D refill), raw `0x17` (L2D
refill), raw `0xE8` (A53 store-stall), read once immediately before and once
immediately after the `sprite_layer->render(pixels, 8)` call in `video.cpp` and
accumulated frame over frame; (c) the existing 2-second FPS report in
`main.cpp` extended with mean px/frame, mean cyc/px (`prof_ns_sprites` delta in
ns == cycles at 1.0 GHz), and the four PMU counters, all as deltas since the
last report divided by frames in the window — same convention the existing
`prof_ns_*` fields already use. `md5sum` of this binary:
`fe67bb256f3bd0ca0fac2a406091625e` (saved as `build/cannonball-se_MBMC`).

Confirmed before wiring into the game: a standalone test program opened the
same four-counter group as the unprivileged `sysadmin` user
(`perf_event_paranoid=2` on this kernel, which still allows user-space-only
counting) and read back plausible values from a tight loop.

**Build 2 (serves M-A).** Build 1 plus one further change: all 13 destination
pointer initialisations in `hwsprites.cpp` —
`pixels + ((y+k) * scrn_width) + xpos` for k=0,1,2, the one `pPix1` site and
the eight `pPix2` / four `pPix3` sites inside the `switch(jump_key)` cases —
redirected to a function-local static scratch buffer of `scrn_width * 16`
`uint16_t` (20,480 bytes at this build's 640px width, comfortably inside the
A53's 32 KB L1D), with the row index folded `& 15`. Same stride, same x, same
store count and pattern (including the shadow read-modify-write); only the
destination address's memory-miss behaviour changes. Output is wrong by
design; `CB_FRAME_CKSUM` is not defined in this build (confirmed absent from
`CMakeCache.txt`/`flags.make`), so no checksum gate needed disabling. `md5sum`:
`a56b6297f45dd3ed6e7944328b1ec22c`.

**Runs.** Two independent 300-second `-60 -t 4` attract-mode captures, one per
build, plus a 75-second `-perftest -t 4` capture with Build 1. The two 300 s
captures reproduce the same deterministic attract-mode sequence — the first
2-second window is 648,592 px (Build 1) vs 649,069 px (Build 2), and the whole
run's *median* px/frame is 251,388 in both — so the two builds are directly
comparable window-for-window without needing wall-clock alignment. Every FPS
report line was parsed and grouped into three pixel-count bands that both runs
actually contain:

| band | px/frame | rationale |
|---|---|---|
| typical | 60,000 - 100,000 | matches `MEASURED_A53_PI3.md`'s 80,000-85,000 |
| moderate-busy | 150,000 - 300,000 | matches the previously assumed 165,000-224,000 busy ceiling |
| extreme | > 300,000, up to 704,796 (attract) / 973,716 (`-perftest`) | a busier regime than previously assumed — see M-C |

## M-A: exposed destination-memory term

Sample sizes: typical 14 windows / 1,681-1,686 frames per build; moderate 28-29
windows / ~3,400 frames; extreme 67-68 windows / ~8,000-8,100 frames per build.

| band | px/frame (avg) | Normal ms/frame | M-A ms/frame | **Δ ms/frame** | Normal wall cyc/px | M-A wall cyc/px | **Δ wall cyc/px** | Normal PMU cyc/px | M-A PMU cyc/px | Δ PMU cyc/px |
|---|---|---|---|---|---|---|---|---|---|---|
| typical | ~80,700 | 1.192 | 1.094 | **0.098** | 14.76 | 13.55 | **1.21** | 11.82 | 10.65 | 1.17 |
| moderate | ~213,000 | 2.355 | 2.193 | **0.163** | 11.01 | 10.36 | **0.65** | 9.13 | 8.47 | 0.66 |
| extreme | ~487,600 | 4.850 | 4.478 | **0.372** | 9.93 | 9.20 | **0.73** | 8.58 | 8.05 | 0.53 |
| whole run | ~293,000 | 3.058 | 2.842 | 0.216 | 10.46 | 9.69 | 0.77 | 8.87 | 8.27 | 0.60 |

"Wall cyc/px" is `prof_ns_sprites` delta / px delta (ns == cycles at 1.0 GHz),
the same convention `MEASURED_A53_PI3.md` uses. "PMU cyc/px" is the raw
`PERF_COUNT_HW_CPU_CYCLES` delta / px delta — active cycles only, excluding
time the prepare thread was descheduled. The two agree on direction and rough
magnitude of the delta at every band (1.21 vs 1.17; 0.65 vs 0.66; 0.73 vs
0.53), so the M-A verdict below does not depend on which clock is used, even
though the two disagree on the absolute rate (see the M-C caveat on the
wall/PMU gap).

**Verdict, per the section 8 thresholds (<0.15 ms / <1.8 cyc/px =
compute-dominated; 0.15-0.30 ms = memory matters, S4 before S5; >0.30 ms =
memory 40%+, build D/D' into S1):**

- **At typical load** (the case the thresholds were anchored to): Δ = 0.098
  ms/frame, 1.17-1.21 cyc/px. Both measures land inside the *compute-dominated*
  band, comfortably. This matches the design review's own predicted "likely
  outcome" (section 8: "the design proceeds exactly as written").
- **At moderate-busy load**: Δ = 0.163 ms/frame — just inside the "memory
  matters, S4 before S5" band in absolute ms — while cyc/px (0.65-0.66) stays
  well under 1.8. The two metrics disagree here because cyc/px's denominator
  (pixel count) grows faster than its numerator (the ms delta) as scenes get
  busier — see M-C.
- **At extreme load** (>300,000 px, which this run shows really occurs and is
  sustained, not a modelling assumption): Δ = 0.372 ms/frame, **above the 0.30
  ms threshold**, while cyc/px (0.53-0.73) is still far under 1.8. Read
  literally by cyc/px alone this would say "still compute-dominated"; read by
  absolute ms it crosses into "memory is 40%+ of the rewrite, build the D/D'
  switch into stage 1." Section 3 of `DESIGN_REVIEW_BANDWIDTH.md` already
  flagged that cyc/px is anchored to an 82,000-pixel frame and would need
  revisiting off that baseline — this is exactly that revisit, and it resolves
  in favour of the ms/frame reading (see "what this means" below).

## M-B: load-side vs store-side

Cross-referencing Build 1's L2 refill count against Build 2's (M-A) *residual*
L2 refill count isolates how much of the shipped kernel's L2 traffic is the
destination store side versus the ROM/side-table load side — a more direct
answer than the store-stall heuristic the design doc proposed, because M-A
removes the destination-miss term outright rather than inferring it.

| band | Normal L2Dref/frame | M-A residual L2Dref/frame | **destination-attributable** | dest share | load share | Normal sstall (% of cyc) | M-A sstall (% of cyc) |
|---|---|---|---|---|---|---|---|
| typical | 2,702 | 1,073 | 1,629 | 60% | 40% | 4.1% | 0.5% |
| moderate | 4,531 | 1,600 | 2,931 | 65% | 35% | 2.1% | 0.4% |
| extreme | 10,464 | 2,109 | 8,355 | 80% | 20% | 1.4% | 0.3% |

Reference point: `tools/road_tiles/analysis/STRATEGY.md` section 1 measured
**2,147 destination lines/frame, none first-touch within the frame** (a
separate, independent 100-frame capture). Build 1's typical-band L2Dref/frame
(2,702) is 26% above that line count — consistent with almost every
destination line missing L2 plus a modest ROM/table overhead, not with the
destination lines being free. Build 2's residual (1,073-2,109, roughly flat
across bands) is in the same range `FABLE_ASSESSMENT.md` estimated for
`sprites[]` rows plus `sprites_shadowinfo` (1,300-2,400 + 320-600 lines) —
somewhat below that estimate's low end, plausibly because ROM rows are reused
across sprites more than assumed, or because part of the 20 KB scratch buffer
itself needs re-fetching into L1D once per frame after the road/tile passes
evict it (a few hundred lines, not enough to change the conclusion).

The store-stall counter (0xE8) drops 8x (typical) when the destination becomes
L1-resident, confirming destination write-allocate misses are a real
contributor to store-buffer pressure — but it is a small fraction of total
cycles either way (≤4.1%), so the *dominant* cost shows up as elapsed time from
linefill latency, not as store-buffer-full stalls specifically.

**Verdict, per the section 8 rule (refills ≤~2,500 with high store-stall =
destination-bound; refills well above with low store-stall = load-bound):**
neither pole cleanly applies. L2 refills sit just above the destination line
count at typical load and grow further at higher load, and the destination
side is the *majority* (60-80%) of that traffic at every band, growing to 80%
in the busiest scenes — so this reads as **destination-bound, more so as load
rises**, with a real but roughly-constant (~1,000-2,100 lines/frame) load-side
contribution from ROM and the shadow-info table that does not disappear at any
scene size.

## M-C: per-pixel rate vs load

| method | typical wall cyc/px | moderate wall cyc/px | extreme wall cyc/px | typical→extreme |
|---|---|---|---|---|
| paced attract, Build 1, `-60 -t 4` | 14.76 | 11.01 | 9.93 | **-33%** |
| uncapped, Build 1, `-perftest -t 4` | 16.01 | 11.39 | 9.50 | **-41%** |

Both independent load regimes (paced attract-mode content and uncapped
flat-out throughput) show the **same monotonic decrease**: cyc/px falls as
scene pixel count rises, the opposite of the rule's stated expectation. The
mechanism is visible in the L2Dref data above: L2 refills/frame grow far
slower than px/frame (2,702→4,531→10,464 for px going ~81k→213k→488k, i.e.
roughly px^0.55), because busy attract-mode scenes here are dominated by fewer,
larger, more-zoomed sprites rather than by more numerous small ones — a wide
zoomed sprite row reuses each 64-byte destination line across many more output
pixels than a small clipped one does, and it amortises the fixed per-source-
pixel decode/branch cost (`FABLE_ASSESSMENT.md` section 2.1) over more output
pixels too.

A genuinely new measured fact, incidental to the pixel-rate question: real
attract-mode content sustains **far busier scenes than previously assumed**.
Both 300 s captures spent over two minutes (67-68 of 147 two-second windows)
above 300,000 px/frame, peaking at 704,796 (attract) and 973,716
(`-perftest`) — 3-4x the 224,000 ceiling `MEASURED_A53_PI3.md` used for its
"busy" projection. Frame drops of 1-10% appear specifically in these windows
(0% elsewhere in the paced run), confirming this is real load, not measurement
noise.

A secondary, unplanned finding: wall-clock cyc/px is consistently 15-25%
above PMU active-cycle cyc/px at every band (e.g. typical 14.76 vs 11.82).
Since `PERF_COUNT_HW_CPU_CYCLES` only counts cycles while the prepare thread is
actually scheduled, this gap is time the thread spent off-CPU — plausibly
`-t 4`'s four threads occasionally contending for the four cores, or the two
extra `read()` syscalls' own dispatch cost. It does not change the M-A verdict
(the delta is consistent either way, see above) but it means part of the
"+29%" in-game contention inflation `DESIGN_REVIEW_BANDWIDTH.md` section 1
quotes may be scheduling rather than pure cache-miss latency. **[E]** as to
cause; the gap itself is **[M]**.

**Verdict, per the section 8 rule ("if busy-scene cyc/px exceeds typical by
>20%, apply the M-A thresholds to the busy figure"):** the *literal* condition
is false — busy cyc/px is *lower*, not higher, by 33-41%. Taken at face value
this says no escalation is needed. But M-A's own ms/frame reading already
crosses the top threshold at extreme load regardless of cyc/px (previous
section), and extreme-load scenes are exactly the ones carrying measured frame
drops. The recommendation below follows the ms/frame reading, not the literal
cyc/px rule, because ms/frame is what the 16.7 ms budget is denominated in and
cyc/px's fall with load is a content-composition artefact (bigger sprites),
not evidence the memory term stopped mattering.

## What this means for the rewrite

- **Target.** Typical-scene behaviour is confirmed compute-dominated
  (0.098 ms / 1.2 cyc/px exposed memory term against `MEASURED_A53_PI3.md`'s
  11.6 cyc/px baseline) — the rewrite's target for typical content stands as
  designed, no re-basing needed there. But the busiest scenes this box's
  attract loop actually produces (300,000-975,000 px/frame, not the previously
  assumed 224,000 ceiling) carry a memory term that grows to 0.37 ms/frame,
  above the top section-8 threshold. The rewrite's busy-scene target should be
  set expecting the memory term to be a real, load-scaling cost, not a fixed
  small addition.
- **S4/S5 order.** At typical load, compute dominates by a wide margin (S5
  compute work is still the bigger lever there), matching the design review's
  default. At the load levels that actually cause drops in this content
  (extreme band), the exposed memory term is large enough that S4 (prefetch,
  replay-side memory work) should not be deferred behind all of S5 — do the
  colour-span `prfm` and D' A/B (already planned for S1) before investing
  further in S5 compute stages, and re-check the in-situ split after S1 lands,
  per `DESIGN_REVIEW_BANDWIDTH.md` section 7 item 7.
- **D/D' switch into stage 1: yes, build it in from the first commit.** M-A
  crosses the ">0.30 ms, memory is 40%+" threshold at extreme load, and M-B
  shows the destination side is the *majority* of L2 traffic (60% typical,
  rising to 80% at extreme load) — both point the same way. The switch was
  already planned for S1 measurement; this data upgrades it from "measure at
  S1" to "ship whichever of D/D' wins, decided at S1 under the checksum gate,"
  since the busy-scene cost of getting this wrong is now measured, not
  estimated.
- **Side-table drop (`sprites_shadowinfo`, `sprites_flipped`) and ROM
  prefetch: worth stage-1 placement.** M-B's cross-check gives the load-side
  L2 cost a real, measured floor: 1,073-2,109 lines/frame, roughly flat across
  scene size (it does not disappear in busy scenes, it just becomes a smaller
  *share* of a bigger total). This is in the range `FABLE_ASSESSMENT.md`
  estimated for exactly the tables the design review recommended dropping, so
  the recommendation is reinforced, and — because it is now a measured,
  present-every-frame cost rather than a modelled one — it belongs in stage 1
  alongside the D/D' switch rather than waiting for a later polish pass.
  Source-stream (ROM row) prefetch, already slated for S1, is supported by the
  same data: whatever remains of the load side after dropping the tables is
  still worth covering with `prfm pldl1keep` two rows ahead.

## Measured versus estimated

**Measured (this document):** all ms/frame, wall cyc/px, PMU cyc/px, L2/L1D
refill and store-stall figures in the tables above, for both builds, across
three pixel-count bands, from two independent 300 s attract-mode captures plus
one 75 s `-perftest` capture; the destination-vs-load L2 refill split (via the
Build 1/Build 2 cross-reference); the existence and duration of sustained
300,000-975,000 px/frame content in real attract-mode play; the frame-drop
correlation with the extreme band; the wall-clock/PMU active-cycle gap.

**Measured elsewhere, carried over and not re-measured here:** the 2,147
destination-lines/frame figure (`STRATEGY.md` section 1, an independent
100-frame capture) used as the M-B reference point.

**Estimated:** the FABLE_ASSESSMENT ROM/shadow-info line counts (1,300-2,400 +
320-600) used as a sanity check on the M-B residual — themselves marked
estimated in that document; the causal explanation for the wall/PMU gap
(scheduling versus syscall overhead — the gap itself is measured, its cause is
not); the mechanism attributed to the cyc/px-falls-with-load finding
(larger/more-zoomed sprites amortising fixed per-pixel and per-line cost) —
consistent with the L2Dref-vs-px scaling measured here, but not independently
confirmed by inspecting which sprites were on screen.
