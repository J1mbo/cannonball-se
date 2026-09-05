# IDEAS_A: memory-first SIMD strategies for `snes_ntsc_blit_hires` on Cortex-A53

Reviewer A. Emphasis: where the bytes come from and how far they travel. Sources: BRIEF.md, ALGORITHM.md, scalar_ref/*.

## 0. Where the 68 cycles go (framing shared by all five strategies)

Per output the scalar code issues 12 four-byte loads to 12 different 512-byte entries, 11 adds, 5 clamp ops, 6 convert ops, a byte swap and a store, plus 1.7 input-load/address ops amortised: about 38 instructions. On a dual-issue in-order A53 with one load per cycle that is ~24 issue cycles if every load hit L1. Measured: 68 cycles per output at 256 colours, 275 at 65536. So roughly **44 cycles per output are memory stalls even in the good case**. I attribute them to three sources, in the order I think they matter:

1. **Address translation.** The table is 32 MiB; a 4 KiB page holds 8 entries; 256 random colours land on ~256 distinct pages. A 6-input chunk keeps 17 entries (17 pages) live, and the A53 data micro-TLB has 10 entries, so nearly every one of the 12 loads per output misses the micro-TLB and takes a main-TLB lookup (assume 2-4 cycles each, exposed by in-order issue: 25-45 cycles per output). At 65536 colours the 8192 live pages exceed the 512-entry main TLB and each miss becomes a page walk; that fits the 275-cycle figure better than DRAM line fills alone do (1.43 first-touch lines per output x ~120 cycles = 170, plus 24 issue).
2. **L1 capacity.** A row touches one 168-byte phase block (3-4 lines) of each colour: 256 x ~3.3 lines = 53 KB > 32 KB L1, and the phase changes every row so each row starts cold. Roughly 0.7-1.0 L2 fills per output at ~15 cycles, mostly serialised: 10-15 cycles.
3. **Instruction count**: the 24 cycles above, the only term SIMD attacks directly.

The consequence is the thesis of this document: a vectorised inner loop that leaves the access pattern alone saves at most 10-15 of 68 cycles. Every SIMD strategy below is therefore paired with the layout changes that attack (1) and (2), and one strategy is nothing but those changes.

Assumptions added to the brief's: 16-byte NEON loads issue at one per cycle from L1 (if it is two, nothing below becomes load-bound); an unaligned 16-byte load that crosses a 64-byte line costs one extra cycle; the L1 tolerates 3-4 outstanding misses (needed for prefetch to help at all); micro-TLB 10 entries, main TLB 512 entries; a 4-register TBL costs ~4 cycles. Each of these is something the experiments below measure rather than something I rely on blindly.

Structural fact used throughout (ALGORITHM.md section 4, re-derived): pixel `(c, s)` (chunk c, slot s) has `t0 = 7c + s`, so within the 7-output chunk c every one of the 17 contributing pixels adds a **contiguous run** of its 14-word window: pixel `(c-1, s)` adds words `14a+7-s .. 14a+13-s` to all seven outputs; pixel `(c, s)` adds words `14a .. 14a+6-s` to outputs `s..6`; pixel `(c-2, s>=1)` adds words `14a+14-s .. 14a+13` to outputs `0..s-1`. The scalar "gather" is only a per-output view of a FIR whose tap axis is already contiguous in memory.

---

## Strategy 1: Phase-split table + chunk vectors in the packed word format

**1. Core idea.** Keep the 32-bit packed words exactly as generated, but read them as vectors along the tap axis: accumulate one chunk's seven outputs in two `uint32x4` registers (lane 7 is junk and never stored) by adding, for each of the 17 contributing pixels, one or two unaligned 16-byte loads from its window. Full runs (previous-chunk pixels) need two loads and no fixing; short runs are aligned to their lanes and zero-filled with one TBL (`vqtbl1q_u8` with out-of-range indices, which return 0) or an AND mask. Per chunk: 28 loads, 28 vector adds, 8 TBLs (counted slot by slot from the run table above). The packed clamp is the same 5 bit-ops on `uint32x4`; conversion is 3 shifts, one byte-TBL to pick R,G,B into place, one OR for alpha. Layout: split into three phase arrays `T_b[65536][42]` (168-byte stride, 16-byte guard at each end so over-reads of up to 3 words past a window stay in bounds), backed by huge pages (Strategy 2), with a software prefetch of chunk `c+2`'s six windows issued during chunk `c`.

**2. Axis.** Lanes = adjacent output pixels; the tap index runs along the lanes.

**3. Gather and access pattern.** 4.0 sixteen-byte loads per output (28/7) instead of 12 four-byte loads; 64 bytes loaded per output versus 48, but the set of cache lines touched is the same as scalar (1.43 first-touch lines per output), falling to ~1.3 because 168-byte-aligned blocks straddle fewer lines than 512-byte entries at offset 168b. Plus 0.86 input loads and 1-2 `PRFM` per output. The prefetch is what converts serialised L2 stalls into overlapped fills on an in-order core: chunk `c+2` is ~60 cycles ahead at the target speed, which covers L2 fully and DRAM partially if 3-4 misses can be in flight.

**4. Table.** 3 x 65536 x 168 B = 31.5 MiB (words 126-127 dropped). Padding blocks to 192 B would make each exactly three lines but costs 36 MiB; I would not, given the budget.

**5. Cost per output.** NEON: (28 add + 8 TBL) + epilogue 2 x (6 clamp + 8 convert) = 64 ops per chunk x 2 cycles / 7 = **18.3 cycles**. Loads 4.0 + ~2.5 (input, prefetch) on the load pipe, in parallel. Scalar: 6 `ldrh`, 6 `madd`, loop = ~4 per output, dual-issued. Stalls: with huge pages, prefetch and phase split, 8-15 cycles (down from ~44). Total 27-33 cycles: **2.0-2.5x** on the 256-colour case. The same kernel without the memory work sits at 18 + 44 = 62 cycles, 1.1x. Large palettes: 1.3 DRAM fills per output, partly overlapped: 275 -> ~120-150 cycles, no cliff relative to today.

**6. Bit-exactness.** Exact by construction: identical words, identical mod-2^32 packed adds (association order is irrelevant for wrap-around addition), identical bit-ops. The prologue and tail use `T_b[0]` (black) as scalar does. The only care point is the last chunk of a row: lane 7 would write `out[749]`, which is the next row's pixel 0 (harmless, overwritten) except on the last row of the half, so the last chunk stores seven lanes.

**7. Portability.** Best of the five. `uint32x4` loads/adds/shifts/ands map directly to SSE2 (`_mm_loadu_si128`, `_mm_add_epi32`, `_mm_srli_epi32`); lane zeroing by AND masks (no `pshufb` on SSE2) costs the same op count as TBL; conversion is 3 shifts + 3 ANDs + 3 ORs on SSE2 (8 ops instead of 5). RVV: `vle32` + `vadd` + masked `vand`, natural.

**8. Risks and experiment.** (a) The stall decomposition in section 0 is inferred; if the TLB term is small, the gain is 1.5-1.8x. (b) Huge pages may be unobtainable on a fragmented 512 MB system. (c) Unaligned-load and 128-bit-op costs are assumed. Experiment, one day: write only the accumulation kernel (28 loads, adds, TBLs) with intrinsics for one chunk over an L1-resident synthetic buffer and confirm ~18 cycles per output; separately run the unmodified scalar blitter against a phase-split, `MADV_HUGEPAGE`-backed table to measure the memory half on its own.

**9. Effort.** 5-8 days: kernel 2-3, split-table init and prefetch 1, edge cases and bit-exact harness 2, x86 macro layer 1-2. Moderate complexity, no new maths.

---

## Strategy 2: Fix address translation and page locality (huge pages, then a dense hot-colour cache) - memory only

**1. Core idea.** Change nothing arithmetic. (a) Allocate the table 2 MiB-aligned and `madvise(MADV_HUGEPAGE)` it (or `MAP_HUGETLB` where the system reserves pages): 32 MiB becomes 16 translations instead of 8192, the 17 live entries per chunk fall on at most 16 pages that the main TLB always holds, and one micro-TLB entry now covers 4096 entries, so the per-load micro-TLB miss that section 0 blames for 25-45 cycles per output mostly disappears. (b) If huge pages cannot be guaranteed on a fragmented 512 MB system, put a dense cache in front: `remap[65536]` (one byte per colour, 64 KB; halfwords for more than 255 slots) mapping a colour to a slot in a small phase-split array of hot colours (1024 slots x 3 x 168 B = 516 KB, itself 2 MiB-aligned so it is one huge page or 126 ordinary pages that fit the main TLB). A slot is filled on first sight by copying 504 B from the master table (~0.2 us) and evicted round-robin; the master table stays as backing store, so a 65536-colour frame degrades to today's behaviour plus copies rather than to a new cliff.

**2. Axis.** None. It composes with Strategies 1, 3, 4 and 5 and helps the untouched scalar blitter.

**3. Gather and access pattern.** Load count unchanged; every load resolves through at most 16 TLB entries. With the dense cache the per-row footprint is also contiguous (256 colours x 168 B = 43 KB inside a 516 KB region), which improves DRAM row-buffer and L2 behaviour. The remap adds 0.86 byte loads per output whose 256 touched lines (16 KB) compete for L1; it must be issued a chunk ahead so its latency is hidden on the in-order core. If the game author is willing, the renderer's palette-conversion pass could emit slot numbers directly (the palette-to-slot map has only 8192 entries) and the remap load vanishes; I flag this but do not rely on it because the input format is fixed.

**4. Table.** 31.5 MiB phase-split master (or the existing 32 MiB) + 516 KB cache + 64-128 KB remap.

**5. Cost per output.** Scalar issue unchanged at ~24 cycles; removes the micro-TLB term and, at 65536 colours, the page walks. Estimate 68 -> 35-45 cycles (**1.5-1.9x**) with no SIMD at all, 275 -> ~180. With the dense cache instead of huge pages, subtract a further ~2 cycles for the remap. This carries the widest error bar in the document because it rests on the TLB hypothesis of section 0.

**6. Bit-exactness.** Exact: the same words are summed.

**7. Portability.** OS-level rather than ISA-level. Linux `madvise` on ARM and RISC-V; Windows large pages need a privilege, and modern x86 cores have 64+ entry L1 DTLBs with 1536-entry L2 TLBs, so the x86 build can simply skip it. The dense cache is plain C.

**8. Risks and experiment.** Transparent huge pages may be off or defeated by fragmentation (check `/sys/kernel/mm/transparent_hugepage/enabled` and `AnonHugePages` in `/proc/self/smaps`); the micro-TLB penalty may be smaller than assumed. Experiment, 2-3 hours: `perf stat -e dTLB-load-misses,dTLB-loads,L1-dcache-load-misses,cycles` on the existing scalar benchmark, then again with the `MADV_HUGEPAGE` allocation. Ten or more dTLB misses per output and a 20+ cycle drop confirm the model; if misses are already low, the memory term is L1/L2 capacity and Strategies 1 and 5 carry the load. This is the cheapest, highest-information experiment in the study and should run first.

**9. Effort.** Half a day for huge pages; two days for the dense cache with eviction and a bit-exact check. Low complexity.

---

## Strategy 3: TBL as the gather - lanes = 16 chunks of one slot, per-band transposed int8 tap tables

**1. Core idea.** The only gather NEON has is TBL: 16 byte lookups from a 64-byte table in one instruction. Make the table small enough to be a TBL operand. For a band of rows whose palette has at most 64 colours, build per phase the 126 little tables `L[a][n][ch][64]` (one signed byte per colour: that tap's contribution, bias removed) and give each pixel a 6-bit colour code. For output `x` of chunk `c`, held in lane `c` of a 16-lane vector, the 12 contributions are 36 `vqtbl4q_u8(L[a][n][ch], code_s)` lookups, where `code_s` is the code vector of slot `s` over 16 consecutive chunks (previous-chunk contributions use the same vector shifted one lane with `EXT`), widened and summed in `int16x8` lanes with `vaddw_s8`, bias 512 preloaded. A phase's whole table is 8 KB, resident in L1; the data dependence has moved from the load address into the TBL index.

**2. Axis.** Lanes = chunks, i.e. output positions seven apart, i.e. pixels of the same slot. Genuinely different from lanes = neighbouring outputs.

**3. Gather and access pattern.** Per output ~4.5 sixteen-byte loads of TBL operands (sequential, 8 KB, L1) and ~2.3 loads of code vectors; nothing from the big table. Distinct lines touched per row: 128 regardless of content within the 64-colour limit, versus ~850 for scalar at 256 random colours. Output for lane `c` lands at `out[7c+x]`, a stride of 28 bytes, so the 16 results of an `x` pass are written with 16 `vst1q_lane_u32` stores after a zip to RGBA (1 store per output, on the store pipe).

**4. Table.** Master table converted once to int8 planar (24 MiB, Strategy 5's format) as the source; per band (16 rows) a 24 KB working table for three phases plus a 64 KB colour-to-code map. Codes are written per row into six slot-major byte arrays so code vectors are contiguous loads.

**5. Cost per output.** 2.25 TBL4 (assume 4 cycles) + 2.25 `vaddw` (2) = 13.5 NEON cycles; epilogue (clamp emulation on six `int16x8`, zips, 16 lane stores) ~5; code assignment ~1.5 scalar, dual-issued; band table build (24 KB of byte scatter per 12k outputs) ~3; stalls ~2. Total ~25 cycles: **~2.7x where a band has at most 64 colours**. 65-128 colours: a second `TBX` level doubles the gather, ~36 cycles, 1.9x. Above 128, including the 256-colour synthetic benchmark, four levels give ~48 cycles (1.4x) with a 32 KB per-phase table that thrashes L1, so the band falls back to Strategy 1. Large palettes are handled by fallback, not by this path.

**6. Bit-exactness.** Conditional. Requires (i) every per-tap, per-channel contribution to fit int8 (they are fractions of a channel value of at most 128, so it should, but it must be read off the init code), and (ii) the field sum `S = 512 + w` reproduced in int16 and the clamp table of ALGORITHM.md section 2 applied per lane (ov 2 -> w, 3 -> 254, 1 -> 0, 0 -> 1). Identical to scalar wherever scalar's own field sums stay within 0..1023, i.e. wherever scalar is not already corrupting a neighbouring field. Proven by whole-frame comparison, not by construction.

**7. Portability.** SSSE3 `pshufb` is a 16-entry table, so a 64-entry gather costs four shuffles plus blends and the advantage evaporates; SSE2 has nothing. RVV `vrgather` does this naturally at any table size. So NEON and RVV yes, x86 uses Strategy 1; the macro layer must permit per-ISA blitter selection.

**8. Risks and experiment.** TBL4 throughput on A53 (8 cycles instead of 4 cuts the gain to ~1.8x); real bands with sprites may exceed 64 colours; int8 range; substantial code. Experiment, half a day: (a) a microbenchmark of a `vqtbl4q_u8` + `vaddw_s8` loop for cycles per gather; (b) dump ten real frames and count distinct colours per row and per 16-row band.

**9. Effort.** 10-15 days: band manager, table builder, three code paths, fallback plumbing, exactness harness. High complexity.

---

## Strategy 4: Exploit runs of identical pixels - lazily built per-colour merged kernels

**1. Core idea.** Wrap-around addition is associative, so any subset of the 12 contributions that comes from one colour can be pre-summed once per colour instead of once per output. Three granularities. (a) Flat constant: if chunks `c`, `c-1` and `c-2` are one colour `v` (a run of 18 or more pixels), all seven outputs equal `C(v)`, the clamped and converted `2*rgb(v)` (phase-independent, since `correct_errors` targets the same `rgb` in every phase): two 16-byte stores. (b) Chunk kernel `K6[v][b][0..19]`: the 20-tap sum of six same-colour pixels in slots 0-5, so a uniform chunk contributes 6 loads + 6 adds instead of ~28 loads + 36 ops. (c) Pair kernel (15 taps) for a same-coloured even/odd slot pair, cheap because the odd slot is the even kernel delayed one word. Kernels are built on first sight into a direct-mapped cache keyed by colour (`K6` 240 B, `C` 4 B) and persist across frames. Detection per chunk: one 16-byte load of the six input halfwords, `cmeq` against a broadcast, one horizontal min.

**2. Axis.** Reuse across identical inputs; the lanes are Strategy 1's (it is an add-on to Strategy 1 and equally to 5).

**3. Gather and access pattern.** A uniform chunk touches one 80-byte kernel (two lines) instead of six 56-byte windows; a flat chunk touches 4 bytes. On sky, road and grass the table traffic essentially disappears; the kernel cache for a few hundred colours is ~128 KB but a sky row's active set is one to three colours.

**4. Table.** Master unchanged plus a kernel cache, e.g. 512 entries x 256 B = 128 KB.

**5. Cost per output.** Flat chunk ~6 cycles (test and two stores) for seven outputs; uniform chunk ~12 accumulate ops + 28 epilogue ops = 80 NEON cycles versus 128 in Strategy 1. My prior for OutRun frames is 50 % of outputs flat, 20 % uniform-chunk, 30 % general: 0.5 x 1 + 0.2 x 12 + 0.3 x 30 = ~12 cycles per output, **~2.5x on top of Strategy 1, ~5x versus scalar on real frames, and 0.97x on the random synthetic benchmark** (detection overhead). It contributes nothing to the brief's synthetic goal and must be justified on captured frames.

**6. Bit-exactness.** Exact by construction: the same multiset of words is summed mod 2^32; `C(v)` is produced by running the ordinary 12-tap path once per colour; the two bias taps per output are preserved because the merged kernels contain exactly the words scalar would have added.

**7. Portability.** Plain C control flow plus Strategy 1's vector adds; SSE2 and RVV identical.

**8. Risks and experiment.** Content dependence (some stages put scenery in the sky band; road stripes shift per row but remain long runs); branch misprediction in mixed regions on the A53's modest predictor (mitigate by deciding per group of chunks); cache bookkeeping. Experiment, two hours: dump 10-20 real frames from attract mode and several stages, histogram horizontal run lengths, and report the share of uniform chunks and of outputs whose 17 contributors are one colour.

**9. Effort.** 3-4 days on top of Strategy 1. Low to moderate complexity.

---

## Strategy 5: int8 planar compact table with 16-bit lanes (lanes = 8 adjacent outputs per channel)

**1. Core idea.** Store each tap as three signed bytes instead of a packed word: per phase 126 B, laid out alignment-major (`a`, then channel, then 14 taps) so one pixel's window is 42 contiguous bytes, padded to 128 B = exactly two cache lines; entry 384 B; table 24 MiB. Accumulate the seven outputs of a chunk in three `int16x8` accumulators (one per channel, 512 bias preloaded) with one 8-byte load and one `vaddw_s8` per channel per contribution. Partial runs are aligned and zero-filled by a 64-bit `vtbl1` (out-of-range index gives 0, so one op does both jobs). The exact clamp is two tiny TBLs on `ov = S >> 8` producing a constant (1, 0, 0, 254) and a pass-through mask, then `(S & 0xFF & mask) | constant`; `vst4_u8` interleaves R, G, B, A for eight pixels.

**2. Axis.** Lanes = adjacent outputs as in Strategy 1, with channels split across accumulators instead of packed fields.

**3. Gather and access pattern.** Per contribution three contiguous 8-byte loads; 51 per chunk = 7.3 loads and 58 bytes per output. A window spans one line (a = 0, 2) or two (a = 1): 1.33 first-touch lines per pixel, 1.14 per output (scalar 1.43); per colour and phase exactly two lines, so 256 colours = 512 lines = 32 KB, the whole row working set is L1-sized where the packed layout needs 53 KB.

**4. Table.** 65536 x 384 B = 24 MiB (8 MiB smaller); 384-byte entries already put each phase on its own line pair, so no phase split is needed.

**5. Cost per output.** NEON per chunk: 30 TBL (1 cycle) + 51 `vaddw` (2) + epilogue ~28 = 160 cycles, **~23 per output**; loads 7.3 plus input, in parallel; stalls 5-10 with an L1-resident set and huge pages. Total 30-34 cycles: **2.0-2.3x**; large palette 1.14 DRAM fills per output, about 20 % better than Strategy 1 (275 -> ~110-130 with prefetch). Same speed as Strategy 1 on the benchmark, smaller table, slightly gentler cliff, but conditional exactness and a NEON-bound loop with no slack.

**6. Bit-exactness.** Conditional, exactly as Strategy 3 (int8 range, field sums in 0..1023, re-implemented clamp table). Building the compact table from the existing init in the same binary is bit-safe; ALGORITHM.md section 7.9 applies if the conversion were done offline elsewhere.

**7. Portability.** SSE2 has no byte table and no widening add: `punpcklbw` + arithmetic shift and AND masks, roughly 1.5x the NEON op count; SSE4.1 `pmovsxbw` helps; RVV `vwadd` is natural. Portable, less elegant than Strategy 1.

**8. Risks and experiment.** A tap with magnitude above 127 breaks the format (the luma centre tap for a 121-unit channel should stay near 64 per tap, but verify); NEON op count is the bottleneck so every extra shuffle costs 2 cycles; `vst4` throughput. Experiment, 2-3 hours: instrument `gen_kernel` to log min/max of `r-256`, `g-256`, `b-256` over all entries and taps, and log the extreme field sums on a few real frames.

**9. Effort.** 6-8 days: init packer, kernel, clamp emulation, harness, x86 fallback. Moderate to high.

---

## Ranking by expected value (gain x probability / effort)

| Rank | Strategy | Gain, 256-colour synthetic | P(success) | Effort | Verdict |
|---|---|---|---|---|---|
| 1 | S2 huge pages / dense cache | 1.5-1.9x, memory only | 0.55 | 0.5-2 d | Run its experiment first |
| 2 | S1 phase-split packed chunk vectors (+S2) | 2.0-2.5x | 0.8 | 5-8 d | Main build |
| 3 | S4 run kernels (+S1) | ~1.0x synthetic; 2-3x on real frames | 0.7 | 3-4 d | High value for the game, nil for the benchmark |
| 4 | S5 int8 planar | 2.0-2.3x | 0.6 | 6-8 d | Equal speed to S1 with more exactness risk |
| 5 | S3 TBL gather | 2.7x on <=64-colour bands, 1.4x otherwise | 0.45 | 10-15 d | Research option |

I would take forward S2 and S1. S2's experiment costs two hours and settles the memory-stall model every other estimate in this document depends on; S1 is exact by construction, ports to SSE2 without cleverness, and its ~18-cycle kernel leaves the memory system as the only remaining term, which is where S2's layout work then pays. S4 is the natural third step if captured frames show the run statistics I expect; S3 stays in reserve for the case where real bands prove to have small palettes and S1+S2 still miss the 8 ms budget.
