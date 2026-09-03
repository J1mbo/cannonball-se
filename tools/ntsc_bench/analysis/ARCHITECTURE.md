# Proposed re-architecture of the hires NTSC filter

Status: proposal, nothing implemented. Date: 2026-09-03.
Inputs: ALGORITHM.md, REVIEW.md, SHORTLIST.md (incl. section 8), GRAPHICS_SETUP.md,
the sl-d303 measurements in README.md, and the captured frames in `frames/`.

## 1. Design goals

1. Bit-identical output to the shipped `snes_ntsc_blit_hires_fast` (752-wide rows) on every
   input, gated by the existing bench checksums and by the captured frames.
2. Well under 8 ms per 224-row half frame on a Cortex-A53 at 1 GHz, on real game content,
   with no cliff when the palette is large.
3. Same source for NEON, SSE2 and RVV, plus a scalar fallback that is also bit-identical.
4. Less memory than today (32 MiB table plus a 573 KB conversion buffer).

## 2. What the evidence says the design must do

| Fact | Source | Design consequence |
|---|---|---|
| Filter is a 14-tap FIR on a 6-in/7-out grid; adjacent outputs read adjacent words | ALGORITHM.md 6.1-6.2 | vector lanes = adjacent outputs, loads are contiguous 16-byte windows |
| Even/odd pixels of a pair use the same 14-word block, odd delayed by one output | ALGORITHM.md 6.3 | a precomputed pair kernel handles an identical pair with one load |
| Flat colour gives a constant output for every position and every phase | ALGORITHM.md 3.6 | steady-state runs are a single precomputed RGBA per slot |
| Wrap-around 32-bit adds, no saturating arithmetic | ALGORITHM.md 6.5 | partial sums can be precomputed and stay exact |
| Scalar cost at 256 colours: 33 issue + 16 micro-TLB + 16 L2 cycles per output | README sweep, REVIEW addendum | need a cheaper kernel and page compaction and line reuse |
| No transparent huge pages on the target | README | page compaction must be done by the table layout |
| The emulator's frame buffer already holds slot indices 0..8191 | GRAPHICS_SETUP.md 6(a) | index the kernel table by slot; delete the per-frame colour conversion |
| Palette writes go through `convert_palette()` for exactly two rows each | GRAPHICS_SETUP.md 6(b) | regenerate kernels per slot on palette write |
| Real frames: ~100 colours, 65% uniform 6-pixel chunks, 91% identical even/odd pairs, 81% vertical repeats | frames/ statistics | specialise for pairs and runs; working set fits L1 |
| Tiles, text, road background are 2x2 doubled; only road foreground and sprites are true hires | GRAPHICS_SETUP.md 4 | the pair kernel is the common path, not a special case |

## 3. Architecture overview

```
 game logic thread                 frame boundary (main loop)              render threads (x2)
 ------------------                -----------------------------           -------------------
 palette RAM write                 regenerate dirty slots:                 for each row of my half:
   -> convert_palette(adr)           gen_kernel + correct_errors             phase = (phase0 + row) % 3
   -> rgb_blargg[adr], [adr|0x1000]  -> K[phase][slot]  (base blocks)        classify each 6-pixel chunk
   -> dirty.set(adr), (adr|0x1000)   -> PK, CK, FLAT   (derived sums)          steady run  -> splat FLAT[slot]
                                     (parallel over cores if many)             uniform     -> CK loads
                                                                               pair        -> PK loads
                                                                               general     -> K loads
                                                                             clamp + unpack per 4 outputs
                                                                             store 752 RGBA per row
 S16 frame: pixels[] = slot ids  ------------------------------------------>  read directly, no conversion
```

Four cooperating parts: (A) a slot-indexed kernel table with incremental regeneration,
(B) an adjacent-outputs vector kernel, (C) a run-length ladder that skips most of (B) on
real content, (D) a thin portable SIMD layer.

## 4. Part A: slot-indexed, phase-major kernel table

Layout: `K[phase 0..2][slot 0..8192][alignment 0..2][16 words]`, 64 bytes per alignment
block, 192 bytes per slot per phase, 4.5 MiB total.

- **Index = slot** (palette index, plus 0x1000 for shadow), exactly the value already in
  `pixels[]`. The `rgb_blargg[]` copy loop in `blargg_filter()` is deleted for the NTSC
  path; the blitter takes `pixels` directly. `rgb_blargg[]` itself stays as the colour
  source of truth for regeneration and for the non-NTSC path.
- **Slot 8192 is reserved for black** (colour value 0). The scalar and `_fast` code pad rows
  with `snes_ntsc_black = 0`, which is the colour black, not palette entry 0. The new code
  must use the black slot in the prologue and tail or it will not be bit-exact.
- **Phase-major** so that one row touches one contiguous plane (1.5 MiB) and rows r, r+3,
  r+6 share pages and lines. At 192 bytes per slot, 21 slots fit a page; a frame's ~180 live
  slots, clustered by 16-entry palette bank, occupy roughly 10 to 20 pages per row instead
  of up to 180 today. The measured 16-cycle micro-TLB term shrinks to a few cycles without
  any kernel support.
- **64-byte aligned 16-word blocks** (14 real words plus two zero words): every 16-byte
  window load an output group needs stays inside one cache line except the single case
  starting at word 13, which is handled with one lane-zeroing op. Line fills per output fall
  from about 1.4 to about 1.0 and no load crosses a line, which the A53 penalises. Cost: 14%
  padding, included in the 4.5 MiB.
- **Regeneration.** `RenderBase::convert_palette(adr)` additionally sets two bits in an
  8192-bit dirty set. At the frame boundary, after the game tick and before the render
  threads are released, the main loop regenerates every dirty slot with the existing
  `gen_kernel` + `correct_errors` code factored into `ntsc_gen_slot(table, slot, rgb555)`,
  then derives the tables of Part C for that slot. Cost is a few microseconds per slot:
  under 1 ms in the worst per-frame case seen in the code (128 rows during sky cycling),
  and a one-off hitch of a few tens of milliseconds at stage load if all 8192 change, which
  can be spread over the four cores with the OpenMP that `snes_ntsc_init` already uses.
  Startup generates 8192 slots, one eighth of today's 65536, so init gets faster.
- **Thread safety.** The dirty set is written on the logic thread and drained on the main
  thread at a point where the logic thread has finished its tick; the render threads only
  read the tables while the main thread is not writing them. This removes the unlocked
  race that exists today rather than widening it. A settings change (sliders, preset)
  marks all slots dirty.
- **Memory.** 4.5 MiB base table plus the derived tables of Part C (about 7 MiB) replaces
  the 32 MiB table and the 573 KB conversion buffer.

## 5. Part B: adjacent-outputs vector kernel (the general path)

Four consecutive output pixels per 128-bit register. The row is processed in units of four
chunks (24 input pixels, 28 outputs, 7 accumulator vectors) so that every load offset is an
immediate. For each of the 24 pixels the kernel loads the 16-byte windows of its 14-word
block that overlap the 7 vectors (4 to 5 loads) and adds them into the accumulators.
Windows that extend past the block's end read the zero padding words; the one window that
would cross into the next block is masked. Left edge: start the grid four outputs early
with black-slot pixels and discard the first vector; right edge: reproduce `_fast`'s tail
(two dropped input pixels, black padding, 24 outputs with the last four black) so the
752-wide checksums match.

Per output: about 3.8 loads, 3.8 vector adds, plus 5 clamp ops and 5 unpack ops per four
outputs. On the A53's 64-bit NEON datapath this is 15 to 20 issue cycles per output against
33 for scalar today, and the loads are issued a whole group ahead of use so L1 misses
overlap. Bit-exact by construction: the same words are summed, addition order does not
matter for wrap-around adds, and clamp and unpack are the scalar bit sequence per lane.

Estimated 1.7 to 2.5x over scalar on the random 256-colour benchmark by itself; it is the
floor the run-length ladder builds on.

## 6. Part C: the run-length ladder (what makes real frames fast)

Classify every 6-pixel chunk with a handful of scalar compares (three pair tests, one
uniform test, one same-as-previous test), then emit through the cheapest applicable path:

| Path | Condition | Table used | Work per 7 outputs |
|---|---|---|---|
| Steady run | chunk c uniform, chunk c-1 uniform, same slot | `FLAT[slot]`, one RGBA word | 2 splat stores |
| Uniform chunk | chunk c uniform, previous differs | `CK[phase][slot]`, 20-word chunk kernel | ~5 loads for the 6 pixels, previous chunk via B |
| Pair | even/odd pixels equal | `PK[phase][slot][align]`, 15-word pair kernel | ~5 loads per pair instead of ~9 |
| General | anything else | `K` | Part B |

Why each is exact: the output of a 12-pixel window that is all one colour is the same
value at every position and phase (ALGORITHM.md 3.6), so `FLAT` is exactly what the FIR
produces once clamped; `PK[n] = K[n] + K[n-1]` and `CK` is the sum of three shifted pair
kernels, and wrap-around addition is associative, so precomputed partial sums equal the
sum the general path would compute. All three derived tables are integer sums of the base
blocks, computed at regeneration time for the dirty slot only.

Sizes: `PK` 4.5 MiB (same shape as `K`), `CK` about 3 MiB (24-word blocks), `FLAT` 32 KB.
If memory matters more than the last bit of speed, `CK` can be dropped and uniform chunks
served by three pair loads.

Expected cost on the captured frames: with about half of all chunks in steady runs, 15%
uniform transients and the rest mostly pairs, average loads per output fall to roughly 1.5
and the kernel spends most of its time in classification, clamp and stores. A working
estimate is 10 to 15 cycles per output, i.e. 3.5 to 5 ms for a full 640x448 frame on one
core and 2 to 3 ms per half frame on two threads, against about 16 ms per full frame for
the shipped `_fast` on similar content. The random benchmark does not exercise this ladder
at all, so the bench needs an `--input frame.bin` mode before Part C is measured.

This ladder generalises the shipped `_fast` repeat-block shortcut from 24-pixel to 6-pixel
granularity and replaces its "broadcast the last sub-vector" trick with a precomputed
constant, which is also what the uncommitted `_fast2` got wrong.

## 7. Part D: portable SIMD layer and scalar fallback

The kernel needs six operations: unaligned 16-byte load, aligned store, 32-bit lane add,
byte shift within the register (`ext` / `_mm_slli_si128` / `vslideup`), and/or/shift by
immediate, and splat. Implement them as a small header (`v4u32`) with NEON, SSE2 and RVV
backends and a plain-C backend. The plain-C backend makes the non-SIMD build run the same
algorithm and produce the same checksums, so the current separate scalar hires path and its
different output width (749) can be retired once the new path is validated. RVV keeps the
option of an indexed-load variant later; nothing in the design depends on an ARM-only
instruction.

## 8. Row interleave and prefetch (second-stage tuning)

With the phase-major table, processing rows r, r+3 and r+6 together (same phase) lets the
lines fetched for one row be reused by the next two; the captured frames repeat vertically
81% of the time. It also gives the in-order core three independent load chains to
interleave. It is a loop-order change only and is left for after Part C, because the ladder
already removes most loads on real content; measure before adding it. Software prefetch of
the next chunk group's blocks is the other knob; both are tunable without touching the
tables.

## 9. Renderer changes outside the filter

1. `blargg_filter()`: drop the conversion loop and pass `pixels` to the blitter.
2. `convert_palette()`: set dirty bits. Main loop: drain dirty slots at the frame boundary.
3. `init_blargg_filter()`: allocate the new tables, generate all slots, mark clean.
4. Pass `(phase + 224) % 3` to the bottom-half call so the phase sequence is continuous
   across the two threads (today both halves start at the same phase). This is a visible
   behaviour change, small, and should be decided explicitly; the bench already assumes
   the continuous sequence.
5. Wide-screen: 808-wide rows give 134 chunks and a 2-chunk remainder; the group-of-four
   unroll needs a 2-chunk epilogue. Keep `_fast`'s tail semantics.

## 10. Validation

- Bit-exactness: every stage is compared to `_fast` at 086591c with the bench checksums on
  random 256 and 65536 colours, the mixed pattern, and all 150 captured frames at all three
  phases. The scalar fallback of Part D must match too.
- Performance: sl-d303, performance governor, pinned core, median of three, on random 256
  colours (Part B) and on captured frames (Part C), one and two threads.
- In game: the fps counter with `-perftest` on the Pi with hires and the composite filter.

## 11. Staged delivery and go/no-go gates

| Stage | Content | Effort | Gate |
|---|---|---|---|
| 0 | bench `--input frame.bin`; measure `_fast` and scalar on captured frames | 0.5 d | numbers for the baseline on real content |
| 1 | Part A with the existing scalar blitter re-pointed at slots; black slot; regeneration; conversion loop removed | 2 d | checksums equal; real-frame time drops (expect 1.3 to 1.5x) |
| 2 | Part B general kernel with the 28-output body; edges; Part D layer with NEON and SSE2 | 3 d | checksums equal; 16-colour row at or under 22 cycles per output, 256-colour at or under 34 |
| 3 | Part C ladder: FLAT, PK, then CK | 2 d | checksums equal on all captured frames; per-frame time on captured frames at or under 6 ms single-thread |
| 4 | Row interleave, prefetch, RVV backend, retire old paths | 2 d | no regression; two-thread half frame at or under 4 ms |

Each stage is shippable on its own and each gate reuses the kill experiments from
SHORTLIST.md. The uncommitted `_fast2` should be discarded rather than carried forward.

## 12. Risks

- **Steady-state constancy.** The claim that a flat field gives one value at every
  position rests on `correct_errors`; it is derived, not yet dumped. Stage 3's first
  checksum run on the mixed pattern settles it; the fallback is seven values per slot.
- **Black slot and tail semantics.** Easy to get subtly wrong; covered by the checksum gate.
- **Regeneration hitch at stage load.** Bounded by parallelising over cores; if still
  visible, regenerate lazily on first use with a per-frame cap and a black fallback for the
  one frame.
- **Branchy classification on an in-order core.** The chunk classifier adds about six
  well-predicted compares per seven outputs; if mispredicts on busy sprite rows cost more
  than expected, classify a whole 24-pixel group at once and fall back to Part B for the
  group.
- **Cache-line-crossing loads.** Avoided by the 64-byte block layout; confirm with the
  Stage 2 microbenchmark that unaligned in-line loads run at one per cycle.
