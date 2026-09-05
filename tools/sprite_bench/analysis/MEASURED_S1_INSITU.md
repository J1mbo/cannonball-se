# Measured in-situ: S1 (LINEBUF) vs the shipped sprite renderer, per pixel, scene-matched

Date 2026-09-05. Machine `sl-d303` (Raspberry Pi Zero 2 W, 4x Cortex-A53 at
1.0 GHz, governor `performance` on all four cores, `vcgencmd get_throttled` =
0x0 verified before and after every run, temp 66.6 C at the end of the longest
run). Hi-res 640x448, widescreen off, Blargg NTSC filter on, full CRT shader,
audio on, attract mode, `-60 -t 4`, `SDL_VIDEODRIVER=offscreen` (this box has
no working KMSDRM backend; `offscreen` still exercises a real GL context and
the full `Video::prepare_frame()` path, which is all this measurement needs).

**All instrumentation described below is throwaway, lives only on `sl-d303`,
and is not part of this commit.** Nothing in the Windows working tree changed
— the two source trees measured here are clean `git archive` exports of
commits `bdf7e9b` (S0, shipped) and `24b8aac` (S1, LINEBUF) piped straight to
the box (`~/cmp_ship`, `~/cmp_s1`), never touching the `sprite-rework`
branch's working copy on Windows (which had unrelated in-progress edits).

## Why this measurement

A naive in-game reading suggested S1's `SPRITES` line runs ~2.0 ms against the
shipped build's ~0.93 ms "typical" — apparently a 2x regression. But sprite
cost is strongly scene-dependent (busy scenes have far more pixels), so an
unmatched ms/frame comparison conflates "S1 draws a different scene right now"
with "S1 costs more per pixel." This document isolates the second question.

## Method

**Instrumentation**, identical in spirit on both trees, kept minimal so it
doesn't perturb the measurement:

- **Shipped tree (`hwsprites.cpp`):** a global `uint64_t g_spr_px`,
  incremented inside the `IF_IS_DRAWABLE()`/`IF_IS_DRAWABLE_NO_CLIP()` guard
  of all twelve `draw_pixel_*` macros, by 1/2/3 to match the row count
  (`1row`/`2row`/`3row`) — one increment per destination halfword actually
  written. The eight `_ref` twin macros used only by `render_ref` under
  `CB_RENDER_TEST` (not defined in this build) were deliberately left
  uninstrumented.
- **S1 tree (`hwsprites.cpp`):** the same-named `g_spr_px`, incremented once
  per `replay()` span by `(hi-lo)*(db-da)` — the exact rectangle both the
  COLOUR (D/D') and SHADOW branches write — right after the span is clipped
  and before the mode dispatch, so the counter itself doesn't care which
  replay strategy ran. Not incremented by the `render_px` fallback path (no
  shipped-side equivalent to compare it against).
- **`main.cpp` (both trees):** the existing 2-second FPS report line extended
  with `SPR_PX/frame` and `SPR_CYC/PX` (`prof_ns_sprites` delta in ns, which
  is cycles at this box's fixed 1.0 GHz, divided by `g_spr_px` delta — both
  summed over the window, i.e. a window-weighted ratio, not a mean of
  per-frame ratios). The S1 build additionally prints `fallback/frame`
  (`stat_fallback` delta) and the active `replay_mode`.
- **S1 tree (`hwsprites.hpp`):** `stat_fallback` is compiled only under
  `CB_SPRITE_STATS` (an existing, documented macro: "game via
  `-DCB_SPRITE_STATS=1`"), so the S1 build was configured with
  `-DCMAKE_CXX_FLAGS=-DCB_SPRITE_STATS=1` and a one-line `get_stat_fallback()`
  accessor was added next to the existing `get_replay_mode()`.

Both trees otherwise built at CMake's own defaults (Release, `-O3 -Ofast
-mcpu=cortex-a53+crc`, `PIXEL_ACCURACY=1` — inert for this hot path per S0's
own commit message, confirmed by the shipped `md5sum` differing only in the
instrumented lines). Binaries: shipped `d8837eff2ae7a294464a59b1d64097fb`, S1
`a07435c07765657623ef4c5a025ec9a6`.

**Runs.** Three independent 240 s `-60 -t 4` attract-mode captures on the same
box back to back: shipped, S1 with `-spritereplay 0` (D), S1 with
`-spritereplay 1` (D'). Attract mode is deterministic, so all three walk the
same content. Each produced 118 two-second FPS-report windows (~14,200-14,220
frames). Every window was parsed and grouped into the three pixel-count bands
from `MEASURED_INSITU_SPRITE.md`:

| band | px/frame | windows: ship / S1-D / S1-D' |
|---|---|---|
| typical | 60,000 - 100,000 | 13 / 12 / 12 |
| moderate | 150,000 - 300,000 | 17 / 21 / 20 |
| extreme | > 300,000 (up to ~456,000) | 33 / 27 / 28 |

(An initial 60 s capture per the base method, run first, only reached the
moderate band once and never reached extreme — too short a window onto a
scene-varying attract loop for a robust per-band split — so each run was
extended to 240 s, matching `MEASURED_INSITU_SPRITE.md`'s own reasoning for
using long captures.)

Note on environment: `pi2-local` was tried first for a faster build (per the
task's own suggestion) but a *different, concurrent process* was found
already using and then deleting the `~/cmp_ship`/`~/cmp_s1` directory names
there, and the box rebooted mid-build unprompted — both outside this
measurement's control. `sl-d303` itself was confirmed exclusively used by
this measurement throughout (no other build/game processes, single ssh
origin) and building there directly (`-j1`) succeeded on retry, so all
figures below are built and run on `sl-d303` alone, as the task specified.

## Results: cyc/px per band (window-weighted: total SPRITES ns / total px, over all windows in the band)

| band | shipped cyc/px | S1-D cyc/px | S1-D' cyc/px | S1-D / shipped | S1-D' / shipped |
|---|---|---|---|---|---|
| typical (60-100k) | 21.256 | 28.236 | 27.873 | **1.328x** | **1.311x** |
| moderate (150-300k) | 17.561 | 20.722 | 21.202 | **1.180x** | **1.207x** |
| extreme (>300k) | 13.712 | 15.241 | 16.182 | **1.111x** | **1.180x** |
| whole run (pooled, NOT scene-matched) | 16.267 | 20.125 | 20.635 | 1.237x | 1.269x |

Corresponding avg SPRITES ms/frame at matched px/frame, for reference:

| band | avg px/frame (ship / S1-D / S1-D') | shipped ms | S1-D ms | S1-D' ms |
|---|---|---|---|---|
| typical | 76,810 / 76,350 / 76,445 | 1.633 | 2.156 | 2.131 |
| moderate | 210,278 / 229,626 / 226,051 | 3.693 | 4.758 | 4.793 |
| extreme | 370,625 / 358,373 / 356,578 | 5.082 | 5.462 | 5.770 |

`fallback/frame` was **0.00000 in every band, both replay modes** — the
out-of-envelope `render_px` oracle is never hit during hires attract mode, so
none of S1's measured cost above is oracle fallback; it is entirely
`draw_sprite_fast`/`replay()`.

## Verdict

**S1 is slower than the shipped renderer per pixel, scene-matched, in every
band tested, under both replay strategies** — 11-33% more cycles per
destination pixel written, worst at typical (light) load and narrowing but
not disappearing as scenes get busier. This is a real finding, not a
restatement of the scene-mismatch problem this measurement was built to rule
out: the whole point of banding by px/frame was to remove exactly that
confound, and the regression survives it at every band. The naive alarm
(~2.0 ms vs ~0.93 ms) was misleading in *magnitude* — most of that gap really
was busier scenes costing more, which is normal and expected, not a defect —
but it was not a false alarm: underneath the scene mismatch there is a
genuine, consistent per-pixel slowdown.

**D beats D' overall.** `-spritereplay 0` (D, span-copy memcpy) is faster than
`-spritereplay 1` (D', load-chunk-once) at moderate and extreme load (20.722
vs 21.202, and 15.241 vs 16.182 — D' costing 2-6% more), and about equal at
typical load where the difference (27.873 vs 28.236, D' nominally ahead) is
within the noise of a 12-13 window sample. D' was designed to save redundant
loads across duplicated destination rows, but without the NEON widening
S4 is meant to add, its manual 8-byte chunk load/store loop does not beat the
platform's `memcpy` for this content. **Recommendation: ship D
(`replay_mode 0`) as the default**, matching D's edge growing (not shrinking)
as scenes get busier — the regime that matters most for frame budget.

**Pixel count check.** S1 is expected to draw slightly more than shipped at
matched scenes (it fixes F1's blanked flipped-sprite rows). The measured
band-average px/frame move in both directions by a few percent (typical:
S1 ~0.5% lower; moderate: S1 ~8-9% higher; extreme: S1 ~3-4% lower) rather
than uniformly up — consistent with 2-second wall-clock window boundaries
landing on slightly different frames run to run (not frame-locked between the
three captures) shuffling a few borderline frames between bands, rather than
with a large F1-driven effect. The moderate-band ~8-9% increase is in the
right direction and plausible size for F1, but this data does not by itself
cleanly isolate it from window-alignment noise.

## Measured versus estimated

**Measured (this document):** shipped and S1 (both replay modes) cyc/px and
ms/frame at three px/frame bands, each from a 240 s / 118-window / ~14,200-
frame deterministic attract-mode capture; the 0% fallback rate in both replay
modes across all bands; the D vs D' ordering and its trend with scene busyness.

**Estimated / not isolated here:** the causal attribution of the regression
(which part of decode-once/replay/schedule-table setup costs more than the
shipped accumulator loop) — this document answers *whether* S1 regressed
per pixel and *which* replay mode to ship, not *why*; the px/frame band-shift
data is suggestive of the expected small F1-driven increase but is not
strong enough, on its own, to separate that from window-boundary noise.
