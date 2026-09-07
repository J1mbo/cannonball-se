# Road and tile renderers: data study, architecture, correctness hazards

This document is the consolidated narrative for the road/tile renderer optimisation
(S0-S7, `hwroad.cpp`/`hwtiles.cpp`). It merges (and replaces) `STRATEGY.md`,
`OPTIMISATION_PLAN.md`, and the sprite-side static audits `ROAD_ANALYSIS.md` and
`TILES_ANALYSIS.md`. Those filenames, and any others named below, no longer exist in
the tree: they are referred to as provenance only, and are recoverable from git
history. For kernel-level build detail (files touched, exact macros,
per-stage tests) see `IMPLEMENTATION_SPEC.md`, which stays in place and is not
duplicated here. For the measured before/after numbers, see
`tools/sprite_bench/analysis/PERFORMANCE_MEASUREMENTS.md` (campaigns 1-2 for the
frame-budget split that started this work, and the "road/tiles optimisation result"
table for the outcome).

This work exists because of `PERFORMANCE_MEASUREMENTS.md` campaign 1 (part B) and
campaign 2: an in-game frame-budget split found road+tiles cost 2.20ms against the
sprite pass's 0.95ms (1.7x), which redirected the second half of the sprite-rework
project here.

---

## 1. The data study

Two independent studies (`OPTIMISATION_PLAN.md`, an isolated agent's cost model and
100-captured-frame instrumentation; and static audits of the pre-rewrite kernels in
`ROAD_ANALYSIS.md`/`TILES_ANALYSIS.md`) established the following facts about
`src/main/hwvideo/hwroad.cpp` and `hwtiles.cpp` before any of S1-S7 was built.

### 1.1 Method

`OPTIMISATION_PLAN.md`'s numbers come from: decoding the actual arcade road and tile
ROMs with a throwaway Python script mirroring `hwtiles::init`/`HWRoad::decode_road`;
and a private instrumented WSL build (`-Ofast -march=native`, unmodified renderers)
that snapshotted road RAM, tile RAM, text RAM, the scroll registers and the 573KB
destination buffer after every layer, across 100 frames sampled over ~5.5 minutes of
the attract cycle (53 driving, 4 logo, 3 best-outrunners frames). Hot loops were
separately cross-compiled with clang 18 (`-mcpu=cortex-a53 -Ofast`) to count
instructions and memory ops (representative, not exact — the Pi build uses GCC).
Nothing under `src/` or the ROMs was modified for this study. Target throughout:
Cortex-A53 @ 1.0GHz, one A53 core, shipping config (hi-res, widescreen off, 640x448
16-bit = 573KB destination), three other cores plus GPU sharing the same LPDDR2.

### 1.2 The headline: cost is paid at first touch of each cache line

An instrumented 100-frame capture accounted for every 64-byte line of the 573KB
destination:

| layer | lines touched | of which first touch |
|---|---|---|
| road background | 4,581 | 4,581 |
| tile background | 1,240 | 372 |
| tile foreground | 552 | 44 |
| road foreground | 4,379 | 3,962 |
| sprites | 2,147 | 0 |
| text | 284 | 0 |

Every one of the 8,960 lines is first-touched exactly once per frame. Because 573KB
does not fit the 512KB shared L2 (itself being streamed through by the NTSC threads'
15.8MB table), each first touch is a write-allocate line fill from DRAM. Road
background costs 58 cycles/line solo, 149 under load, against a store-issue floor of
~4 cycles/line (~0.02ms) — over 90% of that call is waiting for line fills. Initial
estimated split of the 2.51ms road+tiles+text budget: write-allocate fetches
1.0-1.2ms, road foreground pixel loop 0.6ms, the two tilemap scans 0.2-0.25ms, tile
pixel kernels 0.1ms, text 0.1-0.3ms. **This estimate was later revised** — see
section 1.4.

### 1.3 Road ROM structure: 17 fills, not 320 pixels

Road ROM `opr-10185.11` (32KB, decoded to 256 lines x 512px, values 0/1/2/3/7):
every line is distinct; **a line is at most 15 runs of constant value** (220 of 256
lines have exactly 15, none more). The visible window is 320 of the 512 source
pixels, everything outside `[0,0x200)` forced to value 3 (exterior). Per rendered
output row the two roads' run boundaries merge into a mean of **16.9 constant-colour
segments, never more than 29** (measured over 21,900 rendered rows). The pre-rewrite
renderer ran 320 iterations with four loads each to produce what is really a list of
~17 fills. A run-length table for the whole ROM is ~13KB and stays L1-resident,
against 512 bytes of `roads[]` streamed per road per row in the old kernel. This
finding became S6 (run-length road foreground), confirmed shipped in `run_tab`/
`run_first`/`fill_pairs`/`render_row_runs` in `hwroad.cpp`.

### 1.4 DC ZVA measured and killed — the bottleneck is loads, not stores

The plan's first-ranked item was `DC ZVA` line pre-allocation (zero a cache line
without a read, ahead of a full-line rewrite), verified with a microbenchmark before
any renderer code changed (`DCZID_EL0` = 0x4, 64-byte blocks permitted). Filling the
560KB buffer with a constant:

| strategy | solo | 3-core thrash |
|---|---|---|
| plain NEON store | 0.091 ms | 0.60 ms |
| `DC ZVA` then store | 0.192 ms | 1.32 ms |
| `DC ZVA` only (floor) | 0.091 ms | 0.65 ms |

The plain store already matches the zero-only floor — there is no write-allocate read
penalty for `DC ZVA` to remove; the A53 write-streams full lines already. Adding
`DC ZVA` writes each line twice and is 0.46x — slower. **The kill rule (abandon below
1.5x) fired: S2 and S4 (the `DC ZVA` stages) were dropped from the plan and
subsequently removed from the tree** (commit `ca7ed6d`). See section 4 for the full
"do not retry" record.

This also corrected the section 1.2 estimate: a flat fill of the whole 560KB buffer
is 0.09ms solo, so destination store bandwidth is *not* the dominant cost. The
in-game road cost is loads, per-pixel compute and branches — chiefly the two texture
loads per pixel-pair from the 256KB `roads[]` table (8x the L1D). This raised the
run-length road foreground (S6) to the headline change and lowered the expected value
of the tile-clip (S5, which mostly saves stores). The write-allocate line-accounting
in section 1.2 was a real observation but its cost was over-attributed; the fills are
cheap because the core streams them, so the real win is removing loads and branches.

### 1.5 Tile ROM structure: opaque rows and carry-free colour

Tile ROM (3 planes x 64KB -> 8,192 tiles): 6,313 distinct tiles, 737 all-zero; 31.2%
of all pixels transparent; of 65,536 tile rows, 24% all-transparent, **59% fully
opaque**, 17% mixed, 31% a single colour (transparency concentrated at tile edges).
In the rendered set the opaque share is higher still: 43% of background rows and 61%
of foreground rows are fully opaque, only 5%/10% mixed. Because `nPalette` is
`palette << 3` (`hwtiles.cpp`), adding a nibble never carries out of the low byte —
**a pixel's colour is a plain add with no table lookup**. An 8-pixel row therefore
maps to two 128-bit vectors (16 hi-res pixels, 2 rows) with ~12 vector ops and 4 wide
stores, instead of 32 scalar halfword stores. This became S7's NEON `tile_row_hires`
kernel, confirmed shipped.

The static audit of the pre-rewrite kernel (`TILES_ANALYSIS.md`) had already flagged
the underlying symptom independently: hi-res 2x2 tile replication used 4 separate
scalar 16-bit stores (`set_pixel_x4`) where fewer, wider stores would do — "exactly
the pattern the sprite work already found costly on this core." It also found only
13.7% of the 8,192-cell grid is ever visible (1,120 of 8,192 cells), which became
S1's visible-window scan.

### 1.6 Layer-overlap: the tile layers do invisible work

33% of tile-background pixels (11.8k of 36.2k) and **80% of tile-foreground pixels
(12.8k of 16.0k) land in rows that the road foreground later overwrites in full** —
that work, and the first-touch line fills it triggers, is invisible in the final
frame. A further 39% of tile-background pixels are covered by the foreground
tilemap; of the sky region (147k px, 51% of the buffer) 83% remains visible after
both tilemaps. Because the road foreground writes every pixel of its rows in all
four control modes, a per-row "will be overwritten by the road" mask is exact and
cheap to derive (224 entries from road RAM, which is frozen during
`prepare_frame`). This became S5's tile-clip mask, confirmed shipped as
`foreground_row_mask`. Getting the mask's derivation wrong is a real correctness
hazard — see section 3.

---

## 2. Architecture as built

The shipped renderer (`git log` on `src/main/hwvideo/`: `b95d4d0` S0 harness ->
`fb3d6d1` S3+S5 mask+clip -> `b06811a` S6 run-length road -> `027642b` S7 NEON tile
kernel -> `ca7ed6d` DC ZVA removed) carries the following symbols, confirmed present
in the current tree:

- **S1 — visible-window tilemap scan.** `render_tile_layer`/`render_text_layer` now
  precompute the contiguous `mx`/`my` (or column/row) ranges that can land on screen,
  and iterate only those cells instead of the full 8,192/2,048-cell grid. Bit-exact
  (four checksums + exhaustive cell-set test). Measured: tile bg 0.43 -> 0.33ms, tile
  fg 0.20 -> 0.12ms.
- **S3 + S5 — overwrite-mask derivation and tile-layer clip.** A 224-entry per-line
  mask is derived from `ramBuff`/`road_control` using precisely the skip logic of
  `render_foreground` (section 3 below), then used to skip whole tiles or rows the
  road foreground will overwrite. Bit-exact; the `CB_RENDER_CHECKS` invariant
  assertion never fired across 600 attract frames in all four control modes.
- **S6 — run-length road foreground.** `run_tab`/`run_first`/`fill_pairs`/
  `render_row_runs` in `hwroad.cpp` replace the 320-iteration-with-four-loads inner
  loop with a per-row merge of each road's run list (section 1.3) into ≤29 segments,
  filled with wide stores. Bit-exact (four checksums, exhaustive sweeps on x86 scalar
  and real A53 NEON). Per-row speedup on the real ROM: 4.4x. In-game road_fg measured
  0.88 -> ~0.26ms (~3x).
- **S7 — NEON tile-row kernel with scalar fallback.** `tile_row_hires` in
  `hwtiles.cpp`, gated `CB_HAVE_NEON`, implements the carry-free widen-and-add
  colour computation from section 1.5. Bit-exact (four checksums plus an exhaustive
  per-word unit test, 8.55M invocations, on x86 scalar and real A53 NEON).
- **S0 — build and test harness.** `_ref` twins, a `RoadTilesTestAccess` friend, and
  a `-dumpram` capture format, mirroring the sprite harness; confirmed present in
  `tools/road_tiles/test/`.

All five landed sections retain a scalar fallback for non-NEON/MSVC builds. See
`IMPLEMENTATION_SPEC.md` for the exact macros, files touched and per-stage tests
(S0 sec. build/harness, S1 sec. visible-window scan, S3/S5 sec. mask+clip, S6 sec.
run-length road, S7 sec. NEON tile kernel).

**Measured result, 60fps four-thread (road+tiles+text):**

| phase | baseline | after |
|---|---|---|
| road background | 0.68 | ~0.66 (untouched) |
| tile background | 0.43 | ~0.28 |
| tile foreground | 0.20 | ~0.075 |
| road foreground | 0.88 | ~0.26 |
| text | 0.31 | ~0.08 |
| **sum** | **~2.50 ms** | **~1.35 ms** |

About 1.1ms of a 16.7ms frame recovered. Road background (the streaming sky fill) is
the largest untouched item — `DC ZVA` does not help it (killed, section 1.4/4) and
it is bandwidth-bound, so it is left as is.

---

## 3. Correctness hazards

### 3.1 The two road passes are complementary by row, not stacked (verbatim from `STRATEGY.md`, maintainer-verified 2026-09-04)

The names mislead. The two passes are complementary by row, not stacked:

- `render_background` (`hwroad.cpp:521`) paints **only the sky rows** — the rows
  above the horizon, where a road has the low-priority bit `0x800` set
  (`hwroad.cpp:535` onward). It skips the road rows.
- `render_foreground` (`hwroad.cpp:595`) paints **only the road rows**. It skips any
  row where both roads are low priority (`hwroad.cpp:631`).

Between them run the two tile layers. So:

- On a **sky row**, the order is road-background sky colour, then tiles on top
  (mountains, scenery). Nothing overwrites the tiles. This is the "road is the
  background" case.
- On a **road row**, the tile layers draw first, then `render_foreground` repaints
  every pixel of that row opaquely, **including the off-road margin** (texture value
  3 still writes `color_table[pix0]`, `hwroad.cpp:740-745`; there is no transparency
  test). So no tile pixel in a road row survives. The overwritten tiles are the
  grass/ground/scenery under the road region.

This means the overdraw is real and the S3/S5 mask+clip design is valid, but the
overwrite mask is **not** simply "the bottom N rows". It must be derived per frame
from two conditions, and getting either wrong deletes visible tiles:

1. The per-row skip in `render_foreground` (both roads low priority ->
   `hwroad.cpp:631`), which decides which rows the foreground actually paints.
2. The horizon guard at `video.cpp:208`: when `config.engine.fix_bugs &&
   oroad.horizon_base == ORoad::HORIZON_OFF`, the foreground pass does not run at
   all, so **no** tile row is dead that frame.

The mask must be built from exactly those two conditions, and the debug assertion
(S3, `CB_RENDER_CHECKS`) that the mask equals the rows the foreground actually
writes is what catches a mismatch. `IMPLEMENTATION_SPEC.md`'s own "structural facts
easy to get wrong" preamble additionally records the exact per-control-mode
predicate (modes 0/3: exactly complementary by construction; modes 1/2: cover every
row between them, with a row solid in exactly one road written by both, foreground
last) — the mask must follow the foreground's own predicate, never the background's.

### 3.2 Hi-res rows come in pairs that share a fate

For odd `y`, `yy = y >> 1` is the same as for `y - 1`; `data0`/`data1` are re-read
from the same road RAM words, and the interpolation block only rewrites them when
their `0x800` bit is already clear. So the skip decision for rows `2yy` and `2yy+1`
is always identical, and the odd row's `color_table` is the one built on the even
row of the same pair. Any rewrite of the per-row setup code must keep it byte-for-
byte equivalent so this state carry-over is preserved — this is exactly the kind of
subtle state dependency that a naive per-row refactor would break silently (output
would still look plausible, just wrong on odd rows).

### 3.3 Other hazards noted during the static audits (pre-rewrite kernel, now fixed or superseded)

- A stale comment in `render_background_lores` claimed a two-line fill; the lores
  loop actually fills one row. Lores is dead code in the shipping (hires)
  configuration, so this had no runtime effect, but would have misled anyone
  re-enabling it.
- `HWRoad::x_offset` is set to 0 once at init and never written again, yet is added
  into every `hpos` calculation in the hot loop — permanent dead state, harmless but
  worth knowing it is not the (unrelated) `config.video.x_offset` UI setting.
- The road foreground's `roads[]` texture table (256KB, 8x the L1D) is addressed
  with data-dependent, non-sequential row selection across scanlines — not fixable
  by store-width changes alone, and not attempted here; S6's run-length approach
  sidesteps the problem by reading each road line's run boundaries once per row
  instead of streaming the raw texture.
- `nPaletteOffset` in the tile kernels is a dead parameter — computed via a
  non-trivial `ColourOff`/`TILEMAP_COLOUR_OFFSET` expression at every call site but
  never referenced in any of the four `render8x8_tile_mask*` bodies. Maintainer-
  resolved 2026-09-04: no known colour/palette problems in the shipped game, so this
  is vestigial code safe to delete for clarity, not a defect to chase.

---

## 4. Rejected / do not retry: `DC ZVA` line pre-allocation

**Status: built, measured, found useless, removed.** Commit `ca7ed6d` ("hwarch:
remove dead DC ZVA helpers").

The plan's highest-ranked item (P2 in `OPTIMISATION_PLAN.md`, S2/S4 in
`IMPLEMENTATION_SPEC.md`) was to issue `DC ZVA` (zero a 64-byte line without reading
it first) ahead of the road background fill and the road foreground's per-row
rewrite, on the theory that the A53's write-allocate line fills were the dominant
cost (section 1.2's initial estimate: 1.0-1.2ms of the 2.51ms road+tiles+text
budget). This was explicitly gated behind a microbenchmark and a kill rule before any
renderer code was touched, precisely so a wrong prediction would not cost an
implementation effort:

> Kill rule: abandon P2 if `DCZID_EL0` bit 4 is set (ZVA prohibited); the loaded fill
> microbenchmark improves by less than 1.5x; or, in-game, `prof_ns_road_bg` under
> 4-thread load stays above 0.40ms (from 0.68).

The microbenchmark result (section 1.4): plain NEON store already matches the
zero-only floor (0.091ms solo both ways), so there is no write-allocate read penalty
to remove — the A53 write-streams full lines regardless. `DC ZVA` then store is
*slower* (0.192ms solo, 0.46x under contention) because it writes each line twice.
**The kill rule fired before implementation began**, and S2/S4 were dropped from the
plan; the helpers (`cb_dczva_probe`, `cb_zva_lines` in `hwarch.hpp`) were built as
part of S0's shared harness for completeness, then removed once confirmed dead
weight.

**Why this record has value.** It is easy to re-propose `DC ZVA` again from first
principles — it is a real, documented AArch64 feature, and "skip the write-allocate
read" sounds obviously correct for a large streaming fill. The actual constraint on
this specific core, for this specific workload, is that the A53 already streams full
lines on a plain store with no read penalty to remove; the measured floor is set by
line-fill latency and issue width, not by a removable read. Do not re-attempt `DC
ZVA` for the road background or foreground fills without a fresh microbenchmark on
the target hardware — the 2026-09-04 result above is the reason it is absent from
the shipped code, not an oversight.

---

## 5. Test harness and correctness gate

Standalone driver: `tools/road_tiles/test/` (mirrors the sprite harness's shape —
`_ref` reference twins kept in the tree, a `RoadTilesTestAccess` friend, `golden/`
checked-in hashes). See `IMPLEMENTATION_SPEC.md` S0 for the exact harness design
(files touched, macro scheme, dependency order of S0-S7).

Whole-frame correctness gate: `tools/road_tiles/analysis/GOLDEN_CHECKSUMS.md` (kept
in place, not merged here) records the bit-exact FNV-1a whole-frame checksums that
every road/tile/sprite change in this effort was checked against — "every checksum
must match; a mismatch blocks merge." Its one open caveat: the recorded checksums are
attract-mode only; a gameplay-input baseline is still needed.

Two invariants carry real weight and are checked with debug-build assertions rather
than trusted: the background and foreground road rows together account for all 448
rows every frame, and every rendered foreground row writes all 640 pixels (both are
what make the S5 tile clip and the (now-removed) S2/S4 `DC ZVA` scheme safe in the
first place — see section 3.1).
