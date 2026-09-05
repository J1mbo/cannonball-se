# Measured in-situ: S4 (NEON) vs S1 vs shipped, scene-matched, on pi2-local

Date 2026-09-05. Machine `pi2-local` (Raspberry Pi 2 Model B **Rev 1.2** =
BCM2837, 4x Cortex-A53, aarch64, so the NEON paths are active). ARM clock a
**fixed 900 MHz** (`vcgencmd measure_clock arm` = 900000000; governor
`performance` on all four cores). `vcgencmd get_throttled` = 0x0 before and
after **every** run; temperature sampled every 20 s throughout, peaking at
66.1 C on the longest run. 905 MB RAM (so `-j2` builds). This box has a
better thermal path than `sl-d303` (Zero 2 W, no heatsink), which is why it
was chosen: to test whether the S1 regression found on `sl-d303` was a
thermal-throttling artifact. It was not (see Verdict A).

## Why this measurement

`MEASURED_S1_INSITU.md` found, on `sl-d303`, that the LINEBUF rewrite (S1) is
an 11-33% per-pixel regression vs the shipped renderer. Two open questions:
(A) is that finding an artifact of `sl-d303` thermal-throttling under a
missing heatsink, and (B) does the S4 NEON span-replay recover the deficit on
the actual deploy target?

## Method (no pixel instrumentation needed)

`prof_ns_sprites` is already accumulated per frame in `video.cpp` (the `t2-t1`
delta straddling the real `render()` call, so it includes in-situ contention
from the prepare/render/audio threads under `-t 4`) in **all three** trees,
because the phase timing predates the sprite work (merged master). So the only
instrumentation is one throwaway block in `main.cpp`, **identical across all
three trees** (`patch_main.py`): run to a fixed game-frame count (`CB_FRAME_CAP`)
and log cumulative `prof_ns_sprites` in 300-frame windows keyed by
`video.prof_frames`.

Because attract mode is deterministic, **every build walks the identical frame
sequence**, so comparing cumulative sprite time at identical frame boundaries
is scene-matched *by construction* -- no px/frame banding, no reliance on the
shipped renderer's twelve draw-pixel macros, no oracle-fallback confound
(fallback cost, if any, is simply part of that build's real sprite time, which
is the deploy-relevant quantity).

Three clean `git archive` exports, built at CMake defaults (Release,
`-O3 -Ofast -mcpu=cortex-a53+crc`, `PIXEL_ACCURACY=1`), `-j2`:

| tree | commit | md5 of binary |
|---|---|---|
| shipped | `bdf7e9b` | e4a259dfb38e9a5fbb5924adada0a7c2 |
| S1 (LINEBUF scalar) | `24b8aac` | 69533a790ce80a59ded7118783dc4d9c |
| S4 (NEON span replay) | `73dc0c1` | 7bb7ade4aa149405c2af757b331308ee |

NEON confirmed active in the S4 binary (`vld1q_u16` present in its
`hwsprites.cpp`; aarch64 defines `CB_HAVE_NEON`).

**Runs.** Four 9000-frame `-60 -t 4` attract captures back to back,
`SDL_VIDEODRIVER=offscreen`, box idle between each: shipped; S1 `-spritereplay 0`;
S4 `-spritereplay 0` (D); S4 `-spritereplay 1` (D'). All four reached exactly
`frames=9000`, 30 usable 300-frame windows each (window 0 discarded as warmup).

## Results

Cumulative sprite time over the identical 9000-frame sequence:

| build | cum sprite-ns | vs shipped |
|---|---|---|
| shipped (`bdf7e9b`) | 16,682,637,760 | 1.000 |
| S1 scalar (`24b8aac`) | 22,116,424,398 | **1.326** |
| S4 NEON D (`73dc0c1`) | 20,533,907,017 | **1.231** |
| S4 NEON D' (`73dc0c1`) | 20,099,350,813 | **1.205** |

Load-resolved (30 windows split into terciles by shipped per-frame sprite cost;
window-summed ratios, which reproduce the totals above):

| band | windows | shipped ns/frame | S1/ship | S4-D/ship | S4-D'/ship |
|---|---|---|---|---|---|
| light | 10 | 777,616 | 1.385 | 1.248 | 1.241 |
| moderate | 10 | 1,408,879 | 1.373 | 1.282 | 1.252 |
| heavy | 10 | 3,374,385 | 1.292 | 1.206 | 1.177 |
| ALL | 30 | 1,853,626 | 1.326 | 1.231 | 1.205 |

Shipped absolute sprite cost (900 MHz): light 0.778 ms/frame, moderate 1.409,
heavy 3.374, overall 1.854 ms/frame. So S4-D adds ~0.43 ms/frame on average
(overall 1.854 -> 2.28) and ~0.70 ms/frame in the heavy band (3.374 -> 4.07).

Thermal log: throttled=0x0 throughout; per-run peak temps 64.5 / 65.5 / 65.5 /
66.1 C. No throttling occurred on this box in any run.

## Verdict

**A -- THERMAL TEST: the S1 regression is REAL, not a thermal artifact.**
S1/shipped is 1.29-1.39 across load bands on `pi2-local` at 64-66 C with
`throttled=0x0` the entire time. That reproduces (indeed slightly exceeds) the
`sl-d303` finding (1.11-1.33). The regression is not caused by the missing
heatsink on `sl-d303`; it is inherent to the LINEBUF rewrite.

**B -- DECISION: S4-NEON does NOT clear the shipped renderer.** S4-D is a
+21-28% per-frame sprite regression across every load band (+23% overall);
S4-D' is marginally better (+18-25%) but still a clear regression. NEON
recovered roughly 10 percentage points of S1's ~33% scalar deficit but not the
rest. On this deploy target the full rewrite remains a net sprite-throughput
loss versus what ships today.

**Root cause (consistent with the load shape).** The regression is worst at
**light** load and narrows (but never closes) as scenes get busier -- the
signature of a fixed per-frame / per-sprite overhead that the per-pixel copy
work amortises only when there are many pixels. That points at the per-frame
decode/classify recompute (nibble-reverse + shadow classification) the rewrite
does in place of the shipped renderer's cached `sprites_flipped` /
`sprites_shadowinfo` side tables. S4's NEON vectorised the *span copy* -- the
part that was already cheap and that helps most in the heavy band -- and left
the decode/classify overhead untouched, which is why it improves heavy more
than light yet clears none of the bands.

**Note on D vs D'.** On `pi2-local` D' edges D (1.205 vs 1.231 overall),
opposite to `sl-d303` where D won. Immaterial to the decision (both regress),
but it shows the D/D' choice is not robust across A53 SKUs.

## Implication

Shipping S0-S4 as-is would regress sprite throughput ~20-28% on Pi 2 v1.2 /
Pi 3 class hardware. The correctness wins (oracle-exact, F1/F2 fixed, glow) and
the clean testable structure stand, but the performance goal is not met by the
NEON stage. Any path forward must attack the decode/classify recompute (e.g.
restore a cheap per-frame flip/shadow cache) rather than add more span-copy
vectorisation.
