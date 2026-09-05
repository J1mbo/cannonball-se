# Measured in-situ: R1 (de-glue + templated fill) vs shipped, on pi2-local

Date 2026-09-06. Machine `pi2-local` (Pi 2 v1.2, A53 aarch64, 900 MHz fixed,
governor `performance`, `throttled=0x0` before/after every run, peak 61.8 C).
Same box, method and 9000-frame deterministic-attract protocol as
`MEASURED_S4_INSITU.md`, so the numbers are directly comparable to the S4
baseline measured there.

Three clean `git archive` trees, `-j2`, CMake Release defaults
(`-O3 -Ofast -mcpu=cortex-a53+crc`), only the throwaway `patch_main.py`
windowed harness added:

| tree | commit | md5 |
|---|---|---|
| shipped | `bdf7e9b` | e4a259dfb38e9a5fbb5924adada0a7c2 |
| R1a (de-glue) | `2f5e3eb` | 95ccb98674a77f5cc919103ad67aaa99 |
| R1b (fill + ladder) | `59a5b6a` | 521740f310ccf9843bbfd1616806ce78 |

Shipped reproduces the S4 doc's shipped total (16.62 G vs 16.68 G ns over the
same 9000 frames), confirming no box drift.

## Results

Cumulative sprite time over the identical 9000-frame sequence:

| build | cum sprite-ns | vs shipped | vs S4 (20.53 G) |
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

## Verdict

**The S4 regression is eliminated and reversed.** R1b is faster than the
shipped renderer overall (0.905x) and in the moderate (0.913x) and heavy
(0.880x) bands, and dead-even in the light band (1.000x) -- while drawing the
glow effect and carrying the F1/F2 correctness fixes the shipped renderer
does not have. Versus the S4 rewrite it is a 27% reduction in sprite time
(20.53 G -> 15.04 G).

**Attribution held.** R1a (de-glue alone, fill unchanged) already flipped the
regression to 0.924x -- confirming R0's finding that the cost was the
row-proportional glue (uninlined calls, per-nibble reloads, redundant
schedule lookups, per-word bounds), not per-pixel work. R1b's shift-OR fill +
copy ladder added a further ~2 points overall and the most in the heavy band
(0.910 -> 0.880), where per-pixel fill/copy work dominates -- exactly where a
cheaper fill should help most.

**Gate.** Fable's aspirational gate was <= 0.90x in every band. R1b meets it
in heavy (0.880), is close in moderate (0.913), and only reaches parity in
light (1.000). Light is the known-weak case (R0: the deficit was always worst
at light load; SPRITE_REPLAN_FABLE.md R4 -- direct-emit for single-row
sprites -- is the documented, conditional remedy for exactly this band).
Practically it does not matter: light scenes cost 0.77 ms/frame with ample
frame-budget headroom; the win that matters for holding 60 fps is the 0.40
ms/frame saved in the heavy band. R1b is a strict improvement over shipped
(never slower, faster where it counts, plus glow + F1/F2).

## Status / next

R1 (A53) is done and is a net win. Remaining per the re-plan and the widened
targeting: R2 (make the NEON path portable to ARMv7/A7), R3 (promote the
shipped-kernel twin to a corrected non-regressing ARMv6 fallback), and the
optional R4 (light-band direct-emit). Deploy candidate for the Pi 2 gameplay
test: `59a5b6a` (R1b).
