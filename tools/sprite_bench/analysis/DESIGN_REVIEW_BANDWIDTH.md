# Design review: the sprite renderer against the in-situ memory findings

Status: 2026-09-05, branch `ntsc-rearch` (in-situ harness on `sprite-a53`). Reviews
`DESIGN_FABLE.md`, `DESIGN_REVIEW_STAGING.md`, `MEASURED_A53_PI3.md` and `DESIGN_GLOW.md`
against what the road/tile work measured afterwards: `PHASE_PROFILE.md` and
`../../road_tiles/analysis/STRATEGY.md` section 2a. Analysis only; nothing is implemented.
Every figure is tagged **[M]** measured, with its source, or **[E]** estimated here.

## 0. Verdicts

| # | Question | Verdict | In one line |
|---|---|---|---|
| 1 | LINEBUF vs FANOUT | **CONFIRMED** (LINEBUF) | The win is a compute win, largest with an L1-resident destination; both shapes touch identical destination lines, so the contended term is common to both. The margin narrows under real load (est. 21% to ~10%) but does not flip. A/B the D' replay variant at S1, not before S1. |
| 2 | "Compute-bound" | **REVISED** | True of the shipped kernel in-game. Not true of the rewrite: the same ~2-4 cyc/px memory term that hides under 11.6 cycles of compute is 25-45% of a 5.7-cycle kernel. Target re-based from 0.47 ms to 0.55-0.75 ms typical. |
| 3 | Glow + shadow | **CONFIRMED**, two amendments | The end-of-sprite deferral already turns the glow read into an L1/L2 hit. Enable the per-row shadow bitmap from day one; lengthen the shadow prefetch distance. No batching, no consumer-side mask. |
| 4 | ROM working set | shadowinfo drop **REINFORCED**; flipped drop **HOLDS**; schedule tables **HOLD** (tie) | Cross-frame L2 residency is nil, so only per-frame touched lines count, and side-table lines are *load* misses that stall: 0.07-0.3 ms/frame [E] in the shipped kernel. |
| 5 | Other choices | band composition **NEEDS-DATA**; prefetch **REVISED**; staging model **retired**; S4/S5 order **NEEDS-DATA** | Section 6. |
| 6 | Profile first? | **YES**, about one day, three in-situ measurements | They fix the target and the S4/S5 order. They do not gate the store-strategy choice, which is already safe. |

## 1. Reading the contention numbers correctly

The brief's framing is that the synthetic thrasher overstated real contention (6.6x
inflation in the `DC ZVA` bench against 2.6x in the game). That is true as a *ratio* and
misleading as a *cost*, and the difference decides most of what follows. Per 64-byte line:

| condition | source | ns per line |
|---|---|---|
| flat fill, one core, nothing else running | STRATEGY 2a: 560 KB in 0.091 ms [M] | 10 |
| flat fill, 3-core thrasher | STRATEGY 2a: 0.60 ms [M] | 67 |
| road background in-game, `-t 1` (cold buffer, GPU active) | PHASE_PROFILE 0.265 ms / 4,581 first-touch lines [M] | 58 |
| road background in-game, 4 threads + GPU | PHASE_PROFILE 0.682 ms [M] | **149** |

The solo microbench line cost of 10 ns is 6.3 GB/s, several times what this SoC's DRAM
delivers, so that run was mostly absorbed by the 512 KB L2 (a 560 KB cyclic fill under
16-way random replacement keeps a high hit rate) [E]. The in-game single-thread baseline
is a genuinely cold buffer with the GPU on the bus, 5.7x dearer per line. So the 6.6x
ratio is large because its denominator was warm, not because its numerator was heavy. In
absolute terms the game under real load costs **2.2x more per cold line than the
thrasher** did.

The sprite bench agrees. `MEASURED_A53_PI3.md` describes its thrasher as one thread
doing strided RMW over 16 MB, lighter still; it added +0.69 cyc/px to REF and +1.34 to the
pure-store FLOOR [M]. In-game, like-for-like at 80,272 pixels, the shipped kernel went from
10.1 to 13.1 cyc/px between one thread and four: **+3.0 cyc/px** [M, instrumented build;
+2.6 from the uninstrumented 0.72 to 0.93 ms pair]. Real contention hits a REF-shaped
kernel about four times harder than the bench thrasher did.

The correct statement, used throughout: *real contention on cold lines is heavier than
any condition the sprite bench ran under*; what the thrasher got wrong was the direction,
not just the magnitude. The "+30% for compute phases, +157% for a streaming fill" split
still stands: it is the fraction of a phase's time that is memory-exposed that sets the
inflation, and the shipped sprite pass is mostly compute.

Two further facts from the road capture that the sprite design did not have: the sprite
pass touches **2,147 destination lines per frame, none of them first-touch** (STRATEGY
section 1, 100-frame capture) [M]; and the NTSC threads push about 1.0 MB/ms through the
L2 at 60 fps (15.8 MB table + two 573 KB buffers per 16.7 ms) [E from stated sizes], so a
line not re-touched within roughly half a millisecond is gone. The road foreground
(4,379 lines, the road rows) runs immediately before the sprites and most sprite pixels
land in road rows, so a good fraction of sprite lines are probably still L2-resident;
the sky-row lines written a millisecond earlier are probably not. That fraction is
unknown and is the single most valuable number to take (M-B below).

## 2. Q1: LINEBUF against FANOUT under real bandwidth. CONFIRMED.

Re-reason from what each shape does to the shared memory system. Both write every opaque
output pixel to the destination exactly once, in the same order along each row, so they
touch the same 2,147 lines and generate the same DRAM traffic: a read-for-ownership on
each partial line (sparse sprite spans do not trigger the A53's full-line write-streaming
that made the road fill read-free) plus one writeback. The A53 store buffer merges stores
to the same 128-bit window (MICROARCH section 4), so FANOUT's many narrow stores and
LINEBUF's few 16-byte stores collapse to the same bus transactions. Everything LINEBUF
adds, the line-buffer fill and the replay reload, is L1-private traffic on a per-core
cache the neighbouring threads cannot touch. The premise "destination writes are the
scarce resource, FANOUT writes them directly" therefore buys FANOUT nothing: it is not
writing fewer or different lines.

That is what the bench measured, in the one condition immune to any argument about
thrasher realism: with a **15 KB L1-resident destination and no misses at all**, LINEBUF
was 6.08 against FANOUT's 7.91 cyc/px, a 1.83-cycle (23%) compute gap [M]. The margin then
narrowed as memory pressure rose: 1.55 (21%) with the full buffer idle, 1.25 (15%) under
the thrasher [M]. The thrasher's increment was +1.21 for LINEBUF against +0.91 for FANOUT
[M]: FANOUT does absorb contention slightly better, plausibly because it interleaves
`r_v` miss streams with decode work while LINEBUF's replay bursts a whole span of stores
into the store buffer and waits.

Scaling both increments by the 2.2x real-versus-thrasher line cost of section 1 gives
LINEBUF 8.5 against FANOUT 9.4 in-game, a 0.9-cycle (~10%) margin [E]. For the ranking
to flip, FANOUT's 0.3-cycle contention advantage would have to grow six-fold. Where the
conclusion is robust: the compute gap exists at zero memory pressure and cannot be an
artefact of the thrasher. Where it is in doubt: the store-buffer mechanism narrowing the
gap is real and its in-game size is unmeasured; the A53 store-buffer depth is
undocumented (MICROARCH section 2), so no model settles it. Where the doubt should be
resolved: at S1, in-situ, by the D' variant (`DESIGN_REVIEW_STAGING.md` section 0: load
each 16-byte chunk once, store it to all `r_v` rows). D' keeps LINEBUF's wide stores and
compute while interleaving the row streams the way FANOUT does, so it is the shape that
would win if the store buffer turns out to be the limiter. It differs from D only inside
the replay loop. Requirement on the design: keep the replay behind one function so D and
D' are a switch under the checksum gate, and read the A53 store-stall event (0xE8, the one
`MEASURED` already used) for both.

Also note that `PROBLEM_STATEMENT.md` section 11a still records A2 (FANOUT) as the settled
choice; `MEASURED` reversed that and this review upholds the reversal. The slot-counting
model in the staging review predicted A2 ahead of D by 0.85 cyc/px on the A53 in the
compute-bound case and measurement found D ahead by 1.83; that model should not be reused
to re-litigate anything.

## 3. Q2: is the kernel compute-bound in the real game? REVISED.

For the shipped renderer, yes, and it is now measured rather than inferred: +29% from
one thread to four is the same band as the tile and road-foreground phases (+11% to +27%)
and nothing like the streaming fill's +157% [M]. Memory contention adds 0.21-0.24 ms to a
0.93 ms pass; even if every one of its 2,147 lines cost the road fill's 149 cycles fully
serialised, that is 0.32 ms, a third of the pass [E]. The bench's "compute-bound" label
was right for this kernel.

It stops being right for the rewrite. The memory term is a property of the lines touched,
not of the kernel, so it carries over roughly intact while the compute halves. Bounds on
that term for a typical 82,000-pixel frame at 1 GHz [E, from the measured line count]:

| assumption about the 2,147 lines | ms/frame | cyc/px |
|---|---|---|
| all L2 hits (road-foreground lines still resident) | 0.04-0.09 | 0.5-1.0 |
| the thrasher's 67 ns/line, no overlap | 0.14 | 1.8 |
| the in-game fill's 149 ns/line, no overlap | 0.32 | 3.9 |

The bench's 5.81 cyc/px already contains an idle floor of 0.82, so the in-game figure is
about 5.8 plus 1.5-3.0 of exposed contention: **7.3-8.8 cyc/px, 0.60-0.72 ms typical,
1.6-2.0 ms in the 224,000-pixel busy scene** [E]. The saving is 0.23-0.35 ms typical and
0.6-1.0 ms busy, against `MEASURED`'s 0.48 and 1.32. Still worth doing, particularly for
the busy scenes that decide frame drops, but the number the project is carrying is an
upper bound.

Two consequences for the plan. First, `MEASURED`'s "real headroom in the decode, do the
later compute stages" was drawn from a floor of 0.8-2.2 cyc/px that is now 2-4 in-game;
once S1 lands, the split between S4 (memory: prefetch, NEON replay) and S5 (compute: NEON
decode) must be decided from an in-situ compute/memory split, not from the bench.
Second, `DESIGN_FABLE.md` section 3.3's original estimate (memory floor 1.6-3.1 cyc/px,
adding to compute rather than hiding under it) was closer to the in-game truth than the
bench that "contradicted" it.

What settles it is measurement M-A in section 8: the shipped kernel redirected to a
wrapped 32 KB scratch destination, timed in-situ under four threads by the existing
`prof_ns_sprites`. The difference from the normal build is the exposed destination term
in the real game, and it transfers to any kernel that writes the same pixels.

## 4. Q3: glow and shadow on a contended bus. CONFIRMED, two amendments.

The glow clear as `DESIGN_GLOW.md` specifies it is one read-modify-write per visible
colour span-row, pushed to a 64-entry ring and flushed at ring-full or sprite end. The
read targets the pixel left of a span the same sprite stored to microseconds earlier; the
line containing a span's first pixel is a partial line, so it was allocated in L1 (the
write-streaming bypass that could push a line past the caches only engages after a run of
consecutive full-line writes, which a span start never is) [E from the A53 TRM behaviour
MICROARCH cites]. Sixty-four entries is roughly forty row-spans, tens of microseconds at
the rewrite's speed, during which the NTSC threads move some tens of kilobytes through L2:
no realistic eviction. The load is an L1 or L2 hit, the cost is issue slots, and the
count in-game is about 1,200 entries per typical frame (4,500 per 300K pixels scaled to
82K) [E]: 0.01-0.03 ms. The failure mode the contended bus adds is a flush point so late
that the lines have left L2, and 1,200 exposed DRAM reads at 200-250 cycles would be
0.25-0.3 ms [E]. The 64-entry ring already prevents that; `DESIGN_GLOW`'s item 7 (count
L1D refills attributable to the flush loop) should run in-situ with the harness rather
than in the standalone bench.

Amendment 1: enable the per-destination-row shadow bitmap (`DESIGN_GLOW` section 4,
listed as optional) from the start. Bit 12 is set only by shadow ORs, so a clear on a row
no shadow has touched this frame is a provable no-op; the filter is 56 bytes in L1 and one
`tst` per span-row, and it removes *loads*, which the new data says are the scarce kind.
Expect it to remove most clears in sky rows and all clears before the first shadow sprite
of the frame; the fraction is scene-dependent [E], so instrument it.

Amendment 2: the shadow path's one-row-ahead `pld` was sized against a 135-180 cycle miss.
Under load a miss on a cold line is 200-250 cycles [E, from 149 ns per streamed line plus
RFO], and one row of a 100-pixel span at the rewrite's 2-3 cycles per pixel is 200-300
cycles of cover: borderline. Prefetch two rows ahead, or issue the prefetches for all
`r_v` destination rows of a shadow source row at the top of that row. This is a constant,
tuned at S4 by the store-stall counter.

What does not change: the colour path stays store-only; the per-row "has shadow" skip is
inherent in the span records (rows without a shadow record read nothing) and needs no
side table; shadow ORs stay inline. Batching shadow ORs into the same ordered ring as the
clears would be correct (a sprite writes each address once, and the ring preserves the
shadow-before-clear order at `lo-1`) and would let several misses overlap, but the whole
shadow term is bounded at about 10% of pixels, a few hundred lines, under 0.1 ms even if
every line missed [E]. Keep it as an S4 option, not a design change. The consumer-side
shadow mask is still out of scope for the same reason `DESIGN_FABLE` gave, and the memory
finding makes it worse: the mask is one more buffer the NTSC threads would have to read
through a full L2.

## 5. Q4: ROM working-set choices. REINFORCED for the shadow table, HOLDS for the rest.

The L2 argument has to be made per frame, not per resident byte. With about 1 MB/ms
streaming through a 512 KB L2, nothing sprite-side survives from one frame to the next,
so the "pollution" cost of any table is exactly the lines it touches per frame, each
fetched once from DRAM, and, crucially, fetched by a *load*, which stalls the in-order
pipeline for the full miss, unlike a destination store that sits in the store buffer.

Per typical frame, from `FABLE_ASSESSMENT`'s 80-150 KB of ROM rows touched [E]:

| table | lines touched per frame | exposed cost at 170-250 cycles, no prefetcher help |
|---|---|---|
| `sprites[]` rows (irreducible) | 1,300-2,400 | 0.2-0.6 ms, mostly hidden if the stream prefetcher tracks the row stride |
| `sprites_shadowinfo` (1 byte per word address, read per row) | 320-600 | 0.05-0.15 ms |
| `sprites_flipped` | replaces forward lines for flipped sprites; net extra only for art drawn both ways in one frame (mirrored roadside pairs) | 0.02-0.15 ms [E, 10-30% of ROM lines] |
| `ZoomSched` (640 B per zoom, ~50 zooms) | 100-150 | 0.02-0.04 ms |

**`sprites_shadowinfo`: drop, reinforced.** The span design classifies shadow nibbles in
the SWAR test as a by-product of decoding the word it must read anyway; the table adds a
dependent byte load per ROM row per sprite that misses under contention. Worth 0.05-0.15
ms of the shipped pass [E]; measurement M-B's L2-refill count against the 2,147
destination lines will show how much of the shipped kernel's +3 cyc/px is load-side.

**`sprites_flipped`: drop, holds, weakly reinforced.** The per-frame line count is mostly
a wash because a flipped sprite reads one copy or the other; the gain is the mirrored-pair
case, the removal of the 1 MB lazy rebuild after every `reset()`, the removal of the
per-row sentinel check, and a smaller TLB footprint (the 2.25 MB of tables plus the
buffers exceed the 512-entry L2 TLB's 2 MB reach; every sprite start is a candidate
walk). On-the-fly mirroring is ~5 ALU ops per word, dual-issued, about 0.1 cyc/px [E]. The
decision stands on simplicity; the memory data neither undermines nor much strengthens it.

**`ZoomSched` static tables: hold, as a tie.** `DESIGN_FABLE` section 5 called static
tables versus per-sprite generation a 40 us tie; with load misses at 200-250 cycles
rather than 135 the static side is 0.02-0.04 ms and per-sprite generation (122 sprites x
~300 cycles) 0.04 ms [E]. Still a tie. Not worth changing; if S1 shows schedule-table
misses in the refill count, per-sprite generation into L1 scratch is the drop-in.

## 6. Q5: other choices that hinged on memory-versus-compute

* **Source-stream prefetch: REVISED, add it.** The design prefetches shadow destination
  rows and schedule tables; the load stream that actually stalls under contention is the
  ROM row sequence. It is linear within a sprite (`addr + pitch*j`), so the A53's
  prefetcher may catch it, but MICROARCH section 8 could not confirm a data prefetcher
  exists. One `prfm pldl1keep` per source row, two rows ahead, is free insurance and
  belongs in S1, with the cold start of each sprite prefetched at dispatch.
* **Colour-span destination prefetch (`prfm pstl1strm`, S4 optional): promoted to
  "measure at S1".** Store misses do not stall until the store buffer fills; the
  contended-bus data says fills take longer, so it fills sooner. The 0xE8 counter in M-B
  and at S1 decides.
* **Band composition (J / E8): NEEDS-DATA, and probably not for sprites.** Road foreground,
  sprites and text already run back-to-back over the road rows, which is a de facto band
  in L2. The sprite memory term is bounded at ~0.32 ms and is probably well below that;
  band composition is a cross-layer restructure the road work deferred as high risk. Only
  M-B showing the sprite lines mostly missing L2 would justify it, and then the
  cheaper fix is ordering (draw sky-row sprites first, while those lines are freshest).
* **Table encoding and the closed-form schedule: HOLD.** CF1 matching REF to 0.5% [M] shows
  the schedule form is not the lever either way.
* **Occlusion bitmap, multi-core sprite pass, merged COLOUR/MIXED loop: stay rejected**, for
  the reasons `DESIGN_FABLE` section 5 gave, all of which the memory data strengthens.
* **The S4-before-S5 ordering: NEEDS-DATA.** `DESIGN_FABLE`'s rule (a small S4 gain means
  the floor is reached, stop compute work) is the right rule; `MEASURED`'s advice to press
  on with compute stages was drawn from an unrepresentative floor.

## 7. What changes and what holds

Holds: LINEBUF as the structure; store-only colour replay; span records; static
`ZoomSched`; the glow as one deferred clear per span-row in a 64-entry ring; inline shadow
RMW; dropping both side tables; every rejection in `DESIGN_FABLE` section 5; the staged
order S0-S3.

Changes: (1) the performance target, from 5.7 to 7.3-8.8 cyc/px in-game, 0.23-0.35 ms
saved typical, 0.6-1.0 ms busy [E]; (2) the replay loop isolated so D' is a switch, A/B'd
in-situ at S1; (3) ROM source-row prefetch in S1; (4) the per-row shadow bitmap on by
default; (5) shadow prefetch distance two rows; (6) colour-span `prfm` measured at S1
rather than deferred to S4; (7) the S4/S5 order decided by an in-situ split after S1;
(8) `PROBLEM_STATEMENT.md` section 11a corrected to LINEBUF; (9) the staging review's slot
model retired as a predictor.

## 8. Profile first: yes, one day, three measurements

> **DONE 2026-09-05 — results in `MEASURED_INSITU_SPRITE.md`.** All three ran on
> sl-d303. M-A: typical exposed memory term 0.098 ms (compute-dominated, as predicted);
> extreme-load (300k-975k px/frame, real and sustained) 0.372 ms, above the 0.30 ms
> threshold. M-B: destination-store-bound, 60% of L2 traffic typical rising to 80%
> extreme; a flat ~1,000-2,100 load-side lines/frame from ROM + shadowinfo. M-C: cyc/px
> FALLS 33-41% under load (busy scenes are fewer, larger, more-zoomed sprites that reuse
> cache better) — but ms/frame still rises and the busy scenes carry the real 1-10% frame
> drops. Net: proceed as designed for typical content; build the D/D' store-strategy switch
> into stage 1, do S4 memory-side work alongside S5 not after it, and drop the side tables +
> add ROM prefetch in stage 1. The plan below is unchanged; the data upgraded several items
> from "measure at S1" to "build in at S1".


The store-strategy choice does not need gating: its basis is a compute gap measured with
no memory traffic at all, and the only mechanism that could erode it is testable at S1
with the checksum gate in place. What must be measured *first* is the size and the kind of
the in-game memory term, because that sets the target, the S4/S5 order, and whether the
side-table and prefetch items are worth their place in S1. All three use the harness that
exists (`prof_ns_sprites`, `CB_FRAME_CKSUM`, the four-thread 60 fps attract run on
`sl-d303`), need no new renderer code, and are throwaway diagnostic builds.

**M-A: exposed destination term.** Build the shipped renderer with its destination row
pointer masked into a 32 KB wrapped scratch buffer (row index `& 15`, same stride); output
is wrong, the checksum gate is off for this run only. Log `prof_ns_sprites` and the
per-frame pixel count over the full attract cycle at four threads, and the same for the
normal build. Decides: the exposed destination-memory cost per pixel in the real game,
which transfers to the rewrite. Rules: below 0.15 ms typical (< 1.8 cyc/px), the kernel is
compute-dominated in-game, proceed as designed and demote S4's memory items; 0.15-0.30 ms,
proceed, S4 before S5, colour-span `prfm` in S1; above 0.30 ms, the memory term will be
40%+ of the rewrite, so build the D/D' switch into S1 from the first commit and treat
sprite draw ordering by row band as a live S4 candidate.

**M-B: load-side against store-side.** `perf_event_open` around the sprite call in the
normal four-thread build: cycles, L1D refill (0x03), L2D refill (0x17), store-stall
(0xE8). Compare L2 refills per frame with the 2,147 destination lines. Decides: whether
the shipped kernel's +3 cyc/px is mostly destination lines missing L2 (refills near or
above 2,147) or mostly ROM and side-table loads (refills well above the line count with
low store-stall), and what fraction of destination lines hit L2 from the road foreground.
Rules: refills at or below ~2,500 with high store-stall means destination-bound, the
colour-span `prfm` and the D' A/B matter most; refills well above that with low
store-stall means load-bound, so dropping `sprites_shadowinfo` and the ROM prefetch are
worth more than `MEASURED` credited and belong in S1. Half a day including the counter
plumbing, which `MEASURED` already wrote once for the bench.

**M-C: per-pixel rate against load.** In the normal build, log per-frame `prof_ns_sprites`
beside the pixel count across the whole attract loop and under `-perftest`, and regress
cycles per pixel against pixels per frame. Decides: whether the busy 224,000-pixel scenes
run at the typical 11.6 cyc/px or worse, since `MEASURED`'s 2.60 ms busy-scene figure is a
projection, not a measurement, and the busy scene is the one that drops frames. Rule: if
the busy-scene rate exceeds the typical rate by more than 20%, the memory term grows with
load and the M-A thresholds above should be applied to the busy figure, not the typical.
This is a logging change only; it runs alongside M-A.

Then start S0 as `DESIGN_FABLE` specifies, with the additions in section 7. If M-A and M-B
together show an exposed memory term below 0.15 ms and destination lines mostly hitting
L2, the design proceeds exactly as written and the only residue of this review is the
re-based target and the D' switch. That is the likely outcome given the +29% in-game
inflation; the reason to spend the day is that the rewrite's headline number is currently
an idle-bench figure being quoted as an in-game saving, and one day of measurement is
cheaper than discovering that at S1.

## 9. Measured versus estimated, in one place

Measured: all bench cycle counts and gaps (MEASURED); the in-game phase times, the +29%
and +157% inflations, the 0.72/0.93 and 0.81/1.05 ms pairs (PHASE_PROFILE, MEASURED
in-situ); the 2,147 sprite lines and 4,581 road-background lines per frame (STRATEGY
section 1); the `DC ZVA` fill times (STRATEGY 2a); the run-length distribution and the
2.27 mean (SCHEDULE_VERIFICATION); the glow reduction to one clear per span-row
(DESIGN_GLOW, 30.2 M cases).

Estimated: every per-line cost in nanoseconds (derived from measured times divided by
measured line counts, at an assumed 1 GHz for the `DC ZVA` run); the 2.2x scaling and the
8.5-versus-9.4 projection; the 7.3-8.8 cyc/px in-game rewrite figure; the L2 residency of
sprite lines; the ROM, side-table and schedule line counts and their exposed costs; the
glow entry count in-game; the prefetcher's behaviour on the ROM stream; the store-buffer
depth. M-A, M-B and M-C replace the estimates that matter.
