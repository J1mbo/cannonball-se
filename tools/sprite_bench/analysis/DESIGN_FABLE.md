# Sprite renderer: independent design for Cortex-A7 / Cortex-A53

Scope: `hwsprites::render()` as specified by the upstream reference
(upstream `djyt/cannonball` `hwsprites.cpp`, cited below by line). Written without
reading the existing optimised implementation.

Facts established from the reference before designing (all verified
exhaustively by script, zoom 0x20..0x7FF, indices 0..1023, both axes):

* Horizontal: `N(i) = ceil(512*i/zoom)`, `r(i) = N(i+1)-N(i)`, accumulator
  reset per row (line 265), so the schedule is identical for every row.
  Vertical (lines 319-321) is the same law: destination row `d` reads source
  row `(d*zoom)>>9`. One byte table `r[0..511]` per zoom value serves both axes.
* Max run: zoom 0x80..0xFF gives 4, 0x100..0x1FF gives 2, >= 0x200 gives 0 or 1.
  Shipping hi-res default = lo-res table halved: 129 distinct zooms in
  0x80..0x1F8 (r in {1,2,3,4}, nothing skipped). "hiresprites" = hi-res table
  halved: 159 distinct in 0x80..0x3F0. Lo-res: 129 distinct in 0x100..0x3F0.
* `height` (line 206, header line 35) counts DESTINATION scanlines, doubled in
  hi-res (line 254). The number of output rows is therefore known up front;
  the last source row's vertical run is simply truncated. (The brief said
  "height in source rows"; the reference says otherwise and is authoritative.)
* Word-level control: the screen-edge test is per word (line 273) but pixels
  are clipped individually (line 176); the terminator is tested after the
  whole word is drawn (lines 288/313) and depends on read direction
  (`0xF0` forward, `0x0F000000` flipped). Flip reads words downward from
  `addr` with nibbles low-to-high (lines 296-310). `xdelta` chooses the
  destination direction independently of `flip`. Vertical mirroring is a
  negative `pitch` (line 212); `ydelta` only flips the screen direction.
* The word index is a `uint16_t` scratch cell (`ramBuff[data+7]`, lines
  275/300), so addressing wraps modulo 0x10000 inside the 64K-word bank.
* Colour write = `pix | color`; shadow = `(dst & 0xFFF) + 0x1000` on the
  existing value (lines 180-181). If bit 12 is never set by the background
  pass this equals `dst | 0x1000`; the harness must confirm that, otherwise
  keep the exact form (one extra `and`/`bic` per 8 pixels in NEON).

## 1. Irreducible work versus what the reference does

Per output pixel the problem requires: decide transparent / shadow / colour
for the *source* pixel it samples, and then either nothing, one 16-bit store,
or one 16-bit read-modify-write. Nothing else is inherent. In the hi-res
default each source pixel is sampled `r_h` (1-4) times per row and each source
row is sampled `r_v` (1-4) times, so any per-source-pixel decision costs
`1/(r_h*r_v)` per output pixel, typically 1/4 to 1/9 when weighted by pixel
count (the big near sprites dominate the count). The 4-bit decode, the
classification and the schedule are all per-source-pixel work, and the
schedule is per-zoom, not even per sprite. Per sprite the irreducible part is
the record parse, the clip arithmetic and one division for the first source
row.

Where the reference spends effort the problem does not require:

* Per output pixel (macro at 174-188): two clip compares, two transparency
  compares, a shadow compare, `x += xdelta`, `xacc += hzoom`, the `while`
  compare and its branch: ~12 instructions and 4-5 data-dependent branches.
  The `while (xacc < 0x200)` exit mispredicts whenever `r` changes (most
  pixels for most zooms), ~10 cycles a time on both cores. Estimate 15-18
  cycles per output pixel on A7, 10-12 on A53.
* Per source word (275/300): `spritedata[++ramBuff[data+7]]` is a 16-bit
  load, increment, store and then the data load: three load/store slots on
  the single LS pipe where one is needed.
* Per destination row: the entire source row is re-decoded and the schedule
  re-derived by the accumulator, although the schedule is invariant per
  sprite and the row content is invariant across its `r_v` copies. In the
  hi-res default this is 2-4x redundant decode.
* Shadow does a read-modify-write, which is unavoidable, but the colour path
  shares the same branchy macro, so the common path pays for the structure
  of the rare path.
* The list scan (196-202) runs once per priority level; negligible.

## 2. Recommended design

### 2.1 Shape

Decode each *visible source row* exactly once into row-local scratch (a
"line buffer" of expanded 16-bit values plus "span records"), then *replay*
it onto the `r_v` destination rows that sample it. Decode is L1-only work;
replay is a pure store stream for colour spans and a load/or/store stream for
shadow spans, with no per-pixel decisions. Clipping is per span. The
wide-store trick (always store `MAXRUN` copies, advance by the true run) is
used only inside the line buffer, where overrun is harmless; that is what
makes it viable, because transparency and frame-buffer overrun never see it.
The loop iterates over source pixels emitting runs (magnify regime); the
shrink regime is handled by the same loop with `r = 0` allowed.

### 2.2 Data layout and precomputation

* `ZoomSched`, one per distinct zoom value, filled lazily on first use with
  the reference accumulator loop (correct by construction, init-time cost):
  `uint8_t run2[512]` = `2*r(i)` in bytes (feeds post-indexed stores without
  a shift), `uint16_t wsum2[64]` = `2*(N(8k+8)-N(8k))` per source word.
  640 bytes per zoom; ~160 zooms worst case = 100 KB, of which a sprite
  touches only its width. Indexed by zoom value through a 2048-entry pointer
  array. The same table drives the vertical loop (`r_v(j) = run2[j]/2`).
* Two static 16-byte type tables: `TYPE_NOSHADOW[16] = {0,1,...,1,0}` and
  `TYPE_SHADOW[16]` with entry 10 = 2. Types: 0 gap, 1 colour, 2 shadow.
  Selected per sprite by pointer, not by template.
* Per-call scratch (~7 KB, stays in L1): `uint16_t line[2048+64]` (widest
  output row plus one word's worth of overrun, 32 pixels, plus `MAXRUN-1`)
  and `uint32_t rec[520]` = `(type << 16) | pos`, one record per type
  change, worst case one per source pixel plus two sentinels.
* Per-sprite scalars: `base2 = color | color<<16` (and `base4` on 64-bit),
  `pend = line + 2*K_max` where `K_max` = outputs until the destination
  leaves the screen (mirrors line 273 exactly), `srcdir`, `xbase`,
  `type_tab`, `sched`, `smask` (0 or all-ones for the SWAR test).

### 2.3 Dispatch (per sprite, once)

1. List filter as the reference (196-207); off-screen reject in both axes
   (whole sprite outside `[0,h)` or outside `[x1,x2)`): draw nothing.
2. Geometry: `H` destination rows, visible range `[d_lo, d_hi)`, first
   source row `j0 = (d_lo*zoom)>>9`, `N(j0)` by one division, then
   `N(j+1) = N(j) + r_v(j)` from the table.
3. Variant select: `MAXRUN` class from zoom (>= 0x200: 1, >= 0x100: 2,
   else 4) crossed with the `(flip, xdelta)` combination. Only combinations
   the game actually emits are instantiated (instrument first; expect two);
   anything else, plus zoom < 0x80 and rows that would cross the 64K-word
   bank wrap, falls back to the reference routine. Six instances of roughly
   400 bytes each.
4. Row loop: for each source row `j` with at least one visible destination
   row: decode once, replay onto rows `d` in
   `[max(N(j),d_lo), min(N(j+1),d_hi))` at `y = top + ydelta*d`, source
   words at `(addr + pitch*j) & 0xFFFF`. Source rows with `r_v = 0` (shrink)
   are never decoded.

### 2.4 Decode: one source row into line buffer + records

Word-classified. Per 32-bit word `w`:

```
w == 0                       GAP word:    pos += wsum2[k]; record if prev != GAP
w == 0xAAAAAAAA && shadow    SHADOW word: pos += wsum2[k]; record if prev != SHADOW
no nibble is 0 or F (or A when shadow)          [SWAR test, ~8 ALU ops]
                             COLOUR word: record if prev != COLOUR; 8 x {value,
                                          wide store, advance}; no terminator
                                          test needed (no F present)
else                         MIXED word:  8 x {type lookup, value, wide store,
                                          advance, branchless record}; then the
                                          terminator test
```

SWAR test with `C1 = 0x11111111` and `C8 = C1 << 3` from the barrel shifter:
`z = (w - C1) & ~w; f = (~w - C1) & w; xa = w ^ 0xAAAAAAAA;`
`a = ((xa - C1) & ~xa) & smask; mixed = ((z|f|a) & C8) != 0`.
The any-zero-nibble form has no false positives (only the which-nibble form
does), so the COLOUR path is exact. Word classes run in long runs
(interior vs edge), so the three class branches predict well.

Fast COLOUR path per pixel (forward nibble order, unrolled 8x):

```
n   = ubfx(w, 28-4*j, 4)
v2  = base2 | n | (n << 16)              // orr, orr-with-shift
store MAXRUN copies of v at pos          // MAXRUN=1: strh; 2: str; 4: below
pos += ubfx(run_lo_or_hi, 8*(j&3), 8)    // 8 run bytes fetched by one ldrd/ldr x
```

`MAXRUN=4` store: A32 `strd` requires word alignment and `pos` is 2-byte
granular, so use `vdup.32 dK, v2` + `vst1.64 {dK}, [pos], run2` (NEON
post-index by register in byte units, hence the `2*r` table), or two `str`.
A64: `v4 = v2 | v2 << 32` and one unaligned `str x`. In C this is
`vst1_u16(p, vdup_n_u16(v))` under NEON and two `memcpy(p, &v2, 4)` otherwise;
both compile to the above.

MIXED path per pixel, branchless:

```
t     = type_tab[n]                      // ldrb from a 16-byte table
v2, wide store, pos += run               // as above; gap pixels store harmlessly
*rp   = (t << 16) | pos_before           // always written
rp   += (t != prev)                      // cmp + predicated add / cinc
prev  = t
```

Then `if ((w & TERM_MASK) == TERM_MASK) break;` and, at the top of every
word, `if (pos >= pend) break;` (as line 273). After the row append the
sentinel `(GAP << 16) | pos`. Records mean `[pos_k, pos_{k+1})` has type
`type_k`; zero-length spans are legal and skipped by the replay (they arise
from `r = 0` pixels in shrink zooms).

Direction handling: `flip` changes the word step (-1), the nibble order (low
to high) and `TERM_MASK`; `xdelta < 0` fills the line buffer from its top end
downward (the wide store covers `[pos-MAXRUN+1, pos]`, then `pos -= run`) and
the replay uses `x = xpos - (LB_MAX-1) + idx`. Both are template parameters
of the decode only; the replay is direction-agnostic because records are
`[lo, hi)` index ranges either way.

### 2.5 Replay: records onto one destination row

```
for each record k with type != GAP:
    lo = max(rec[k].pos, cx0); hi = min(rec[k+1].pos, cx1)   // clip in index space
    if (hi <= lo) continue
    dst = row + xbase + lo; src = line + lo; n = hi - lo
    COLOUR: copy n pixels in 16-byte chunks (NEON vld1q/vst1q or ldrd/strd
            pairs), final chunk re-based to hi-8 so it overlaps the previous
            chunk and never passes hi; n < 8: two overlapped 8-byte or 4-byte
            stores; n == 1: strh. Pure stores; the destination is never read.
    SHADOW: n pixels of {load, orr 0x1000, store} in 16-byte chunks with
            scalar head/tail; issue pld/prfm for the same span on the row
            where the NEXT source row first lands (y + ydelta*r_v(j)) so its
            miss overlaps this row's decode.
```

`cx0/cx1` are `[x1,x2)` converted to line-index space once per sprite. Copies
never write outside their span because a span end is always either a gap
(must not be touched) or a shadow span (must read the pre-sprite value); the
end-overlap trick stays inside the span, which is why it is safe where a
trailing overrun would not be.

Every store on the colour path is a write to a line the core does not need to
read; on an in-order core that is the property that matters: store misses
queue in the store buffer and do not stall until it fills, whereas a load miss
stalls the pipeline for the full 100-150 ns. The shadow path is the only
consumer of that latency and is prefetched.

### 2.6 Register budget, A7 fast COLOUR path (13 of 14)

`pos`, `w`, `srcp`, `pend`, `base2`, `run_lo`, `run_hi`, `n`, `v2`, `rp`,
`prev`, `C1`, `sched` (one pointer; `type_tab` is addressed off `sp` in the
MIXED path, `wsum2` off `sched`). `0x88888888` and `0xAAAAAAAA` are derived
from `C1` by shifted operands. The replay is a separate loop with its own
allocation. Expect the compiler to spill one or two values in the MIXED path;
that path is LS-bound anyway so the spill costs little. A53 has no pressure.

## 3. Cycle estimates

Assumptions: A7 issues ~1 instruction/cycle with about 25% pairing (LS+ALU,
branch+ALU); A53 sustains ~1.8 IPC on unrolled independent ALU work; both
have one LS slot per cycle and 128-bit NEON loads/stores cost two slots.
Miss latency 150 ns = 135 cycles at 900 MHz, 180 at 1.2 GHz. The
pixel-weighted mean magnification in the hi-res default is taken as
rho = r_h = r_v = 2.5 (zoom around 0xCD); big near sprites dominate the count.

### 3.1 Decode, per source word (8 pixels), MAXRUN = 2

| class | ALU | LS | branch | A7 cycles | per src px | A53 cycles | per src px |
|---|---|---|---|---|---|---|---|
| COLOUR (SWAR pass) | 49 | 10 | 4 | ~49 | 6.1 | ~35 | 4.4 |
| MIXED | 84 | 26 | 5 | ~85 | 10.6 | ~61 | 7.6 |
| GAP / all-SHADOW | 9 | 2 | 3 | ~12 | 1.5 | ~8 | 1.0 |

COLOUR breakdown: word load and 8 run bytes (2 LS), index step (1), three
class tests (3 cmp + 3 branch + 8 SWAR ALU), record check (4 predicated), 8 x
(ubfx, orr, orr, ubfx-run = 4 ALU + 1 LS str post-indexed), loop (3).
MIXED adds per pixel a type load, a record store, cmp and predicated add, and
a prev move: 8 ALU + 3 LS per pixel, so it is LS-bound at 26 slots per word
even on A53. MAXRUN = 4 adds one slot per pixel on A7 (vdup) and none on A53.

Class mix for a typical hi-res frame, by source word: 60% COLOUR, 25% MIXED
(row ends, holes), 15% GAP/SHADOW. Weighted: A7 6.5, A53 4.7 cycles per
source pixel.

### 3.2 Replay, per output pixel

Colour copy: one 16-byte load (L1 hit) plus one 16-byte store per 8 pixels =
4 LS slots, 0.5 cycles per pixel on both cores (the scalar ldrd/strd form is
the same 0.5). Per span-row ~15 cycles (clip, class, setup); with 1.5 spans
per 100-pixel row that is 0.22 per pixel. Shadow: load, vorr, store = ~0.6
per pixel of issue plus exposed miss latency 135/32 = 4.2 per pixel if
unprefetched, assumed 1.5 residual with the one-row-ahead prefetch.

### 3.3 Per output pixel, hi-res default

| term | A7 | A53 |
|---|---|---|
| decode 6.5 / rho^2 (4.7 / rho^2) | 1.04 | 0.75 |
| per-source-row overhead ~50 cycles / (rho * K=100) | 0.20 | 0.14 |
| per-row-copy overhead ~30 cycles / K | 0.30 | 0.20 |
| colour replay incl. span setup | 0.72 | 0.60 |
| shadow share (10% of pixels at 2.1 / 1.8) | 0.21 | 0.18 |
| **compute total** | **~2.5** | **~1.9** |

Memory: 2 bytes per output pixel plus read-for-ownership on partial lines,
say 1.3x, so 2.6 bytes per pixel. At 1.0-1.5 GB/s effective for one core on
the contested bus: 1.6-2.3 cycles per pixel at 900 MHz, 2.1-3.1 at 1.2 GHz.
The store buffer overlaps at most a few lines of drain with the next row's
decode, so the floor and the compute add rather than max. Estimated wall
cost: A7 3.5-4.5 cycles per output pixel (300 K pixels = 1.2-1.5 ms at
900 MHz); A53 3.5-4.5 (0.9-1.1 ms at 1.2 GHz). Reference at 15-18 (A7) /
10-12 (A53) plus the same memory floor: 5-6 ms and 3-4 ms. Expected
speed-up 4x wall, 7x in issue slots.

Lo-res and "hiresprites" shrink sprites have r_v <= 1 and r_h <= 1, so decode
is not amortised: up to 13 cycles per output pixel at zoom 0x3F0 on A7. Those
sprites are small; the lo-res frame is 4x fewer pixels overall, so ~0.5 ms.

Weakest parts of the estimate, in order: (1) the memory floor under
contention, which is a guess at effective single-core write bandwidth with
two other cores streaming 16 MB per frame through the same L2 and the GPU on
the bus; the "stores-only" synthetic in stage 0 replaces it with a
measurement; (2) rho, which sets the decode amortisation and is scene
dependent; (3) the class mix (COLOUR versus MIXED), which depends on sprite
art; (4) A7 pairing rate, taken as 25%.

## 4. Where A7 and A53 differ, and one implementation

A7 wants: the LS pipe protected (MIXED is 3 LS per pixel, so keep COLOUR and
MIXED as separate loops rather than one branchless loop that would put the
record store on the common path); no `strd` at 2-byte alignment, so
MAXRUN = 4 stores go through `vst1.64` or two `str`; 8 run bytes via one
`ldrd`; constants derived from one register by shifted operands to fit 14
registers; word-class branches only (the predictor is weak, per-pixel
data-dependent branches are ruinous); `pld` for the schedule table at sprite
start since a table miss is a load stall.

A53 wants: 64-bit GPRs (`ldr x` for the run bytes, unaligned `str x` for four
copies, no NEON needed in decode); dual issue makes the SWAR test and value
math nearly free, so MIXED could be made cheaper by moving the type lookup
into ALU form (`(0x7FFE >> n) & 1` plus the shadow bit) to relieve the LS
pipe; `tbl` over 16-byte tables makes a NEON expansion stage twice as
productive as on A7; `prfm pstl1strm` on colour spans if the store buffer
proves to be the limiter; the L1 write-streaming detection makes full-line
colour spans cheaper than on A7.

One source serves both: the design is expressed in C++ templates over
`<MAXRUN, FLIP, XDIR>` with exactly two `#ifdef` islands, the MAXRUN = 4 wide
store and the 16-byte `tbl` in the optional stage 5. Everything else is
identical, and the scalar ARMv6 / MSVC builds are the same code with the
islands' scalar branches (two `memcpy` of 4 bytes; scalar copy loop).

## 5. Alternatives considered and rejected

* **Write output pixels straight from the decode (no line buffer).** Loses
  the vertical reuse (2-4x redundant decode in hi-res), forbids the
  wide-store trick because overrun into the frame buffer past a transparent
  neighbour or the row end is a visible bug, and forces per-pixel clipping.
* **Masked blend replay (line buffer + mask, `vbsl` against the destination).**
  Reads the destination on the common path; each 64-byte line is a load
  stall of ~135 cycles on an in-order core, about 4 cycles per pixel, more
  than the whole rest of the design. The store-only replay with spans avoids
  it entirely.
* **Copy the first destination row to the other r_v-1 rows with memcpy.**
  Copies background pixels under the transparent parts of the sprite: wrong.
  Spans give the same effect correctly.
* **Static per-zoom NEON shuffle-index tables.** 160 zooms x up to 2 KB =
  270 KB of L2 pollution that the neighbour cores evict every frame; misses
  on them are load stalls. If NEON expansion is adopted, generate the index
  vector per sprite (`((d*zoom)>>9) & 7`, K bytes, about one row's cost).
* **Per-sprite schedule generation instead of per-zoom static tables.** A
  tie at ~40 us per frame each way (122 sprites x ~300 cycles versus 1-2 line
  misses per sprite). Static chosen because the vertical loop needs the same
  table and the generator is the reference loop, hence trivially correct.
* **Branching on the run length per pixel** (`switch (r)` with exact stores).
  Run patterns are periodic with periods from 3 to over 100; A7's predictor
  does not learn them, so ~0.5 mispredicts x 10 cycles per source pixel.
  Wide stores cost one slot and are branch-free.
* **Output-pixel iteration for the shrink regime.** Roughly halves decode
  cost when r < 1, but only lo-res and "hiresprites" have that regime and
  those frames are small. Deferred to stage 6.
* **Reverse-order drawing with an occlusion bitmap to remove overdraw.**
  Attractive because the frame is memory-bound and overdraw may be ~2x. But
  the 36-72 KB mask is evicted by the 16 MB/frame streaming neighbour, so
  mask read-modify-writes become load misses (~1150 lines x 150 ns = 0.17 ms
  per sweep), close to the bandwidth it saves, and the shadow rule needs a
  second mask bit and a deferred OR. Rejected until overdraw is measured
  (a counter in stage 0); revisit only if it exceeds 2x.
* **Deferred shadow via a mask applied by the consumer pass.** The cheapest
  possible shadow, but it changes the buffer contract of the video filter;
  out of scope here, noted for the project.
* **Merging COLOUR and MIXED into one branchless loop.** Puts the record
  store and type load (2 LS) on every pixel: +40% on the common path for the
  sake of removing three well-predicted branches per word.
* **Non-temporal or cache-bypass stores.** ARMv7 has none; A64 `stnp` is a
  hint. The A53 write-streaming detector already covers full lines and
  partial lines need the read regardless. Not a lever.
* **Spreading the pass over cores.** The other three are busy and the pass
  is bandwidth-bound on a shared bus; it would only add contention.

## 6. What must be proven, and how

1. **Scaling law.** Exhaustive: reference accumulator versus `ceil(512*i/zoom)`
   and `(d*zoom)>>9` for every zoom 0x20..0x7FF and i, d < 1024, both axes.
   Done during this design (0 mismatches); keep it as a unit test.
2. **Schedule tables.** Generated by the reference loop, so correct by
   construction; assert `sum(run2)/2 == N(512)` per zoom and `wsum2[k] ==
   sum(run2[8k..8k+7])`.
3. **SWAR classifier.** Exhaustive over all 2^32 words against the
   per-nibble definition, both shadow modes: ~40 s in C, run once per change.
4. **Bit-exact output.** (a) Sprite-RAM snapshots recorded from real play in
   lo-res, hi-res and hiresprites, rendered by the reference and the new
   routine into identical pre-filled backgrounds (including bit 12 set, to
   settle the `or` versus `and/add` question), whole-buffer memcmp per
   frame. (b) Fuzzing: random records with edge-biased `xpos`/`top`
   (off-screen, straddling `x1`/`x2`, straddling the 16-bit word wrap), all
   flip/xdelta/ydelta/pitch/shadow combinations, over the real ROM and over
   a synthetic ROM dense in 0/F/A nibbles with terminators at every nibble
   position and rows longer than the pitch. Coverage counters per word class
   and per template variant must all be non-zero.
5. **Overrun.** Canary words after `line` and `rec`, checked per sprite in
   debug builds; ASan in the harness; debug assertion that no replay store
   falls outside `[x1,x2)` or rows `[0,h)`. The analytic bound is
   `K_max + 32 + MAXRUN - 1` entries.
6. **Scratch cell `ramBuff[+7]`.** Determine by search whether the 68K side
   ever reads it back; if it does, compute the final word index once per
   sprite (it is the last word index read on the last visible row).
7. **Variant coverage.** Counters of `(flip, xdelta, MAXRUN class)` over
   recorded play decide which templates are instantiated; every uncovered
   combination routes to the reference so correctness never depends on
   the instrumentation being complete.
8. **Performance attribution.** A "stores-only" synthetic (same spans,
   constant data) measures the memory floor per frame on each Pi; running
   the real routine into a 32 KB destination separates compute from memory.
   Both cores, perf counters (cycles, L1D refills, L2 refills) via
   `perf_event_open`, with the NTSC filter running on the other cores to
   reproduce contention.

## 7. Staged build order

Each stage is a compile-time switch defaulting to the previous stage;
reverting is flipping the switch. The bit-exact suite gates every stage.

* **S0 - harness.** Reference kept as the oracle and the fallback; golden
  frames; overdraw, class and variant counters; the stores-only floor.
  Kill: none, it is infrastructure.
* **S1 - schedule tables, one decode per source row, vertical replay.** Simple
  per-pixel decode with exact-length stores and branchless records; scalar
  span copy. Expect >= 2.5x over the reference on hi-res A7 from the row
  reuse and the removal of per-pixel branching. Kill: < 1.5x means the
  cost model is wrong; stop and re-measure before building anything else.
* **S2 - wide stores and MAXRUN variants.** Kill: < 15% over S1 on both cores.
* **S3 - word classes** (GAP, all-SHADOW, SWAR COLOUR fast path). Kill: < 10%.
* **S4 - NEON replay copy, NEON shadow OR, one-row-ahead shadow prefetch,
  optional `pldw`/`prfm` on colour spans.** Kill: < 5%; a small result here
  is the signal that the routine is at the memory floor, at which point
  further compute work is pointless and effort moves to bytes written.
* **S5 - NEON expansion in decode** (`vtbl2` on A7, 16-byte `tbl` on A53,
  per-sprite index vectors). Kill: < 10% on both cores or any A7 regression.
* **S6 - output-iteration variant for zoom >= 0x200** (lo-res, hiresprites).
  Kill: < 20% on lo-res frames, or lo-res deprioritised by the project.

Order rationale: S1 captures most of the gain with the least code and proves
the model; S2-S3 are local to the decode loop and individually measurable;
S4 tells whether the memory floor has been reached before S5 spends effort
on compute that may already be hidden.
