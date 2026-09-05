# Proposed optimisation strategy: road, tiles and sprites on Cortex-A53

Consolidated 2026-09-04 from the independent plan in `OPTIMISATION_PLAN.md` (Fable
agent, isolated from all prior analysis), the two earlier static reviews in
`../../sprite_bench/analysis/{ROAD,TILES}_ANALYSIS.md`, and the measured phase
profile in `../../sprite_bench/analysis/PHASE_PROFILE.md`.

## 1. The headline: the cost is the first touch of each cache line

The independent review instrumented its own build, captured 100 frames with
per-layer buffer snapshots, and accounted for every 64-byte line of the 573 KB
destination:

| layer | lines touched | of which first touch |
|---|---|---|
| road background | 4,581 | 4,581 |
| tile background | 1,240 | 372 |
| tile foreground | 552 | 44 |
| road foreground | 4,379 | 3,962 |
| sprites | 2,147 | 0 |
| text | 284 | 0 |

Every one of the 8,960 lines is first-touched exactly once per frame. Because
573 KB does not fit a 512 KB L2 that the NTSC threads are streaming a 15.8 MB
table through, **each first touch is a write-allocate line fill from DRAM**. The
road background costs 58 cycles per line alone and 149 under load, and its
store-issue floor is only about 0.02 ms, so that call is more than 90% waiting
for line fills.

Estimated split of the 2.51 ms for road, tiles and text: write-allocate fetches
1.0 to 1.2 ms, the road foreground pixel loop 0.6, the two tilemap scans 0.2 to
0.25, tile pixel kernels 0.1, text 0.1 to 0.3.

**This locates the hypothesis precisely.** Memory is the first-order problem, and
it is paid at exactly one place per line: whichever layer touches that line first.
It is latency and read-for-ownership traffic, not store throughput.

## 2. What the data allows

Two findings from decoding the actual arcade ROMs change what is possible.

**The road is 17 fills, not 320 pixels.** Every line of the road ROM is at most
15 runs of constant value, and 220 of the 256 lines have exactly 15. Merging the
two roads per output row gives a mean of 16.9 constant-colour segments and never
more than 29. The renderer currently runs 320 iterations with four loads each to
produce what is a list of about 17 fills. A run-length table for the whole ROM is
about 13 KB and stays in L1, against 512 bytes of texture streamed per road per
row today.

**Tile rows are mostly whole.** Of rendered tile rows, 43% of background and 61%
of foreground rows are fully opaque and only 5% and 10% are mixed; transparency
is concentrated at tile edges. And because the palette base is shifted left by
three, adding a nibble never carries out of the low byte, so a pixel's colour is
a plain add with no table lookup. An 8-pixel row becomes two 128-bit vectors with
about twelve vector operations and four wide stores, against 32 halfword stores.

**Overdraw is measurable.** 80% of tile-foreground pixels and 33% of
tile-background pixels land in rows that the road foreground later overwrites in
full. That work, and the line fills it triggers, is invisible.

**Important clarification on the two road passes (verified 2026-09-04, prompted by
the maintainer).** The names mislead. The two passes are complementary by row, not
stacked:

- `render_background` (`hwroad.cpp:521`) paints **only the sky rows** — the rows
  above the horizon, where a road has the low-priority bit `0x800` set
  (`hwroad.cpp:535` onward). It skips the road rows.
- `render_foreground` (`hwroad.cpp:595`) paints **only the road rows**. It skips
  any row where both roads are low priority (`hwroad.cpp:631`).

Between them run the two tile layers. So:

- On a **sky row**, the order is road-background sky colour, then tiles on top
  (mountains, scenery). Nothing overwrites the tiles. This is the "road is the
  background" case.
- On a **road row**, the tile layers draw first, then `render_foreground`
  repaints every pixel of that row opaquely, **including the off-road margin**
  (texture value 3 still writes `color_table[pix0]`, `hwroad.cpp:740-745`; there
  is no transparency test). So no tile pixel in a road row survives. The
  overwritten tiles are the grass/ground/scenery under the road region.

This means the overdraw is real and P4/P5 are valid, but the overwrite mask is
**not** simply "the bottom N rows". It must be derived per frame from two
conditions, and getting either wrong deletes visible tiles:

1. The per-row skip in `render_foreground` (both roads low priority →
   `hwroad.cpp:631`), which decides which rows the foreground actually paints.
2. The horizon guard at `video.cpp:208`: when
   `config.engine.fix_bugs && oroad.horizon_base == ORoad::HORIZON_OFF`, the
   foreground pass does not run at all, so **no** tile row is dead that frame.

P4/P5 must build the mask from exactly those, and the debug assertion in section 6
(the mask equals the rows the foreground actually writes) is what catches a
mismatch.

## 2a. MEASURED UPDATE 2026-09-04: DC ZVA is killed, the bottleneck is loads not stores

The plan's item 0 was a `DC ZVA` microbenchmark with a kill rule. It ran on the mule
(`scratchpad/pi3/dczva_bench.c`). `DCZID_EL0` = 0x4, block 64 bytes, permitted. Filling
the 560 KB buffer with a constant:

| strategy | solo | 3-core thrash |
|---|---|---|
| plain NEON store | 0.091 ms | 0.60 ms |
| `DC ZVA` then store | 0.192 ms | 1.32 ms |
| `DC ZVA` only (floor) | 0.091 ms | 0.65 ms |

The plain store already matches the zero-only floor, so there is no write-allocate read
penalty for `DC ZVA` to remove: the A53 is evidently write-streaming full lines already.
Adding `DC ZVA` writes each line twice and is 0.46x, i.e. slower. The kill rule (abandon
below 1.5x) fires. **S2 and S4 are dropped from the plan.**

Larger consequence: a flat fill of the whole 560 KB buffer is 0.09 ms solo, so
destination store bandwidth is **not** the dominant cost. The in-game road cost is loads,
per-pixel compute and branches, chiefly the two texture loads per pixel-pair from the
256 KB `roads[]` table (8x L1D). This raises the run-length road foreground (S6) to the
headline change and lowers the expected value of the tile-clip (S5, which mostly saves
stores). The write-allocate line-accounting in section 1 was a real observation but its
cost was over-attributed; the fills are cheap because the core streams them.

Revised expectation for road+tiles+text: the win comes from S6 (removing texture loads
and per-pixel branches) and S1 (removing wasted scan/loads), not from touching memory
less. Target is still meaningful but re-based on compute.

## 3. The plan, in the order I would run it

## Implementation progress

- **S1 (visible-window scan) — DONE, `d461d6c`.** Bit-exact (four checksums + exhaustive
  cell-set test). Mule: tile_bg 0.43 to 0.33 ms, tile_fg 0.20 to 0.12 ms. Gate `fcf9d05`.
- **S0 (test harness) — DONE, `b95d4d0`.** Reference twins + standalone driver; shipping
  game still bit-exact.
- **S6 (run-length road foreground) — DONE, `b06811a`.** Bit-exact: four checksums, and
  the driver's exhaustive sweeps (s6a-d) pass on x86 scalar and on real A53 NEON. Per-row
  speedup on the real ROM 4.4x. In-game road_fg 0.88 to ~0.26 ms (~3x).
- **S3+S5 (overwrite mask + tile clip) — DONE, `fb3d6d1`.** Bit-exact; the CB_RENDER_CHECKS
  invariant assertion never fired across 600 attract frames in all four modes.
- **S7 (NEON tile-row kernel) — DONE, `027642b`.** Bit-exact: four checksums plus an
  exhaustive per-word unit (8.55M invocations) on x86 scalar and real A53 NEON.

**Measured result, 60 fps four-thread on the mule (road+tiles+text):**

| phase | baseline | after |
|---|---|---|
| road background | 0.68 | ~0.66 (untouched) |
| tile background | 0.43 | ~0.28 |
| tile foreground | 0.20 | ~0.075 |
| road foreground | 0.88 | ~0.26 |
| text | 0.31 | ~0.08 |
| **sum** | **~2.50 ms** | **~1.35 ms** |

About 1.1 ms of a 16.7 ms frame recovered. Road background (the streaming sky fill) is now
the largest untouched item; DC ZVA does not help it (killed) and it is bandwidth-bound, so
it is left as is. All five sections retain a scalar fallback for non-NEON/MSVC builds.

Order after the DC ZVA kill (section 2a):

| # | Change | Saving | Risk | Effort |
|---|---|---|---|---|
| 1 | Scan only the visible tilemap window | ~0.25 ms | negligible | 2 hours |
| 6 | Run-length road foreground (now the headline) | ~0.6 ms | moderate | 1-2 days |
| 3 | Derive the road overwrite mask + invariant assertion | gates 5 | none | 2 hours |
| 5 | Clip tile layers to rows the road will overwrite | ~0.05-0.15 ms | low | half a day |
| 7 | NEON tile-row kernel | 0.05-0.1 ms | low | 1 day |
| ~~2, 4~~ | ~~DC ZVA line pre-allocation~~ | **killed by microbench** | | |
| 8 | Fused sky composer / band interleaving | ~0.1 ms | high | defer |

Expected end state for road, tiles and text: **2.51 ms down to roughly 1.2-1.5 ms**,
re-based on compute after the DC ZVA kill removed the ~0.6 ms the memory items were
credited with.

### Why this order differs from the source plan

The independent plan ranks `DC ZVA` first as a single item covering both road
calls. I have split it. Zeroing lines in the **road background** is unconditionally
safe: it is the first writer of those lines in the frame. Zeroing them in the
**road foreground** is safe only because the road foreground rewrites every pixel
of its rows, and by that point the tile layers have already drawn into them. If
that invariant fails in any control mode or edge case, tiles silently vanish into
black. The same invariant is what makes item 5 valid. So it gets proved once, with
an assertion comparing the mask against the rows actually written, before either
item depends on it.

I also moved the visible-window scan ahead of everything. It is independent of the
memory question, carries no risk, and can land while the microbenchmark runs.

## 4. Where the estimates are soft

**The `DC ZVA` saving is the least certain number in the plan.** It removes the
read half of write-allocate traffic, halving DRAM traffic for the fill, and it
removes the exposed fill latency. But the dirty lines must still be written back.
Whether the visible cost drops to 0.1 ms, as the source plan estimates, or to
about 0.35 ms, depends on how much of that writeback overlaps later work. The
microbenchmark settles it, and the kill rule is well chosen: abandon if the
capability bit says prohibited, if the loaded fill improves by less than 1.5x, or
if the in-game road background stays above 0.40 ms.

**The text measurement does not fit.** 0.31 ms is consistent only with a
text-heavy screen of 600-plus tiles, not the 33-tile driving frame it was
attributed to. The two-second sampling window probably sat on the high-score
screens. Re-measure with the per-frame tile count logged beside the time before
treating text as a target.

**The single-thread comparison is not a controlled A/B.** That run drops half its
frames, so scene content differs. It is good enough to show that the streaming
fill is the contention-sensitive phase, not to quantify it precisely.

## 5. How this sits against the sprite work

| target | now | after | saving |
|---|---|---|---|
| road + tiles + text | 2.51 ms | 0.8-1.2 ms | **1.3-1.7 ms** |
| sprite kernel rewrite | 0.93 ms | ~0.47 ms | 0.47 ms |

Road and tiles is roughly three times the opportunity, which confirms the
reordering the phase profile suggested. Doing both takes the whole prepare-thread
render from 3.44 ms to about 1.3-1.7 ms, freeing 1.8 to 2.2 ms of a 16.7 ms frame.

Note that the two efforts share a lever. The sprite work concluded that fewer and
wider stores win; the road work concludes that avoiding the first-touch line fill
wins. Both are the same underlying fact about this machine: the memory system is
the constraint, and the way to go faster is to touch it less.

## 6. Correctness gate

Output must stay bit-identical. The verification the source plan proposes is
sound and should be adopted wholesale: keep the current renderers as reference
implementations in a test build; exhaustively compare the run-length road against
the scalar loop across all lines, scroll values and control modes; compare the
NEON tile row against the scalar row across all codes, palettes and clip offsets;
property-test the window scan against the full scan across all scroll values; and
capture a few hundred frames of per-layer snapshots to replay both renderers and
compare after every layer.

Two invariants carry real weight and deserve assertions in a debug build rather
than trust: that the background and foreground road rows together account for all
448 rows, and that every rendered foreground row writes all 640 pixels.
