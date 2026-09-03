# IDEAS_B: SIMD strategies for `snes_ntsc_blit_hires` on Cortex-A53

Reviewer B. Starting emphasis: axes of parallelism, then the memory system.
Sources: BRIEF.md, ALGORITHM.md, `scalar_ref/*`. No prior SIMD attempt seen.

## 0. Baseline arithmetic and added hardware assumptions

- Full frame = 749 x 448 = 335,552 outputs. Scalar, 256 colours, 1 thread: 22.7 ms
  at 1 GHz = **67.6 cycles per output**. 65536 colours: 275 cycles per output.
- Scalar issue bound (ALGORITHM.md section 5): per 7 outputs about 90 loads, 200
  ALU, 7 stores = 297 instructions; dual issue gives >= 149 cycles, i.e. **~21
  cycles per output if nothing ever stalled**. So roughly **two thirds of the
  measured time (~46 cycles per output) is stall**: L1 misses to L2 (~900 distinct
  lines per row for 256 colours, more than the 512 lines of L1), load-use latency
  exposed by in-order issue, and very probably micro-TLB misses (the 12 loads of an
  output hit up to 12 different 4 KB pages of a 32 MiB table; the A53 data
  micro-TLB has about 10 entries). The 65536-colour figure (~1.4 new lines per
  output x ~150 cycles) shows that this core exposes essentially the full latency
  of every miss: memory-level parallelism is close to 1.
- Consequence for the whole study: instruction count matters, but the win has to
  come mostly from **issuing fewer loads and issuing them earlier**, and from
  **touching fewer lines and pages**. A vector design that keeps 12 loads per
  output cannot beat scalar by much whatever it does with the lanes.
- Added assumptions: a 128-bit NEON load costs 2 LSU cycles (64-bit path; if it is
  1 cycle the estimates below only improve); `ext`, `bsl`, `ushr`, `add`, `and`,
  `orr` on Q registers each cost 2 issue cycles; a NEON op can dual-issue with a
  load, store or integer op; unaligned Q loads are allowed and cost one extra cycle
  when they cross a 64-byte line (19% of random 4-byte-aligned addresses); the A53
  can keep only a handful of L1 misses in flight; Raspberry Pi OS uses 4 KB pages
  (THP status unknown, see experiment in strategy 2). Target for 2x is <= 34
  cycles per output; the 8 ms per half-frame budget is ~48 cycles per output.

## 1. Survey of the axes: what should sit in the four lanes?

For each candidate the decisive quantity is the number of load instructions per
output and whether the lane data can come straight from memory into the vector
register without a general-register round trip. The tap table (ALGORITHM.md 4.3)
and the closed form `out[t] = sum_j T[P(j)][42b + 14a(j) + (t - t0(j))]` are
the basis.

| lanes = | what one 16-byte load delivers | loads per output | extra ops per output | verdict |
|---|---|---|---|---|
| 4 adjacent outputs `t..t+3` (H4) | words `n..n+3` of one pixel's 14-word block: contiguous (fact 6.2) | 3.64 Q-loads | 1.29 `ext` for window edges, 3 finalize | **the only axis with a natural vector load** |
| 3 rows `r, r+1, r+2` at output `t` (V3) | with a phase-interleaved table, tap `(a,n)` of one colour at the 3 phases; useful only when the 3 rows hold the same colour at pixel `j` | 4.7 Q-loads if vertically flat, else 12 lane-loads | 3.7 adds, 4 finalize (one lane wasted) | dominated by H4 (strategy 5) |
| 4 rows `r..r+3` | four unrelated entries and two different phases: pure gather | 12 lane-loads (`ld1 {v.s}[i]`) | 2.75 adds, 3 finalize | no better than scalar on loads |
| 3 burst phases | a row has one phase; only rows differ in phase, so this is V3 | as V3 | | folded into V3 |
| 3 colour channels of one output | one packed word already holds all three channels; splitting them triples the bytes loaded | 12 loads | | worse than scalar |
| 12 taps of one output | 12 words at unrelated addresses | 12 lane-loads plus a horizontal add tree (`addv` is slow on A53) | ~4 | worse than scalar |

Two facts settle the choice. First, the 14 taps of one pixel are contiguous and
are consumed by 14 consecutive outputs, so "lanes = adjacent outputs" turns each
pixel into 3.5 to 5 contiguous 16-byte loads with no shuffling at all; every other
axis needs 12 separate lane loads per output because the words a lane needs live
in different 512-byte entries. Second, on this in-order core moving data from a
general register into a lane (`ins`/`dup`) costs several cycles of latency and a
NEON slot, so any axis that "gathers with scalar loads then inserts" is strictly
worse than the scalar loop it replaces. Lane loads from memory (`ld1` single
lane) avoid the register transfer but still cost one LSU op per word, i.e. the
same 12 loads per output as scalar, and they serialise on the destination
register in an in-order pipeline.

Rows and phases are therefore not useful as lanes. Rows are useful in two other
ways, both exploited below: (a) rows `r, r+3, r+6` share a burst phase and hence
the same 168-byte block of every colour they both contain, so processing them
together turns L2 misses into L1 hits (strategy 4); (b) independent rows give the
scheduler independent instruction streams to hide load latency without deep
unrolling inside one row.

Recompute versus look-up: ALGORITHM.md 3.4 shows every table word is a per-tap,
per-channel rounding of a linear function of (Y, I, Q) followed by an integer
error-correction step that depends on six other roundings. No strategy below
recomputes anything at blit time; all of them add exactly the same 32-bit words
as the scalar code (possibly in a different order or pre-summed), which is
bit-exact because the packed sum is plain modulo-2^32 addition, associative and
commutative. The only bit-exact recompute is to run `gen_kernel` +
`correct_errors` themselves (same code, same compiler, ~3-5 us per colour), which
is mentioned in strategy 2 as an optional lazy generator, never as a per-pixel
method.

Common H4 building block (used by strategies 1-4). Slot `s` of chunk `c` (input
pixel `j = 6c + 2 + s`) starts at output `t0 = 7c + s` and covers outputs
`t0..t0+13`. Keep accumulators as Q registers aligned to output index multiples
of 4. A pixel with lane offset `o = t0 mod 4` needs `ceil((o + 14) / 4)` vectors:
4, 4, 4, 5 for `o = 0..3`. The first vector for `o > 0` and the last vector when
the window does not end on a lane boundary are partial: they are loaded from
inside the block (never outside the 14 words) and shifted against a zero
register with `ext` (`vextq_u32(zero, v, o)` puts zeros in the low `o` lanes;
`vextq_u32(v, zero, k)` zeros the high lanes). Over the natural unroll period of
4 chunks = 24 pixels = 28 outputs = 7 output vectors, that is exactly 102 Q loads,
102 Q adds and 36 `ext`s, i.e. **3.64 loads, 3.64 adds, 1.29 `ext` per output**.
Finalize per output vector: clamp = `ushr 9, and, sub, orr, sub, and` (6 ops,
literally the scalar bit sequence, so 254/1 saturation is reproduced), convert =
three shifts (`ushr 21` for R to byte 0, `ushr 3` for G to byte 1, `shl 15` for
B to byte 2) plus three `bsl` with byte masks, the last one merging the constant
alpha byte (6 ops); no `rev32` is needed because the shifts place the bytes in
memory order directly. 12 ops per 4 outputs = 3 per output. ARM-only variant:
replace the three `bsl` by one `tbx` over three registers (saves ~2 cycles per 4
outputs, needs a portable fallback).

## 2. Strategy 1: H4-EXT, four adjacent outputs per register, table unchanged

1. **Core idea.** Vectorise across four consecutive output pixels. Each input
   pixel contributes 4-5 contiguous 16-byte loads from its 14-word block, added
   into 4-lane accumulators aligned to output groups of 4; window edges are
   handled with `ext` against a zero register. The loop is unrolled over 4 chunks
   (24 inputs, 28 outputs, 7 accumulator vectors) so every load offset is an
   immediate. The 32 MiB table is not touched; only the blitter changes.
2. **Axis.** Lanes = adjacent outputs `t..t+3` of one row.
3. **Gather.** There is none: the words the four lanes need are adjacent by
   construction (fact 6.2). Per output: 3.64 unaligned Q loads from 3.64 distinct
   entries instead of 12 scalar loads from up to 12 entries. Lines and pages
   touched per row are unchanged (same 56-byte windows); the number of load
   instructions that can miss falls 3.3x, and each load can be issued a whole
   group (~60 cycles) ahead of its use, so L2 latency is hidden if the miss queue
   allows (about 5 outstanding misses per group in the 256-colour case, at the
   limit of the A53, so expect partial hiding).
4. **Table.** Unchanged, 32 MiB.
5. **Cost per output.** NEON pipe: 3.64 add + 1.29 `ext` + 3.0 finalize = 7.93 ops
   x 2 cycles = **15.9 cycles**. LSU: 3.64 Q loads (7.3 cycles at 2 each) + 0.86
   `ldrh` + 0.25 Q stores, in parallel. Integer: 6 address computations per 7
   outputs (`madd` by 168 plus phase base), ~2 ops per output, dual-issued. Bound:
   ~16 cycles per output. With residual L1-miss stalls (same lines as scalar, but
   only 3.64 stall points per output and software pipelining across groups) the
   estimate is **18-24 cycles per output: 2.8-3.7x** over 67.6; call it 3x. The
   instruction count per output drops from ~42 to ~14, so the residual is
   NEON-throughput-bound, not issue-bound.
6. **Bit-exactness.** Exact everywhere including row edges: the same words are
   added (plus zeros), and 32-bit wrap-around addition is order-independent. The
   left edge is done by starting the accumulator grid at `t = -4` with the four
   black prologue pixels and discarding the first vector; the tail feeds six black
   pixels exactly as scalar does. Clamp/convert is the scalar bit sequence per
   lane, so 254 and 1 saturation values are reproduced.
7. **Portability.** SSE2: `_mm_loadu_si128`, `_mm_add_epi32`, `_mm_srli_epi32`,
   and/or/sub; `ext` against zero is `_mm_slli_si128`/`_mm_srli_si128` (no SSSE3
   needed because one operand is zero); `bsl` is and/andnot/or. RVV: `vle32`,
   `vadd`, `vslideup`/`vslidedown`, `vmerge`. Fully generic 128-bit SIMD.
8. **Risks.** (a) The 2-cycles-per-Q-op assumption drives the estimate; if some
   ops are single-cycle the loop becomes memory-bound and needs strategies 2 and 4
   to deliver. (b) Unaligned Q loads crossing lines (19%) and the miss-queue depth
   limit latency hiding. (c) Register pressure with 2-group software pipelining: 7
   accumulators + ~6 in-flight loads + 8 constants fits in 32 but leaves little
   for a second row. **Experiment (1 day):** hand-write the 28-output unrolled H4
   body with intrinsics, no edge handling, on a 256-colour random row repeated
   448 times; measure cycles per output with the cycle counter and compare the
   sums against scalar. This confirms or kills the 16-cycle bound and the
   miss-hiding assumption in one go.
9. **Effort.** 4-6 days: kernel 2, edge/prologue/tail 1, bit-exact harness 1, SSE2
   and RVV macro layer 1-2. Moderate complexity, no data-structure change.

## 3. Strategy 2: compact phase-major slot table with unified addressing (memory lever)

1. **Core idea.** A frame uses a few hundred of the 65536 entries, scattered over
   ~256 pages and ~900 partly used lines per row. Give each distinct input value
   seen a small slot number and copy its three 168-byte phase blocks into a
   compact, phase-major table `C[3][NSLOT][42 words]`, optionally with three zero
   words between 14-word blocks so that H4 needs no `ext`. The blitter addresses
   `plane[b] + off[v]` where `off[]` is a 65536-entry `uint32` byte-offset map
   (256 KB, one load per input pixel). To keep one base per phase, the master is
   re-laid-out at init as phase-major `M[3][65536][42]` (still 32 MiB) and the
   compact slots are the first NSLOT entries of each phase plane, so a value with
   no slot addresses the master with the same stride and no branch. Slots persist
   across frames; a value is copied (3 x 168 B, ~200 cycles from DRAM) the first
   time it is seen and re-copied only after round-robin eviction. No separate
   remap pass: the `off[v]` load replaces the `v * 512` address computation in
   the row loop.
2. **Axis.** None by itself; it is the memory-side companion of any blitter,
   scalar or H4. It attacks the ~46 stall cycles per output directly.
3. **Access pattern.** Per output still 12 scalar (or 3.64 Q) loads, but for a
   256-colour frame the per-row working set becomes 256 x 168 B = 43 KB = 672
   lines in 11 pages (864 lines, 14 pages if padded), against ~900 lines in 256
   pages today. The frame's working set (~300 colours x 504 B = 150 KB) is
   L2-resident after warm-up; NSLOT only bounds the allocation. The 65536-colour
   cliff becomes "one entry copy per new value", at most NSLOT per frame, then
   master-table speed for the rest: no worse than today.
4. **Table.** Master 32 MiB (relaid, not grown) + 65536 x 4 B offset map (262,144
   B) + 3 x NSLOT x 168 B compact planes (1,032,192 B for NSLOT = 2048; padded at
   216 B per slot: 1,327,104 B). If the master is replaced by lazy `gen_kernel` +
   `correct_errors` calls (bit-identical, same code, ~3-5 us per new colour) the
   whole object shrinks to ~1.3 MB, with a worst-case ~2.5 ms hitch on a
   500-colour scene change; keep the master unless RAM is needed elsewhere.
5. **Cost.** Scalar blitter unchanged except one `ldr` per input pixel (0.86 per
   output) replacing a shift-add. The gain is entirely stall removal: micro-TLB
   misses fall from up to 12 per output to about 0 (11 pages), L2 line fetches
   per row fall ~25%, and nothing goes to DRAM after warm-up. Scalar alone: **68
   to 40-50 cycles per output (1.4-1.7x)**, uncertain because the split of the
   stall between TLB and cache is not measured. With H4 (strategy 1): residual
   stalls ~3-5 cycles on top of the 13.3 (padded, no `ext`) or 15.9 (unpadded)
   NEON bound: **17-20 cycles per output, 3.4-4x**.
6. **Bit-exactness.** Exact: words are copied, never recomputed; padding words
   are zero and only land in lanes that the scalar code does not sum.
7. **Portability.** Pure C; identical on all targets. On x86 and RVV, where
   128-bit ops are single-cycle and H4 is memory-bound, this is the dominant
   lever, not a companion.
8. **Risks.** (a) The stall budget may be mostly L2 latency on lines rather than
   TLB, in which case the gain on the 256-colour synthetic is nearer 1.2x (real
   frames with fewer colours per row gain more, since 190 colours x 168 B fits
   L1). (b) Slot churn on scene changes. **Experiment (half a day, no new code):**
   run the existing scalar blitter on the 256-colour random frame twice, once with
   the 256 values drawn at random from 0..65535 and once as `0..255` (entries
   contiguous in 128 KB = 32 pages); the ratio isolates page/TLB spread from line
   count. Also try `madvise(MADV_HUGEPAGE)` on the 32 MiB allocation: if THP alone
   recovers most of the gap, the slot table can be reduced to the phase-major
   relayout only.
9. **Effort.** 2-3 days (relayout at init, offset map, slot allocator with
   eviction, verification). Low complexity; independent of the SIMD work.

## 4. Strategy 3: pre-summed chunk kernels for flat runs (run-length reuse)

1. **Core idea.** When the six input pixels of a chunk share one colour `P`, the
   six shifted 14-tap kernels sum to a fixed 19-word "chunk kernel"
   `CK_b[P][m] = sum_{s=0..5} K_{b,a(s)}[P][m - s]`, `m = 0..18` (`K[n] = 0`
   outside `0..13`). Precompute it per slot when the slot is filled (strategy 2)
   for the three phases: 3 x 20 words = 240 B per slot. Per chunk, test
   uniformity of the six input values; if uniform, the chunk's entire
   contribution is 5-6 grid-aligned Q loads and adds instead of 25-26 loads,
   25-26 adds and 9 `ext`. Optional second table, the pair kernel
   `PK[a][m] = K_a[m] + K_a[m-1]` (15 words, 3 alignments, 3 phases, 540 B per
   slot), halves loads and adds for even/odd pairs of equal colour inside mixed
   chunks. Mixed chunks use the H4 per-pixel path unchanged.
2. **Axis.** Adjacent outputs (the H4 grid) plus run-length redundancy along the
   row; the game's sky, road body and grass are long horizontal runs.
3. **Gather.** As H4 for mixed chunks. A flat chunk loads 80 contiguous bytes (2
   lines) instead of three 56-byte windows, and issues 0.8 Q loads per output
   instead of 3.64.
4. **Table.** Compact slot grows from 504 B to 744 B (with CK) or 1284 B (with
   CK and PK); for NSLOT = 2048 that is 1.5 MB or 2.6 MB. Per-frame working set
   at 300 colours: 223 KB or 385 KB; the latter is marginal against the 512 KB
   shared L2, so start with CK only. Master table unchanged.
5. **Cost.** Flat chunk: 5.5 loads, 5.5 adds, ~1.5 `ext` (0 if padded) per 7
   outputs, plus the 3 finalize ops per output: 4.0 NEON ops = **8 cycles per
   output** against 15.9 for the per-pixel path. Uniformity test: two 64-bit
   scalar loads of the six input halfwords compared against `v * 0x0001000100010001`,
   ~5 integer ops and one branch per chunk (< 1 cycle per output); mispredicts
   at run boundaries cost ~8 cycles each, ~30 per row = 0.3 cycles per output.
   With a fraction `f` of flat chunks the NEON bound is `15.9 - 7.9 f`: `f = 0.5`
   gives 12, `f = 0.7` gives 10.4 cycles per output. Including residual stalls,
   at `f = 0.5`: **14-17 cycles per output, 4-4.8x**, i.e. a further 1.2-1.4x on
   top of strategies 1+2. On the synthetic random-colour benchmark `f = 0` and
   the gain is nil (only ~1 cycle of test overhead), so the brief's headline
   metric does not reward this; real frames do.
6. **Bit-exactness.** Exact. The chunk kernel is the same words added in a
   different order (associative modulo 2^32). The two bias taps per output are
   preserved automatically: `CK` contains slot 0's and slot 1's `a = 0, n <= 6`
   words at the same positions the per-pixel path would have added them. `PK`
   likewise. The flat/mixed decision changes only the grouping, never the set of
   words.
7. **Portability.** Pure C plus the strategy 1 primitives.
8. **Risks.** `f` on real hires OutRun frames is unknown: road stripes, sprites,
   text and the road-edge dither reduce it; sky and grass raise it. The chunk
   alignment is fixed (`j = 6c + 2 .. 6c + 7`), so a flat run of length `L`
   yields only about `floor((L - 5) / 6)` flat chunks: runs shorter than 11 never
   qualify. **Experiment (half a day):** dump 20 in-game 16-bit input frames,
   count uniform 6-pixel chunks at the blitter alignment and uniform even/odd
   pairs; if `f < 0.3` drop the idea, if `f > 0.5` also evaluate `PK`.
9. **Effort.** 3-4 days on top of strategy 2 (kernel generation at slot fill, path
   selection with a 19-word footprint spanning three output groups,
   verification on flat and boundary rows). Moderate.

## 5. Strategy 4: same-phase row-triplet interleaving (row-order lever, no table change)

1. **Core idea.** Within a 224-row call, rows `r, r+3, r+6, ...` share a burst
   phase and hence the same 168-byte block of every colour they contain. Process
   three same-phase rows chunk by chunk together (for each chunk `c`: row `r`,
   then `r+3`, then `r+6`), so a colour's block fetched from L2 for row `r` is an
   L1 hit for the other two rows a few hundred cycles later. Triplets are visited
   in the order (0,3,6), (1,4,7), (2,5,8), (9,12,15), ... Alternatively process
   all rows of one phase back to back (0,3,...,222 then 1,4,...), which keeps a
   phase's working set in L1 across 75 rows when it fits (<= 190 colours x 168 B
   = 32 KB). Output rows are written out of order, which is harmless: the rows
   are independent 3 KB stores.
2. **Axis.** Rows, but as cache locality and independent instruction streams for
   the in-order scheduler, not as lanes.
3. **Gather.** Unchanged per row. L1 misses: the first row of a triplet takes them
   as today, the other two mostly hit, so misses per row fall up to 3x (bounded
   by L1 capacity: at 43-57 KB per phase with 256 colours even the first row is
   partly thrashed, but the reuse distance between rows `r` and `r+3` drops from
   three rows to one chunk). Vertical redundancy is exploited implicitly: where
   rows `r` and `r+3` hold the same pixel at column `j`, the second access is
   the same address and a guaranteed hit, with no explicit test.
4. **Table.** None; 0 additional bytes.
5. **Cost.** No new arithmetic; pointer bookkeeping only (3 input, 3 output and 18
   kernel pointers in the H4 version fit in 31 GPRs; the scalar version needs 12
   kernel pointers per row so pair rows `(r, r+3)` rather than triple). The gain is
   whatever share of the 46 stall cycles is L1-miss-to-L2 latency: if 20-30
   cycles, scalar goes **68 to 50-55 (1.25-1.35x)**; on top of strategies 1+2,
   where the residual stall is ~4 cycles, it saves 2-3 cycles per output
   (~1.15x). Small in absolute terms, but free.
6. **Bit-exactness.** Identical computation per row; only the visiting order
   changes. The per-row phase must be computed as `(burst_phase + row) mod 3`
   explicitly instead of incremented.
7. **Portability.** Pure C.
8. **Risks.** Rests on the stall being L1-miss dominated; no interaction with the
   two-thread split (each thread already owns 224 rows). **Experiment (2 hours):**
   in the scalar blitter, reorder the row loop to equal-phase order and time the
   256-colour frame; then try the `(r, r+3)` chunk-interleaved pair.
9. **Effort.** 0.5-1 day scalar; 1-2 days inside the H4 kernel (register
   allocation). Low.

## 6. Strategy 5: V3, three rows in lanes with a phase-interleaved table (analysed, not recommended)

1. **Core idea.** Lanes 0..2 hold rows `r, r+1, r+2`, whose phases are
   `b, b+1, b+2` (a fixed rotation of 0,1,2), lane 3 unused. Re-lay the table so
   the three phases of one tap are adjacent: `T'[v][a][n][phase]`, 12 B per tap
   unpadded (16 B padded). For each of the 12 (pixel, tap) pairs of an output
   column: if the three rows hold the same value at pixel `j`, one Q load fills
   all three lanes; otherwise three single-lane loads `ld1 {v.s}[i]`. Then 11 Q
   adds, a 3-lane finalize and three single-lane stores.
2. **Axis.** Lanes = rows, equivalently lanes = burst phases.
3. **Gather.** Vertically uniform pixel: 14 Q loads per pixel-triplet = 4.67 per
   pixel-row = 4.0 per output (H4: 3.64). Non-uniform: 42 lane loads per triplet
   = 12 per output, the scalar count. Locality is genuinely better: a colour's
   three phases are 504 contiguous bytes shared by three consecutive rows, so
   lines per colour per three rows fall from ~10 to 8, in one page.
4. **Table.** 32 MiB unpadded with a 12-byte tap stride (lane 3 reads the next
   tap's phase 0 as garbage, masked for free because lane 3 is never stored), or
   42 MiB padded (+31%, over budget).
5. **Cost per output.** NEON: 11/3 adds + 12/3 finalize = 7.67 ops = 15.3 cycles,
   the same as H4, but loads are 4.0 (flat) to 12 (mixed) and the single-lane
   loads serialise on their destination register in an in-order core, so the
   mixed path is >= 12 LSU cycles plus exposed latency chains: **15-20 cycles
   (vertically flat) to 30-40 (mixed), 1.7-3x**, never better than H4 and worse
   wherever there is detail. Also one lane store per output instead of 0.25.
6. **Bit-exactness.** Exact: the same words are summed.
7. **Portability.** Single-lane loads are `_mm_insert_epi32` (SSE4.1 only; SSE2
   must go through a GPR and shuffles, several ops). RVV 1.0 has indexed loads
   (`vluxei32`), which makes rows-in-lanes far more attractive there than on A53;
   worth remembering if the RVV path ever needs its own kernel.
8. **Risks.** Single-lane load throughput on A53 is unmeasured; the fraction of
   3-row-uniform pixels in hires frames is unknown (the road texture changes
   every row). **Experiment (half a day):** microbenchmark a chain of
   `ld1 {v.s}[i]` on the Pi; count 3-row-uniform pixels on the dumped frames from
   strategy 3's experiment.
9. **Effort.** 4-6 days. Not recommended on this hardware; kept as the honest
   answer to "what about rows in lanes".

## 7. Ranking and recommendation

Estimated gains are for the 256-colour single-thread case unless stated; the
combined figures assume the H4 NEON bound holds.

| rank | strategy | expected gain | P(success) | effort | EV (gain x P / days) |
|---|---|---|---|---|---|
| 1 | S1 H4-EXT (four adjacent outputs) | ~3x alone (2.8-3.7x) | 0.8 | 5 d | 0.48 |
| 2 | S2 compact phase-major slot table | 1.4-1.7x alone; 3.4-4x with S1 | 0.8 | 3 d | 0.43 alone, enabler for S1/S3 |
| 3 | S4 same-phase row-triplet interleave | 1.25-1.35x alone; +1.15x on S1+S2 | 0.7 | 1 d | 0.9 per day but small absolute |
| 4 | S3 chunk kernels for flat runs | +1.2-1.4x on real frames, 1.0 on the synthetic | 0.6 | 4 d | 0.2 |
| 5 | S5 V3 rows in lanes | 1.7-3x optimistic | 0.4 | 5 d | 0.2, dominated by S1 |

By pure EV per day S4 is first, but it is an afternoon's change and cannot reach
2x on its own, so it is a rider on whichever kernel wins. The two I would take
forward are **S1 (H4) and S2 (compact phase-major table)**: S1 is the only axis
with a natural vector load and lowers the instruction count 3x, S2 removes the
page and line spread that the measurements say dominates scalar time, and the
two are independent enough to be built and measured in parallel by running the
two half-day experiments first (H4 microkernel; contiguous-versus-random colour
test). Together with S4 folded into the row loop the expected result is 15-18
cycles per output, about 4x, i.e. roughly 5-6 ms for a full frame single-threaded
and about 3 ms per half frame on two threads, comfortably inside the 8 ms
budget; S3 is the follow-on if the frame dump shows more than 30% flat chunks.
