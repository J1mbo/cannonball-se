# REVIEW: SIMD strategies for `snes_ntsc_blit_hires` on Cortex-A53

Technical review of IDEAS_A.md and IDEAS_B.md against BRIEF.md and ALGORITHM.md,
revised with the working-set sweep supplied after the proposals were written
(section 1.2). Cycles are at 1 GHz. "Benchmark" means the 256-colour, single-thread
synthetic case; "real frames" means game frames with large flat sky/road areas and a
few hundred colours.

> **Read the addendum at the end first.** A second data batch (page-spread runs,
> and confirmation that huge pages are unavailable) supersedes sections 1.2-1.3,
> every HP entry, and the ranges in sections 4-5: the micro-TLB term is ~16 cycles
> (not <= 10), the compact table (CT) returns to the shortlist, and the final
> ranking is AO, RI, CT, RK. The body is kept as written for traceability.

**Verdict in one paragraph.** Both authors converge on the right kernel (lanes =
adjacent output pixels, packed 32-bit words summed unchanged) and both build their
memory story on a micro-TLB thrash that the sweep now shows is at most a minor term.
The dominant stall is L1 capacity: the per-row table footprint changes with the
phase every row and is re-fetched from L2. That promotes the one proposal that
shares a phase's lines across rows (B4, row interleave) and demotes both
compact-table/huge-page proposals (A2, B2) to footprint tools rather than speed
levers. The kernel alone should give 1.8-2.6x on the benchmark; kernel plus row
interleave 2.3-3.1x; the run-length idea adds nothing to the benchmark and plausibly
1.3-2x on real frames. Only the packed-word designs are exact by construction; the
int8 designs are exact under a range condition that can be proven from the table in
an hour.

---

## 1. Shared framing

### 1.1 Arithmetic of the original three measurements

| quantity | value | check |
|---|---|---|
| outputs per frame | 749 x 448 = 335,552 | ALGORITHM 4.5 |
| 256 colours, 1 thread | 22.7 ms -> 67.6 cycles/output | both authors, correct |
| 65536 colours | 92.2 ms -> 274.8 cycles/output | correct |
| 2 threads | 12.0 ms -> 71.5 cycles/output per core, 1.89x | not bandwidth bound, as BRIEF says |
| issue floor assumed | ~42 instr/output (ALGORITHM 5) -> 21-24 cycles | A 24, B 21 |
| implied stall | 44-47 cycles (65-70 %) | **wrong: see 1.2** |

The 65536-colour figure fits ~1.43 first-touch lines per output x (DRAM ~120-150 +
page walk ~30) + issue, so B's inference that memory-level parallelism is ~1 (every
miss fully exposed) is sound and remains the most important hardware fact in the study.

### 1.2 The working-set sweep (new data, unmodified scalar, contiguous colour values assumed)

| N colours | ms | cycles/output | step | table set | pages | regime |
|---|---|---|---|---|---|---|
| 1 | 10.98 | 32.7 | - | 512 B | 1 | pure issue + in-order losses |
| 16 | 13.01 | 38.8 | +6.1 | 8 KB | 2 | L1 and micro-TLB both fit |
| 64 | 19.03 | 56.7 | +17.9 | 32 KB | 8 | 3-phase set = L1 size; 8 + in/out/stack pages ~ 11 vs 10 micro-TLB entries |
| 256 | 22.20 | 66.2 | +9.5 | 128 KB | 32 | L1 thrashed per row, L2 hits; micro-TLB fully thrashed |
| 1024 | 47.79 | 142.4 | +76 | 512 KB | 128 | = L2 size, DRAM begins |
| 4096 | 83.95 | 250.2 | +108 | 2 MB | 512 | > L2; = main TLB size |
| 65536 | 93.53 | 278.7 | +28 | 32 MB | 8192 | page walks add only ~20 cycles per new line |

Three things follow.

1. **The compiled scalar code costs 33 cycles/output, not 21-24.** About 10 cycles
   of in-order scheduling loss (load-use latency, the 11-add chain) sit inside what
   both authors called "issue". The stall at N = 256 is 33.5 cycles, about half. This
   is good news for any rewrite: a vector kernel replaces 33 cycles of issue, not 22.
2. **The micro-TLB term is bounded at ~10 cycles and is probably smaller.** Under the
   coordinator's page counts the 64 -> 256 step goes from a micro-TLB that roughly
   fits (11 pages live against 10 entries: ~1 refill per output under LRU) to one that
   thrashes on every load (~12 refills per output), yet costs only 9.5 cycles. Even if
   the whole 16 -> 64 step were also TLB (it cannot be: 128 KB >> 32 KB must produce
   ~0.8 L2 fills per output at N = 256, i.e. >= 12 cycles), the term would be <= 18.
   If instead the sweep's colour *values* were random over 0..65535, N = 16 already
   spans 16 pages and thrashes, and the bound tightens to <= 6. Either reading kills
   the 25-45 cycle figure both authors relied on.
3. **The big step is at the L1 boundary, and the cliff is at L2.** 16 -> 64 (+18) is
   where the three-phase set reaches 32 KB; 256 -> 1024 (+76) is where it reaches
   512 KB. What matters is *bytes of distinct table lines per frame*, which no
   compacting of pages changes (N colours x ~504 B whether they sit in a 32 MiB
   master or a 1 MB slot table).

Revised decomposition at N = 256: 33 issue-as-compiled, ~6 "many distinct lines" L1
overhead of unknown mechanism (present already at N = 16), ~17-27 L1-miss-to-L2
latency, 0-10 micro-TLB. Proposals most affected: A2 and B2 (their headline term is
gone), B4 (its term is the dominant one), A1 and B1 (unchanged range, different
composition: they must pair with B4, not with huge pages).

### 1.3 The measurement that settles the residual ambiguity

The 16 -> 64 step crosses the L1 boundary and the micro-TLB boundary at once. One
pair of runs of the unmodified blitter separates them (a few hours, only the frame
generator changes): N = 48 with values 0..47 (24 KB, 6 pages: both fit) versus N = 48
with 48 values spread over 48 distinct pages (24 KB, L1 fits, micro-TLB thrashes).
Equal times (~50 cycles by interpolation) mean TLB ~ 0; the random-page run at ~66
means the whole 16 -> 64 step is TLB. Then, since THP is available in `madvise` mode,
`MADV_HUGEPAGE` on a 2 MiB-aligned table at N = 256, confirming `AnonHugePages` in
`/proc/self/smaps`, gives the huge-page gain directly (prediction: <= 6 cycles). If
the PMU is exposed, `perf stat -e r05,r03,r17` (L1D_TLB_REFILL, L1D_CACHE_REFILL,
L2D_CACHE_REFILL) per output confirms the counts.

---

## 2. Audit of the ten proposals

Counts re-derived from ALGORITHM 4.3/4.4: slot `s` of chunk `c` starts at output
`7c+s`; per chunk the 17 contributors add 27 + 42 + 15 = 84 words = 12 x 7.

### 2.1 Cost model, counts, exactness, portability, effort

| # | proposal | counts vs tap table | bit-exact | portability claim | effort (theirs -> mine) |
|---|---|---|---|---|---|
| A1 | phase-split table + 7-lane chunk vectors | 28 loads, 28 adds, 8 masks per chunk verified slot by slot; 9.1 NEON ops/output = 18.3 c | exact by construction; over-reads up to 3 words past word 125 (guard or split table needed); lane-7 write past `out[748]` | honest; SSE2 AND masks replace TBL | 5-8 -> 6-9 d |
| A2 | huge pages, then dense hot-colour cache | page counts correct, but the dense cache in 4 KB pages (126 pages, 12 live per output) does not help the micro-TLB at all, and the sweep shows the micro-TLB term is minor anyway; remap costs 16 KB of L1 and a dependent load | exact | honest | 0.5 / 2 -> 3 d (two threads allocate concurrently: unaddressed) |
| A3 | TBL gather, lanes = 16 chunks, per-band int8 tables | 2.25 TBL4 + 2.25 `vaddw`/output correct; **operand loads understated ~2x** (2.25 x 64 B = 9 Q loads/output) | conditional (int8 range, field sums 0..1023, re-implemented clamp) | honest: needs a per-ISA kernel, against the brief's preference | 10-15 -> 15-20 d |
| A4 | run kernels: flat constant, chunk kernel, pair kernel | 6 loads + 6 adds per uniform chunk correct; `C(v)` phase-independent verified via ALGORITHM 3.6 (each lores tap set sums to `rgb` in every phase, so flat hires output = `2R'`); 18-pixel condition correct (17 suffice) | exact incl. both bias taps (virtual black pixels count as colour 0) | honest | 3-4 d |
| A5 | int8 planar, 3 x int16x8 accumulators | 51 loads, 30 TBL, 160 NEON cycles/chunk = 23/output correct; 1.14 vs 1.43 first-touch lines correct | conditional, as A3 | honest (SSE2 ~1.5x ops) | 6-8 -> 8-10 d |
| B1 | H4-EXT, 4-aligned grid, table unchanged | 102 loads, 102 adds, 36 `ext` per 28 outputs verified (6 pixels per lane offset; 4/4/4/5 vectors; 1/2/1/2 partials); 7.93 ops = 15.9 c | exact, no over-reads, edges exact via the `t = -4` prologue | honest and best of the ten (`ext` vs zero = `_mm_slli/srli_si128`) | 4-6 -> 6-8 d |
| B2 | compact phase-major slot table + 256 KB offset map | 11 pages / 672 lines correct; "~0 micro-TLB misses" is ~1/output at 256 colours and rises above ~300 colours per row; but per-row *lines* are unchanged (672 new lines per row either way), so the sweep leaves it little to remove; the map adds 0.86 dependent loads/output and 16 KB of L1 | exact | honest | 2-3 -> 3-4 d (thread safety, per-frame cap; a 504 B slot copy from DRAM is ~500-1000 cycles, not 200) |
| B3 | chunk kernel `CK` (19 words) + pair kernel `PK` | correct; `floor((L-5)/6)` flat chunks per run correct; 5.5 loads/chunk correct | exact | honest | 3-4 d |
| B4 | same-phase row-triplet interleave | rows `r, r+3` share phase: correct; up-to-3x reduction of per-row compulsory L1 misses: correct, and the sweep says that is the dominant term | exact (compute the phase, do not increment) | pure C | 0.5-2 d |
| B5 | V3, rows in lanes, phase-interleaved table | 12 lane loads/output on non-uniform columns correct; B's own rejection is right | exact | honest: SSE4.1 only; RVV indexed loads | 4-6 d |

### 2.2 Most likely failure and revised ranges

| # | most likely reason it misses its number | their claim (benchmark) | revised benchmark | revised real frames |
|---|---|---|---|---|
| A1 | huge pages buy ~nothing (1.2); prefetch overlaps only 3-4 L2 fills; the intrinsics kernel loses 2-4 cycles to in-order scheduling as the scalar did | 2.0-2.5x with S2 | **1.8-2.6x alone**; 2.3-3.1x with B4 | 2.0-2.8x alone |
| A2 | its 25-45 cycle term is <= 10 | 1.5-1.9x | **1.0-1.15x** (huge pages); 1.0-1.1x (dense cache) | same |
| A3 | every 16-row band of the benchmark has 256 colours: fallback everywhere; TBL4 at 8 cycles; operand loads | 2.7x on <= 64-colour bands | **1.0x** | sky bands only, <= 1.3x over the kernel |
| A4 | the 50/20/30 flat/uniform/general prior is unsupported | 0.97x; ~5x real | 0.95-1.0x | +1.3-2.0x on the kernel (A's +2.5x is optimistic) |
| A5 | NEON bound 23 > AO's 16-18, and with the memory term now known to be ~27 rather than ~45 the kernel bound is what matters; "32 KB fits L1" is exactly L1 size | 2.0-2.3x | 1.6-2.2x alone | 1.9-2.6x |
| B1 | residual stall 2-6 assumed; 0.8 L2 fills/output at 15-20 cycles with MLP 2-3 is 5-12; scheduling loss 2-4 | 2.8-3.7x | **1.8-2.6x alone** | 2.0-2.8x |
| B2 | pages are not the problem; lines are, and it does not change them; also cannot move the 256 -> 1024 cliff (same bytes) | 1.4-1.7x; 3.4-4x with B1 | **1.0-1.15x**; +1.0-1.1x on AO | similar |
| B3 | as A4 | 1.0x; 4-4.8x real | 1.0x | as A4 |
| B4 | L1-to-L2 term smaller than 3x-reducible (some misses are within-row conflict misses at 43 KB per phase, not cross-row) | 1.25-1.35x alone; +1.15x | **1.2-1.4x alone; +1.2-1.3x on AO**; also the only lever that lifts the 1024-colour cliff (3x fewer L2/DRAM line fetches) | larger where rows share colours |
| B5 | single-lane loads serialise on the destination register; 12 LSU ops/output on detailed rows | 1.7-3x | 0.8-1.5x, dominated | worse on road texture |

**Exactness notes that apply across the table.** (i) Every packed-word design sums
exactly the multiset of words scalar sums, in a different order; modulo-2^32 addition
is associative, the clamp is the scalar bit sequence per lane, so 254/1 saturation
and the two bias taps are reproduced. Documented deviations: B1 assumes `Alevel <= 255`;
A1 writes `out[749]` on the last row of a half. (ii) The channel-separated designs
(A3, A5) are exact iff every 12-tap field sum stays in 0..1023; outside that range
scalar corrupts a neighbouring field and no int16 emulation matches it. This can be
*proven*: at init, take per (phase, alignment, tap, channel) the min and max
contribution over all 65536 entries, sum the 12 per-position extrema of the tap
table, and check the bound lies in -512..511; the same pass checks the int8 range.
One hour, and it turns "verified by whole-frame comparison" into a guarantee.
(iii) Dropped pixels 638-639: a fixed-4-chunk unroll must stop at chunk 105 and feed
six black pixels, never read `in[638..639]`; all proposals say so.

---

## 3. Merge: distinct approaches

| id | approach | absorbs | composes with | conflicts with |
|---|---|---|---|---|
| **AO** | adjacent-outputs packed-word kernel: B1's 4-aligned grid with `ext` against zero (no table change, no over-reads); A1's chunk-aligned form is the same idea with 12.5 % lane waste and a guard requirement | A1, B1 | RI, RK, HP, CT | P8, TG (alternative kernels), RL |
| **RI** | same-phase row interleave, `(r, r+3, r+6)` chunk by chunk | B4 | everything; needs register-allocation care inside AO | TG's band order (moot) |
| **RK** | run-length kernels: flat constant `C(v)`, chunk kernel `CK`, pair kernel `PK`, cached per colour | A4, B3 | requires AO (or P8) accumulators; RI | none |
| **HP** | `MADV_HUGEPAGE` on a 2 MiB-aligned master; no blitter code | A2(a) | everything | none |
| **CT** | compact phase-major slot table with remap and per-frame cap; A1's phase-split master and B2's phase-major master are the same relayout; A2's dense cache and B2's slot planes are the same structure | A2(b), B2, A1's table change | AO, RI, RK, HP | none; do not pad slots to 216 B |
| **P8** | int8 planar table, int16 lanes | A5 | RI, RK, HP, CT | AO |
| **TG** | TBL gather per <= 64-colour band | A3 | needs P8's master; falls back to AO/P8 | AO/P8 as band kernel |
| **RL** | rows in lanes | B5 | - | rejected |

AO and RI stack multiplicatively (issue term vs L2-latency term). HP and CT attack the
same, now-minor TLB term and should be counted once. RK multiplies on real frames only.

---

## 4. Ranking on the primary target

| rank | approach | standalone | marginal on AO | P | days | (gain-1) x P / days |
|---|---|---|---|---|---|---|
| 1 | AO | 2.2x (1.8-2.6) | - | 0.8 | 7 | 0.14; the only route to >= 2x |
| 2 | RI | 1.3x (1.2-1.4) | 1.2-1.3x | 0.75 | 1 (scalar) + 1.5 (in AO) | 0.22 standalone; also the cliff lever |
| 3 | HP | 1.07x (1.0-1.15) | 1.0-1.1x | 0.5 | 0.5 | 0.07; half-day test, keep if it measures >= 3 cycles |
| 4 | RK | 1.0x | real frames 1.3-2.0x | 0.6 | 4 | 0 on the benchmark; 0.05-0.15 real |
| 5 | CT | 1.07x (1.0-1.15) | 1.0-1.1x | 0.6 | 3.5 | 0.01; worth building only for footprint (lazy generation shrinks 32 MiB to ~1.3 MB) |
| 6 | P8 | 1.9x (1.6-2.2) | not additive | 0.5 | 9 | 0.05 |
| 7 | TG | 1.0x | <= 1.3x sky bands | 0.4 | 17 | ~0.01 |
| 8 | RL | <= 1.5x, dominated | - | 0.4 | 5 | 0 |

Expected stack on the benchmark: AO 1.8-2.6x; AO + RI (+ HP if it measures) **2.3-3.1x**,
i.e. 21-29 cycles/output, 7.2-9.7 ms per full frame single-threaded, 3.8-5.1 ms per half
frame on two threads, inside the 8 ms budget. B's 15-18 cycles (~4x) needed the TLB
story and a 1-cycle Q load; neither survives. Large palettes: AO's early issue plus
RI's 3x fewer fetches should take N = 1024 from 142 to ~60-80 and N = 65536 from 279
to ~120-160 cycles/output: no new cliff, and the old one is softened.

**Secondary targets.** On x86 (large TLBs, single-cycle 128-bit ops) AO is
memory-bound and RI is the lever that matters; AO ports on plain SSE2
(`_mm_loadu_si128`, `_mm_add_epi32`, `_mm_slli_si128`). On RVV 1.0 AO maps to
`vle32`/`vadd`/`vslideup`; RL becomes viable via `vluxei32` but is still unnecessary;
TG via `vrgather` is the one idea genuinely more attractive there than on A53.

---

## 5. Shortlist and kill experiments

**1. AO (build first; everything else attaches to it).**
Kill experiment, one day: hand-write B1's unrolled 28-output body with intrinsics,
table unchanged, no edge handling, on (a) a 16-colour row (L1-resident, compare
against the sweep's 38.8) and (b) the 256-colour row, each repeated 448 times; cycle
counter, sums checked against scalar. Go: (a) <= 22 and (b) <= 34 (2x met by the
kernel alone). Amber: (b) 34-45 with (a) <= 22: kernel right, memory-bound, proceed
only with RI. No-go: (a) > 28: the 2-cycle-per-Q-op model is wrong and, since P8 and
TG have more NEON ops per output, nothing vector reaches 2x; the project would then
rest on RI plus RK. Dependency: none.

**2. RI, with HP as a half-day rider.**
Kill experiment, two hours: reorder the scalar row loop into `(r, r+3, r+6)`
chunk-interleaved triplets, phase computed per row, and rerun the sweep at N = 64,
256, 1024. Go if N = 256 drops >= 8 cycles/output and N = 1024 drops >= 30; then fold
RI into AO's register allocation (7 accumulators x 3 rows will not fit 32 registers,
so interleave at chunk granularity with one row's accumulators live at a time).
Same afternoon: `MADV_HUGEPAGE` A/B at N = 256 and the 48-colour pair of 1.3; keep HP
only if it measures >= 3 cycles. Dependency: independent of AO for the experiments;
integration follows AO.

**3. RK (real frames only).**
Kill experiment, two hours: dump 10-20 in-game 16-bit input frames from several
stages and count, at the blitter's alignment `j = 6c+2..6c+7`: uniform 6-pixel
chunks, 17-pixel same-colour supports, uniform even/odd slot pairs. Go if flat
chunks >= 30 % (build `CK` and `C(v)`) or uniform pairs >= 80 % (build `PK` first: if
any layer of the hires frame is pixel-doubled, `PK` halves loads and adds of the
general path in that layer unconditionally). Dependency: AO's accumulators.

**4 (conditional). CT**, only for memory footprint: if the owner wants the 32 MiB
table gone, CT with lazy `gen_kernel` + `correct_errors` per new colour is the
route, and it costs ~nothing in speed once the TLB story is dropped. **P8** only if
item 1 lands amber and item 2 shows line traffic still binding, and only after the
range proof of 2.2(ii) passes.

**Not pursued, and why.**
- **RL, rows in lanes (B5).** The words the three lanes need live in three unrelated
  512-byte entries unless the column is vertically uniform, so on every detailed row
  it is 12 single-lane loads per output (scalar's count) that serialise on their
  destination register; the phase-interleaved table (12 B tap stride, 42 MiB padded)
  buys locality that RI gets for free with no table change; SSE2 has no lane insert.
  It cannot beat AO on any content and loses on road texture. On this core rows are
  a *cache-locality* axis (RI), not a lane axis.
- **TG (A3).** Nothing on the benchmark; operand loads understated 2x; per-ISA kernel;
  15-20 days; its habitat (flat sky bands) is where RK wins for a tenth of the effort.
- **A2's dense cache and B2's slot planes as speed levers**: the term they attack is
  <= 10 cycles and they do not reduce lines per row or bytes per frame.
- **B2's 216 B padding, A1's 192 B padding**: pages/bytes for cheap `ext`/TBL ops.
- **"All rows of one phase back to back"**: works only below ~190 colours; the
  triplet interleave dominates it.

---

## 6. Open questions for the project owner

1. **Colour placement in the sweep.** Were the N values contiguous (0..N-1) or random
   over 0..65535? The page-count reading in 1.2 assumes contiguous. Either way the
   TLB term is minor, but the 48-colour pair in 1.3 is the cheap confirmation, and it
   also tells whether the BRIEF's original 256-colour case was already the
   page-friendly variant.
2. **Real-frame capture and pixel doubling.** Can the renderer dump 16-bit input
   frames plus phase for 10-20 frames? Needed for RK's go/no-go, for real-frame
   numbers, and for the bit-exact harness. Are hires frames pixel-doubled in any
   layer (even/odd pairs identical)? A yes makes the pair kernel a near-unconditional
   1.5-1.8x on the general path for real frames.
3. **Colours per row on real frames.** The sweep says scalar already runs at ~50-57
   cycles/output when a three-row window has <= 64 colours; if real rows are that
   sparse the real-frame baseline is faster than the benchmark and RI/RK matter more
   than any memory lever. The dump in (2) answers this too.
4. **Table format and footprint.** Is shrinking the table to ~1.3 MB via lazy
   generation (CT) wanted for its own sake, given ~100 MB free? It is a footprint
   decision now, not a speed one. If yes: is a serial pre-scan per frame (~0.5-1 ms)
   or a per-thread compact table acceptable for the two concurrent halves?
5. **Huge pages.** THP `madvise` is available; is a 2 MiB-aligned allocation with
   `madvise` acceptable in the build, and does `AnonHugePages` confirm it on the
   fragmented 512 MB system? Only worth asking if the half-day A/B shows >= 3 cycles.
6. **PMU access** (`perf` hardware events) on the target, for counter confirmation.
7. **Scoping details.** Wide-screen `in_width` (808?) for the unroll remainder;
   tolerance of writes to `out[749]` on the last row of a half (B1 needs none);
   confirmation that the bottom-half call is seeded with `(phase + 2) mod 3`
   (ALGORITHM 7.12) so the full-frame harness compares like with like.

---

## Addendum, 2026-09-03: second data batch (page-spread experiment; no THP)

Two corrections to the text above. First, the batch-1 sweep in 1.2 used *random*
colour values: N = 16/64/256 were 16/64/256 pages, not 2/8/32, so the "micro-TLB
<= 10 cycles" bound in 1.2 and 1.3 is withdrawn. Second, there is no transparent
huge page support on the target (`madvise` fails, no sysfs entry): HP is dead, and a
compact user-space table is the only way to reduce live pages.

### A.1 What the page-spread data says

Cycles/output; pages = table pages + 2 (input row, output row); refills = 12 loads x
P(miss) for random access over P pages against a 10-entry LRU micro-TLB,
P(miss) = max(0, P-10)/P.

| N | random: cycles (pages) | contiguous: cycles (pages) | delta | refills/output, random vs contiguous | implied cost per refill |
|---|---|---|---|---|---|
| 1 | 32.8 (3) | - | - | 0 | - |
| 16 | 38.7 (18) | - | +6.0 vs N = 1 | 5.3 vs - | ~1.1 |
| 64 | 55.4 (66) | 39.6 (10) | 15.8 | 10.2 vs 0 | ~1.5 |
| 256 | 65.9 (258) | 60.5 (34) | 5.4 | 11.5 vs 8.5 | ~1.8 |
| 1024 | 144.5 (1026) | 113.0 (130) | 31.5 | 11.9 vs 11.1 | not micro-TLB: main-TLB walks (1024 > 512 entries) and L2 set conflicts |

The model is crude but the three small-N rows fit **~1-2 cycles per micro-TLB
refill**, the number both authors needed and guessed at 2-4. The robust bound is the
N = 64 pair: with the same 32 KB of lines, going from 10 to 66 live pages costs 16
cycles, so a fully thrashing micro-TLB costs 12-18 cycles per output in the scalar
code. I read the N = 256 pair differently from the coordinator: both layouts are far
past 10 pages, so the 8 % gap is the difference between a 71 % and a 96 % miss rate,
not evidence that the TLB term is spent. Revised decomposition at the benchmark
(N = 256 random): **33 issue as compiled, ~16 micro-TLB, ~16 L1-miss-to-L2 (0.8
fills/output x ~20), ~1 other.** Two terms of equal size, needing two different
levers: page compaction for the first, cross-row line sharing (RI) for the second.
Neither author had this split; A had 25-45 TLB, B had "lines and very probably
TLB", and both memory proposals were sized for a TLB term that is real but half
what A assumed.

### A.2 Consequences for the memory-side proposals

- **Page compaction pays only below ~10 live pages, and two of those are not the
  table.** A phase-major compact table (168 B per colour per phase) holds 24 colours
  per page: up to ~190 colours per row fit in 8 table pages and remove the term;
  256 colours need 11 pages (14 live with input, output and the remap page) and cut
  it to ~3.5 refills, ~5 cycles. The remap costs a page, 0.86 dependent loads per
  output and 256 lines (16 KB) of L1; hence question 5 (renderer emits slot indices)
  is now important, not optional.
- **AO cuts the TLB term by itself**: 4 loads/output give ~3.8 refills (~5-6 cycles)
  with no table change. CT's marginal value on AO is therefore ~4 cycles on the
  benchmark, the full ~6 on real rows with < 190 colours, and large at the cliff: the
  31-cycle N = 1024 random/contiguous gap is placement, and CT makes every frame's
  placement contiguous whatever its colour values.
- **CT's benefit is visible today without code**: N = 64 contiguous runs at 39.6,
  essentially the N = 16 figure, so scalar on a <= 64-colour row goes from ~55 to
  ~40 (1.4x) from compaction alone. A's dense cache and B's slot table were right
  in mechanism and wrong only in magnitude at 256 colours.
- **RI is unaffected** and attacks the other 16 cycles; HP is removed.

### A.3 Revised ranges

| stack | benchmark | notes |
|---|---|---|
| scalar + CT | 1.2x at 256 colours; 1.4x on <= 64-colour rows | supersedes every HP figure above |
| AO alone | **1.7-2.5x** | 18-22 NEON + 3-6 TLB + 6-12 lines partly hidden |
| AO + RI | **2.0-2.9x** | lines -> 2-5 |
| AO + RI + CT | **2.2-3.1x** | TLB -> 1-2; N = 1024 from 144 to ~50-70 cycles/output |
| + RK on real frames | 3.5-6x | unchanged |

Ranking becomes **AO, RI, CT, RK, P8, TG, RL**. CT's P rises to 0.7 (its term is now
measured), effort stays 3-4 days including the two-thread allocation and the
per-frame cap; on the benchmark its EV per day is still the lowest of the three, but
it is the only page lever left, it is worth 1.4x on sparse real rows even in scalar,
and it is the cliff insurance.

### A.4 Shortlist changes

Item 2 loses its HP rider and item 3 becomes CT, with RK moving to item 4 (real
frames only). CT kill experiment, half a day, scalar code: copy the 256 live entries
into a phase-major 129 KB buffer and pre-remap the synthetic frame to slot numbers
0..255 (map cost excluded); run N = 256 and N = 1024. Predictions: ~52-55 and
~85-100 cycles/output. Then put the 64 KB byte map inside the row loop; go for CT if
>= 8 cycles/output survive with the map at N = 256, or regardless if the owner wants
the cliff insurance or the footprint reduction. Build order: AO, RI folded into AO,
CT, RK. Open question 1 is answered (random placement); the THP half of question 5
is void; question 5's slot-index half is promoted to the top alongside question 2.
