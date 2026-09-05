# Design review: is the line buffer the right intermediate?

Reviewed: `DESIGN_FABLE.md` section 2 (decode once into `line[]` + `rec[]`, replay
onto `r_v` rows). Sources: `MICROARCH_A7_A53.md` (cited as MA), `SCHEDULE_VERIFICATION.md`,
the upstream reference. Numbers below were produced by two scripts in the session
scratchpad (`rv_model.py`, `budget.py`) so every figure can be regenerated.

**Verdict.** The line buffer is not the right choice as proposed. Its second
justification (the wide-store overrun must be confined to scratch) is false: a one-pixel
lookahead makes the overrun exact in the frame buffer, and in every zoom class of the
hi-res default the overrun never reaches past the next source pixel. Its first
justification (amortise decode over `r_v`) is shared by a design that stores directly to
all `r_v` rows from the decode. Removing the intermediate saves 0.3-0.5 memory slots and
0.3-0.8 issue cycles per output pixel, on both cores, in every `(r_h, r_v)` cell that can
occur. A cheaper repair of the line buffer (replay all `r_v` rows in one pass) closes
about two thirds of the gap on the A7 and one third on the A53, and keeps the exact-span
property; it is the fallback if measurement shows the interleaved store order hurts.

## 0. Alternatives as modelled

Notation: `r_h`, `r_v` per-axis runs, `rho = r_h * r_v` output pixels per source pixel,
`K` output pixels per row (100), `W = 8 r_h` output pixels per source word, `L` mean
colour span (66 px, 1.5 spans per row), class mix by source word 60% COLOUR / 25%
MIXED / 15% GAP as in the design. A "slot" is one issue of the single load/store pipe
(MA section 1); a 128-bit NEON load or store is two slots on both cores (MA sections 3,
6). The shadow term (10% of pixels, read-modify-write) is the same for every alternative
and is carried as the design's 0.21 / 0.18 cycles.

* **D** - the design: scalar wide-store decode into `line[]`, records, replay one
  destination row at a time (16-byte load + 16-byte store per 8 px).
* **D'** - D with the replay loading each 16-byte chunk once and storing it to all
  `r_v` rows. A refinement, not in the design.
* **A1** - register staging as posed: per word, NEON `tbl` expansion into vector
  registers (index vectors per word from a per-sprite generated table, loaded once per
  word and reused for `r_v` rows), exact-width stores to each row, narrow lane stores for
  the `W mod 8` tail. MIXED words fall back to A2.
* **A2** - per-source-pixel fan-out: D's decode loop, but each wide store goes to the
  `r_v` destination rows instead of `line[]` (`str x,[dst]`, `str x,[dst,#stride]`, ...
  then `add dst, run2`), with run-end exactness by one-pixel lookahead (section 4).
  Alternative A's amortisation with alternative B's trick; no scratch at all.
* **B** - A2 with `r_v = 1`: the row is re-decoded for every destination row.
* **C** - D for `r_v >= 2`, B for `r_v = 1`, selected per source row.

## 1. Memory operations per output pixel

Source reads (all alternatives, once per source row): one 32-bit word load plus one
8-byte run-table load per 8 source pixels = `0.25 / rho` (A2, D); B repeats them per
row: `0.25 / r_h`; A1 needs no run bytes: `0.125 / rho`.

Scratch traffic:
* D: one wide store per source pixel into `line[]` plus one record store per MIXED
  source pixel (25%): `1.25 / rho`; replay loads 16 bytes per 8 px: `0.25`. Total
  `1.25/rho + 0.25`.
* D': the chunk load is shared by the `r_v` rows: `1.25/rho + 0.25/r_v`.
* A1: one 16-byte index load per 8-px chunk, shared by `r_v` rows: `0.25/r_v`.
* A2, B: none.

Destination stores:
* D: 16-byte store per 8 px = `0.25`, plus the re-based tail store per span-row,
  `4/L`: `0.31`. D': tail per span-set, `0.25 + 4/(L r_v)`.
* A1: `0.25` plus 1.5 narrow tail stores per word-row: `0.25 + 1.5/W`.
* A2, B: one wide store per source pixel per row = `1/r_h`, plus two extra stores at
  each run end (three spans per row, six ends): `1/r_h + 0.06`.

| `(r_h, r_v)` | D | D' | A2 | B | A1 |
|---|---|---|---|---|---|
| (1, 1) | 0.31+1.50+0.25 = **2.06** | 2.06 | 1.06+0+0.25 = **1.31** | 1.31 | 0.81 |
| (2, 1) | 1.31 | 1.31 | **0.69** | 0.69 | 0.66 |
| (1, 2) | 1.31 | 1.16 | 1.19 | 1.31 | 0.62 |
| (2, 2) | 0.94 | 0.78 | **0.62** | 0.69 | 0.50 |
| (2.27, 2.27) mean | 0.85 | 0.68 | **0.55** | 0.61 | 0.47 |
| (3, 2) | 0.81 | 0.66 | **0.43** | 0.48 | 0.46 |
| (3, 3) | 0.73 | 0.52 | **0.42** | 0.48 | 0.41 |
| (4, 4) | 0.65 | 0.42 | **0.33** | 0.37 | 0.37 |

Where the ranking changes. A2 beats D everywhere except `r_h = 1` with `r_v >= 3`
(A2 1.14 vs D 1.06 at (1,3)), and beats D' except at `r_h <= 1.5, r_v >= 3` and
`(2, 4)`. None of those cells occur: both axes share one zoom, so `r_h` and `r_v` are
each floor or ceil of the same `512/zoom`, and the only off-diagonal cells are (1,2),
(2,1), (2,3), (3,2), (3,4), (4,3). In every reachable cell A2 has the fewest slots of the
scalar alternatives. B is only 0.06 behind A2 in slots because the stores are the same;
its penalty is entirely in re-decoded ALU work. A1 wins on slots at `r_h <= 2` because
it issues 16-byte stores where A2 issues one 4- or 8-byte store per source pixel; at
`r_h >= 3` the two are level. The line buffer's cost is the `0.25` replay load plus the
`1/rho` line store, and D' halves the first term only for `r_v = 2` and quarters it for
`r_v = 4`.

## 2. Issue cycles per output pixel

A7: the design's class-weighted decode of 6.5 cycles per source pixel (largely single
issue; MA section 1 says most pairs do not dual-issue), replay load/store-bound at 0.5
per pixel, span setup 15 cycles per span, per-source-row overhead 50 cycles, per-row-copy
overhead 30 cycles. A2 replaces the line store with `r_v` stores, drops the record path
(-0.5), adds the lookahead (+0.3); in the MAXRUN = 4 class (`r_h >= 2.5`) each wide store
is `vdup + vst1.64` or two `str` (A32 `strd` needs word alignment), counted as 2 cycles.
A53: decode 4.7 (design); A2 per source pixel `max(0.25 + r_v, (6 + r_v)/1.8)` (LS-bound
against dual-issue bound); replay LS-bound; A1 uses the 16-bit-table `tbl` form (index
load 2 slots + `tbl` 2 cycles per 8 px) with a 10-cycle per-word table build.

| `(r_h, r_v)` | A7 D | A7 D' | A7 A2 | A7 B | A7 A1 | A53 D | A53 D' | A53 A2 | A53 B | A53 A1 |
|---|---|---|---|---|---|---|---|---|---|---|
| (1, 1) | 8.2 | 8.1 | 7.1 | 7.1 | 4.0 | 6.1 | 6.0 | 4.5 | 4.5 | 2.8 |
| (2, 1) | 5.0 | 4.8 | 4.0 | 4.0 | 2.8 | 3.7 | 3.6 | 2.5 | 2.5 | 2.0 |
| (1, 2) | 4.7 | 4.4 | 4.2 | 7.1 | 2.4 | 3.6 | 3.3 | 2.6 | 4.5 | 1.7 |
| (2, 2) | 3.1 | 2.7 | 2.3 | 4.0 | 1.7 | 2.4 | 2.1 | 1.5 | 2.5 | 1.3 |
| (2.27, 2.27) | 2.7 | 2.3 | 1.9 | 3.6 | 1.5 | 2.1 | 1.8 | 1.25 | 2.3 | 1.1 |
| (3, 3) | 2.1 | 1.7 | 1.7 | 2.9 | 1.2 | 1.7 | 1.3 | 0.9 | 1.9 | 0.9 |
| (4, 4) | 1.8 | 1.3 | 1.2 | 2.4 | 1.0 | 1.4 | 1.0 | 0.6 | 1.6 | 0.7 |

Dominant terms. D on both cores: the replay round trip (0.5 LS-bound cycles plus 0.2
span setup) exceeds the amortised decode (`6.5/rho = 1.3` at the mean, 0.4 at (4,4)) for
every `rho > 5`; the intermediate, not the decode, is the largest term for the big
sprites that dominate the pixel count. A2: the decode, `(6 + r_v)/rho`; on the A53 the
`r_v` stores hide under ALU dual issue until `r_v = 4`. B: the un-amortised decode
`6.3/r_h`, which is why B loses to D above `r_v = 2` despite fewer memory slots. A1: the
per-row 16-byte store (0.25) plus the NEON expansion `0.75/r_v`; it rests on NEON
timings MA section 6 could not source, so treat A1's column as a lower bound with a
plausible range up to 1.5x.

Undocumented figures and how the conclusion moves across their range:
* Unaligned store cost (MA section 3: "not found" for A7; +1 cycle per 8-byte boundary
  crossing on A53, measured for loads, not confirmed for stores). A2's 8-byte stores at
  2-byte alignment cross a boundary 75% of the time; if the +1 applies, add `0.75/r_h`
  (+0.33 at the mean) to A2 and B, and `0.125` to D, D', A1. A2 at the mean goes from
  1.25 to 1.58 on the A53; D' from 1.8 to 1.9. The ordering does not change; the margin
  narrows from 30% to 17%.
* A7 dual-issue rate (25% assumed by the design, MA says narrower): scaling every A7
  ALU term by 0.8-1.1 moves all columns together; ordering unchanged.
* Store buffer depth and outstanding write-allocate misses (MA sections 2 and 8: A7 has
  4 x 64-bit slots, A53 entry count unknown, nothing on concurrent linefills). D writes
  one row at a time (one miss stream); D', A1 and A2 interleave `r_v` streams. With one
  outstanding fill, interleaving adds no misses but overlaps less compute per stall; with
  `r_v` outstanding fills it recovers up to `(r_v - 1)/r_v` of the exposed latency. This
  is the one unknown that could reverse the recommendation; measurement M1 targets it.
* The memory floor itself (1.6-3.1 cycles per pixel in the design's estimate). If wall
  time is `max(compute, floor)` the A53 is at the floor under every alternative and the
  choice is invisible there; if it is `compute + floor` (a 4-entry store buffer suggests
  this on the A7) the 0.3-0.8 cycle saving is worth 8-20% of wall time.

**Alternative C.** From the shipping `ZOOM_LOOKUP` (256 input levels, art widths 94/46/
23/12/4 for SIZE1..5, output area `art^2 (512/z)^2`), the share of output pixels whose
source row has `r_v = 1` is **3.5%** if sprite count is uniform over input zoom level
(near sprites dominate), **18%** if uniform in depth or equal per distinct zoom (the
SCHEDULE_VERIFICATION weighting). The pixel-weighted effective `r_v` (harmonic) is 3.1
to 6.6, so the design's `rho = 6.25` is inside the range. The premise that this class is
"small distant sprites" is wrong: the class includes SIZE1 art at 1.0-1.3x, 95-120 px
wide in hi-res (60-96% of their rows are `r_v = 1`); only sprites narrower than 97 px are
more than 90% in the class. D wastes 0.85 (A7) / 0.6 (A53) cycles per pixel on that
class relative to B, so a B path inside D would save 0.03-0.15 cycles per pixel on the
frame, 1-6% of compute. Not worth a second decode loop in D. In A2 the `r_v = 1` template
*is* B, so C is subsumed at zero code cost.

## 3. Recommendation

Build **A2** as the structure: decode each visible source row once, store each source
pixel's wide value to all `r_v` rows directly, no `line[]`, no `rec[]`, no replay. Keep
the design's word classes (GAP skip, SWAR COLOUR fast path, MIXED slow path) and its
schedule tables unchanged. Concretely:

* Row bases: the `r_v` rows are consecutive on screen (`y = top + ydelta * d`, `d`
  contiguous), so one base register at the lowest row and immediate offsets `stride *
  i` serve all rows: A64 `str x, [dst, #1280*i]` (scaled 12-bit, fits to 32 KB); A32
  `str r, [dst, #imm12]` fits 3 x 1280 = 3840 for 640 px, but not 3 x 1616 for
  widescreen, which needs `str r, [dst, rS3]` with one stride register. Register cost
  versus D's decode loop: +0 to +1.
* Templates over `(MAXRUN, R_V, FLIP, XDIR)`; `R_V` is fixed per source row, so the
  dispatch is one indirect branch per source row (about 10 cycles on the A7's 8-entry
  BTAC, `0.02` per pixel). Instances that occur: MAXRUN 4 with `R_V` 2-4, MAXRUN 2 with
  `R_V` 1-2, MAXRUN 1 with `R_V` 1: six per direction pair, about 5 KB total against a
  32 KB L1I. A `rows-visible < R_V` partial set (top or bottom clipping) uses the
  smaller `R_V` instance; no dummy rows.
* Run-end exactness by the rule in section 4, branch-free on the COLOUR path (only the
  word's last pixel is tested, against the next word's first nibble) and a predicted
  branch per pixel on the MIXED path.
* Edge words: any word whose output range plus `MAXRUN - 1` is not inside `[cx0, cx1)`
  goes through a per-pixel exact path. At most two per row.

Selection rule: compile-time templates, run-time selection per source row on `r_v(j)`
from the schedule table and per sprite on the MAXRUN class. Cost: the dispatch branch
above plus one compare per source row.

Fallbacks and stages. If measurement M1 shows the interleaved store order costing more
than the 0.4-0.5 cycles per pixel A2 saves in compute, revert to D itself (D' interleaves
too). **A1** is the NEON stage on top of A2 (the design's S5), COLOUR words only, sharing
A2's MIXED and edge paths; expected gain 0.4-0.5 cycles per pixel on the A7 at the mean,
probably hidden under the memory floor on the A53. Do not build C.

## 4. Correctness obligations that differ

Common to all: per-row `y in [0, h)`; 16-bit word index wrap; the terminator is tested
after every word including words fully clipped; `ramBuff[data+7]` write-back if the 68K
reads it.

**The frame buffer's slack cannot make any overrun safe.** The 128-byte front pad is 64
pixels before row 0 and the two spare rows follow row `h-1`. A store past the right end
of row `y < h-1` lands on row `y+1`'s left edge; past the left end of row `y > 0` on row
`y-1`'s right edge; both are visible. The slack only makes a bug non-fatal on rows 0 and
`h-1` (64 px left, two rows right). Every alternative must guarantee zero stores outside
the sprite's opaque span on each row; the slack is for the debug canaries, not design.

**D / D'.** Frame-buffer stores are exact span copies in `[max(lo,cx0), min(hi,cx1))`
with the tail re-based to `hi - 8` (requires `n >= 8`; `n < 8` takes the narrow-store
path). All overrun is in `line[]`: forward fill writes `[pos, pos + MAXRUN)` per pixel
and the `pos >= pend` test is per word, so the highest index written is
`K_max + 32 + MAXRUN - 2`; reverse fill (`xdelta < 0`) writes `[pos - MAXRUN + 1, pos]`
so the base must leave `MAXRUN - 1 + 32` entries below the lowest position. `line[2048
+ 64]` covers 808 px. **`rec[520]` is undersized:** one record per source pixel is
possible, and a row runs until `pos >= pend` or the terminator, so up to `K_max + 16`
source pixels in the magnify regime (`K_max = 640` or 808) and `2 K_max + 16` in the
shrink regime; size `rec` to 1024 entries, or bound the word count explicitly.

**A2 / B.** Lookahead rule, with `t(i) in {G, C, S}` and `t = END` past the last drawn
pixel (terminator word's last nibble, or the last pixel before `pend`): pixel `i` with
run `r_i` stores `MAXRUN` copies iff `t(i) != G` and (`t(i+1) == t(i)` or `t(i) = S,
t(i+1) = C`); otherwise it stores exactly `r_i` copies (three stores: `strh` at `p`,
`strh` at `p + 2(r-1)`, `str` at `p + 2 max(r-2,0)` redirected to a scratch word when `r
= 1`, or predicated on A32). Sufficiency: overrun width `MAXRUN - r_i` must not exceed
`r_{i+1}`. Zoom `[0x80, 0xAB)`: runs {3,4}, MAXRUN 4, overrun <= 1; `[0xAB, 0x100)`: runs
{2,3}, overrun <= 2 = min run (tight); `[0x100, 0x200)`: runs {1,2}, MAXRUN 2, overrun <=
1 = min run (tight); `>= 0x200`: no overrun. Assert per zoom table at generation:
`MAXRUN - r_i <= r_{i+1}` for all `i`. The `S -> C` case is safe because the colour
store, later in program order on the same row, covers the whole overrun; `C -> S` is a
run end because the shadow RMW would read the colour value. Order within a source pixel
is rows 0..`R_V-1`, so per row pixel `i` always precedes `i+1`. The lookahead must cross
the word boundary (load the next word early) and must see END when the current word
terminates the row regardless of the next word's content. Edge words are exact per pixel.
With these, no store lands outside `[cx0, cx1)` on any row and the pad is unused.

**A1.** Word-confined exact stores; the tail is `W mod 8` pixels as 4/2/1-px lane
stores from the last chunk register, never a re-based 16-byte store unless `W >= 8`
(always true in the magnify regime). Forward overrun into the next word is permitted only
when the next word is COLOUR (classified first). MIXED words use A2's rule per run.

## 5. Measurement that settles it

All four run before any sprite logic exists, on both mules, pinned to one core, with
the NTSC filter (or a 16 MB/frame streaming load) on two other cores, `perf_event_open`
counters per the existing `pmu_test.c`.

* **M1 store-order synthetic** (decides A2 vs D'/D): generate a sprite list from
  `ZOOM_LOOKUP` with the section 2 depth mix (or a recorded list once stage 0 exists),
  constant data, no decode. Kernels: (a) D, row-major 16-byte copies from a 4 KB L1
  buffer; (b) D', chunk loaded once, stored to `r_v` rows; (c) A2, one 8- or 4-byte store
  per source pixel per row using the real run tables; (d) B, (c) with `r_v = 1` repeated.
  Report cycles per output pixel, L1D refills (0x03), and on the A53 `STALL_SB_FULL`
  (0xC7), `ST_STALL` (0xE8); on the A7 the store-buffer-full stall event if the TRM's
  event table confirms one. Prediction: (c) within 5% of (a) if fills overlap, up to
  `(r_v-1)` x the per-miss stall worse if they do not.
* **M2 unaligned stores**: L1-resident loop of 8-byte and 16-byte stores at 2-byte
  versus natural alignment, both cores. Fixes the +0 to +0.75 per store range above.
* **M3 compute-only**: kernels (a)-(d) with the real decode into a 16 KB wrapped
  destination. Prediction from section 2: A2 below D by 0.8 (A7) and 0.85 (A53) cycles per
  pixel at the mean; D' between.
* **M4 `r_v` histogram**: stage-0 counters over recorded play, output-pixel weighted,
  to replace the 3.5-18% model range.

Decision rule: adopt A2 if `M1(c) <= 1.05 * min(M1(a), M1(b))` and `M3(c) < M3(a)`;
otherwise D' if `M1(b) <= M1(a)`, otherwise D. Each kernel is under 100 lines of C.
