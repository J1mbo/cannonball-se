# Proposed re-architecture of the hires NTSC filter

Status: proposal, nothing implemented. Date: 2026-09-03.
Inputs: ALGORITHM.md, GRAPHICS_SETUP.md, the (since-retired) idea-shortlist and review notes,
the test mule measurements in README.md, and the captured frames in `frames/`.

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
- Performance: test mule, performance governor, pinned core, median of three, on random 256
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
the retired idea shortlist. The uncommitted `_fast2` should be discarded rather than carried forward.

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

## 13. Implementation notes (deviations from this proposal)

This section is retrospective, added once Stages 1-4 landed (see `README.md`'s "Current
state" section and its per-stage narrative below that for the full detail and measurements
behind each line). Kept short deliberately - it exists to flag *where* the shipped design
differs from the plan above, not to re-derive why; `README.md` is the source of truth for
numbers and rationale.

- **20-word blocks, not 16.** Section 4 above specifies a 16-word (14 real + 2 zero) block.
  The vector kernel actually shipped (Stage 2) needs a compile-time-constant 16-byte window
  landing anywhere from 3 words before the first real tap to 3 words after the last, which a
  2-word trailing pad cannot cover in every case; the block grew to 20 words (3 lead + 14
  real + 3 trail) instead. `snes_ntsc_block_words`/`snes_ntsc_block_lead` in `snes_ntsc.h`.
- **~15.8 MB table, not ~11.5 MB.** A consequence of both the wider base block above and
  Part C's own derived tables (`plane_pk`/`plane_ck`/`plane_flat`) needing correspondingly
  wider trailing pads for the same reason (a residue-2 window reaches one word further per
  table). Section 4's 4.5 MiB base + section 6's ~7.5 MiB derived estimate becomes 5.9 MB +
  10.6 MB = 16.5 MB in practice (`sizeof(snes_ntsc_t)`, confirmed by the bench).
- **Pair level (PK) off by default.** Section 6 lists the pair path as one of four
  unconditional levels. Stage 3's own mask experiment measured it giving nothing on real
  frames (`--input`) and a 32% regression on transition-heavy content (`--pattern runs`) -
  the extra pair-equality branch and table lookup outweighs its saving once dispatch cost on
  an in-order core is accounted for. `SNES_NTSC_LADDER_MASK` defaults to 3 (flat/skip + CK)
  as of Stage 4; the code and table are kept, off by a compile-time default rather than
  deleted, so a build tuned for busier content can re-enable it.
- **Prologue and tail stay on the general path (Part B), not the ladder (Part C).**
  Section 6 does not carve these out explicitly. They are at most 2 of a row's 26-33 groups,
  so shortcutting them saves little, and excluding them removes a real class of edge-case
  risk (black-padding interacting with the classifier, and the tail's short/spill semantics)
  - see README's Stage 3 "Scope decision" for the bug this avoided when it was tried anyway.
- **Lookahead-free classifier, not a row-wide pre-pass.** Section 6's classification is
  described as a per-chunk test with no explicit statement of whether it needs lookahead;
  the Stage 3 implementation used a row-wide pre-pass (`std::vector` buffers, one full pass
  over every pixel before any accumulation) so a chunk's flat/skip decision could look ahead
  up to 2 chunks. Stage 4 replaced this with a state machine carried forward across chunks
  in processing order (run length + up to two pending "skipped, not yet accumulated" chunks,
  resolved retroactively when a run ends) - no row-wide table, no heap allocation, and the
  ladder now costs nothing extra on content it cannot shortcut. See the banner comment above
  `snes_ntsc_blit_hires_vec()` in `snes_ntsc.cpp` for the full state-machine design.
- **Clamp+convert is a direct build of the final RGBA word, not clamp-then-bswap.** Neither
  section 5 nor 7 specifies the convert step's exact instruction sequence. The Stage 3
  review found the original ABGR-build-then-`bswap32` route (~20 ops) could be replaced with
  three shifts/ands/ors building the little-endian word directly from the clamped value -
  verified bit-for-bit equivalent, applied in `snes_ntsc_simd_clamp_convert`
  (`snes_ntsc_simd.h`) and reused by every FLAT/general/tail emit path.
- **Measured results vs. the estimates above.** Section 5's "15-20 issue cycles per output"
  and section 6's "10-15 cycles per output, 3.5-5 ms per frame" were pre-implementation
  estimates. Measured (test mule, see README): Part B alone (random 256, no ladder shortcut
  available) is 40.4 cycles/output, not 15-20 - the general kernel is more expensive in
  practice than estimated, though still faster than scalar (56.3) and `_fast` (49.4). With
  the full ladder on real content (`--input`), `vec` lands at 9.26 ms/frame total (6.77 ms
  filter-only) - inside the brief's real-content target range but not quite section 11's
  "at or under 6 ms" gate, and still slightly behind `_fast`'s 8.55 ms on the same content
  (`_fast`'s coarser 24-pixel repeat-block shortcut remains competitive on this specific
  metric even though `vec` wins everywhere else measured - random, mixed, 16-colour
  cycles/output, and portability to non-SIMD targets `_fast` cannot reach at all).

## 14. Reviewer's guide: what changed and why

This section is the reviewer's-eye comparison of the shipped code against the pre-rework
baseline (commit `086591c`), organised by topic - table, kernel, run-length shortcut,
deleted scaffolding, an unrelated fix, and what stayed the same - rather than
chronologically; see `README.md`'s Stage 0-4 log (and section 15 below) for the
chronological account and the bugs found along the way. It assumes intricate working
knowledge of the filter as it stood at commit `086591c`: `snes_ntsc_blit_hires_fast`, the
per-ISA `SNES_NTSC_HAVE_SIMD` dispatch, `CHECK_ALL_EQUAL_SSE4/NEON/AARCH64/RVV`,
`INSERT_/EXTRACT_SIMD_REGISTER_VALUE`, `SNES_NTSC_CLAMP_AND_CONVERT_*`, and the
65536-entry colour-indexed `snes_ntsc_t::table`; it does not re-derive that context. It
covers commits `137ad66`..`0bdb9ff` on the `ntsc-rearch` branch:

`137ad66` (SSE4 bugfix, pre-existing code) -> `7ec34e5` (table) -> `810a4be` (vector
kernel) -> `a6dfa98` (run-length ladder) -> `583b689` (retire old paths) -> `6a46028`
(review fixes) -> `0bdb9ff` (unrelated threading fix, see 14.6).

### 14.1 The one-line answer

The old code's SIMD idea - *pack four output accumulators, but fill each lane by
scalar table-gather-then-insert* - is **replaced**, not extended. The new kernel
instead *loads a contiguous 128-bit window straight out of the table into the
accumulator*, which only works because the table layout changed first (14.2) to make
that window exist and be safe to read. The old code's *chunking* (24 input pixels /
28 outputs per iteration, 7 output accumulators) and its *repeat-run shortcut concept*
are both preserved, but the run-length mechanism is generalised into a three-tier
ladder driven by precomputed derived tables rather than one runtime vector-compare
(14.4). The per-ISA macro scaffolding itself is deleted outright and replaced by one
~12-op portable vector layer (14.5).

### 14.2 Table: colour-indexed -> slot-indexed (not a SIMD change at all)

This is the precondition for everything else, and it's a data-flow change, not a
vectorisation change.

- Old: `table[0x10000][128]`, indexed by the 16-bit packed colour value
  (`SNES_NTSC_S16(ktable, n)` = `n` used directly as the row). Populated by
  `rgb_blargg[]` in `RenderSurface::blargg_filter()` doing one lookup per pixel,
  every frame, to convert the emulator's palette-slot buffer into colour values
  before the blitter ever sees them.
- New: `plane[3][8194][slot_words]`, indexed by the *palette slot* the emulator's S16
  buffer already contains natively (0..4095, +0x1000 for shadow). The `rgb_blargg[]`
  per-pixel conversion is gone; the blitter reads the game's own pixel buffer
  directly. Regeneration is incremental: `RenderBase::convert_palette()` marks the
  (at most two) affected slots dirty, and `RenderSurface::flush_ntsc_palette()`
  regenerates just those slots from the main thread at the frame boundary, before
  either render thread is released - this also closes a real pre-existing unlocked
  race, where the old code wrote two `rgb_blargg[]` words directly into a table a
  render thread might be concurrently reading.
- Consequence for reviewers: `gen_kernel`/`correct_errors`/`merge_kernel_fields` are
  untouched byte-for-byte; only what surrounds one generated entry (how it's
  addressed, and that it can be regenerated one slot at a time) changed.
- Each slot's block gained padding (16 words, later 20) around the 14 real kernel
  words. The old table had no such padding - it didn't need it, because the old
  kernel never issued a vector *load* against table memory (see 14.3). The padding
  exists solely so the new kernel's unaligned windows never read outside a slot's
  own memory or need masking.

### 14.3 The kernel: `snes_ntsc_blit_hires_fast` -> `snes_ntsc_blit_hires_vec`

#### What the old kernel actually vectorised

Worth stating precisely, because it's the crux of "preserved vs. replaced": the old
kernel already grouped work into **4 sub-blocks of 6 input pixels -> 28 outputs**, held
in **7 accumulator registers of 4 lanes each** (`xmm10..xmm16`) - the same chunk
granularity the new kernel uses. But within that structure, every output value was
computed as **12 scalar table reads and adds** (`SNES_NTSC_COLOR_IN` doing pointer
arithmetic per pixel, `SNES_NTSC_HIRES_OUT_SIMD_` summing scalars), and the resulting
scalar sum was then placed into its accumulator lane with
`INSERT_SIMD_REGISTER_VALUE`. The *only* place the vector unit did real arithmetic on
more than one output at a time was the final clamp/convert/store. In other words: SIMD
was used as a **packing container for scalar results**, not as compute. This is also
why `_fast2` existed as an abandoned, uncommitted A7-tuned variant before this project
started - its stated goal was to stop *extracting input lanes back to GPRs* for the
gather (which stalls an in-order core), while still keeping the same scalar-sum,
insert-into-lane structure for the outputs. It's a half-measure along the same axis,
not the axis change described next, and none of its code survives on this branch (it
also had a bit-exactness bug in its repeat-block reuse, unrelated to the phase fix in
14.6).

#### What the new kernel does instead

`snes_ntsc_blit_hires_vec` exploits a fact about the table layout that the old code
never used (it's proven in `ALGORITHM.md` section 6.2): **adjacent output positions read
adjacent words within a kernel block.** So instead of computing a scalar sum and
inserting it, each pixel contributes 4-5 **unaligned 128-bit loads** taken directly
from its table block, added straight into the 7 accumulators with vector adds. There
is no per-tap scalar gather-and-insert; the "gather" is reduced to one 16-bit load per
pixel (to find its slot) plus integer address arithmetic, and the FIR summation itself
is genuine vector work. Loads per output fall from 12 scalar reads to roughly 3.6
vector loads. The 24-pixel/28-output/7-accumulator chunk shape carries over unchanged
from the old kernel, including how a group's tail 3 accumulators are carried into the
next group's leading 3 - that bookkeeping pattern is inherited, not reinvented.

#### Portability consequence

Because the new kernel's only primitives are unaligned load, vector add, and a handful
of bitwise/shift ops for clamp+convert, it needs no per-ISA gather trick and no
SSE4.1-specific instruction (`_mm_extract_epi16`/`insert` are gone). One kernel body,
one `snes_ntsc_simd.h` layer (14.5), covers NEON/SSE2/RVV/plain-C.

### 14.4 The repeat-run shortcut: generalised, not just ported

- Old: `CHECK_ALL_EQUAL_{SSE4,NEON,AARCH64,RVV}` vector-compares a fixed 24-pixel
  window each iteration; if uniform, it broadcasts the *previous* iteration's already-
  clamped 7-vector output instead of recomputing. One shortcut, one granularity
  (whole 4-chunk group), detected fresh every iteration by a vector reduction. (The
  SSE4 feeder for this check - `ROTATE_OUT_SSE4` - had a real, independent bug found
  and fixed at `137ad66`: its two byte-shifts were swapped, so on x86 it sampled four
  pixels from the wrong side of the load boundary. NEON was correct. Synthetic test
  patterns never triggered it; real game content with long flat runs did. This is a
  fix to pre-existing code, unrelated to the redesign, and is the one place bit-
  exactness against the *original* `_fast` output required accepting a corrected
  reference rather than the shipped value - see section 15.1 for the full write-up.)
- New: a three-level ladder, evaluated per **6-pixel chunk** (finer than the old
  24-pixel granularity) with run-length carried as scalar state across chunks (no
  per-chunk vector reduction needed):
  1. **Flat** - 3+ consecutive uniform chunks of one slot: output is a precomputed,
     already-clamped-and-converted RGBA pattern per slot/phase (`plane_flat`),
     applied as 2 loads + 2 ORs (for alpha) + 2 stores, with no accumulation at all.
     A group whose four chunks are all flat skips the general kernel entirely.
  2. **Chunk kernel** - one uniform 6-pixel chunk not (yet) in a run: one precomputed
     19-word window (`plane_ck`) replaces six separate pixel-window adds.
  3. **Pair kernel** - an identical even/odd pixel pair: one precomputed 15-word
     window (`plane_pk`) replaces two pixel-window adds. Measured to cost more than
     it saves on the Cortex-A53 target (extra branch/lookup not repaid), so it's
     compiled in but **off by default** (`SNES_NTSC_LADDER_MASK` selects it).
  All three derived tables are *exact integer sums* of the same base kernel words
  generated in `gen_slot()` - `PK[n] = K[n] + K[n-1]`, `CK = PK0 + PK1<<2 + PK2<<4`,
  `FLAT` from `CK` - proven exact via the table's associative wrap-around addition,
  not approximated. A run-length classifier tracks state across chunks and applies a
  retroactive correction for one or two chunks that turn out not to extend into a
  flat run (the "skip now, add back later if wrong" case) - there is no equivalent
  bookkeeping in the old code, whose shortcut only ever looked backward one iteration
  (see section 15.3 for a bug this retroactive correction produced and how it was found).

Net effect: the old code had a binary flat/not-flat decision at coarse granularity;
the new code recognises runs, isolated uniform chunks, and pairs separately, at finer
granularity, backed by precomputed algebra instead of a runtime vector reduction.

### 14.5 Scaffolding: deleted, not refactored

Gone entirely: `SNES_NTSC_HAVE_SIMD`/`SNES_NTSC_X86_LEVEL`/`SNES_NTSC_HAVE_RVV`
detection and intrinsic includes in `snes_ntsc.h`, `SIMD_REGISTER_t`/
`SIMD_INPUT_REGISTER_t` and every per-ISA macro built on them
(`LOAD_/INSERT_/EXTRACT_SIMD_REGISTER_VALUE`, `ROTATE_OUT_*`,
`SET_SNES_MASK_VECTORS*`, `SNES_NTSC_CLAMP_*_{SSE4,NEON,RVV}`,
`SNES_NTSC_CLAMP_AND_CONVERT_*`, `SNES_NTSC_RGB_OUT_STORE_*`, `SNES_NTSC_RGB_STORE_*`,
`CHECK_ALL_EQUAL_*`, `SNES_NTSC_HIRES_OUT_SIMD`), plus `snes_ntsc_blit_hires_fast`
itself and the separate 749-wide scalar `snes_ntsc_blit_hires`. Replacing all of it:
one new header, `snes_ntsc_simd.h`, exposing a `v4u32` type and ~12 operations
(unaligned/aligned load, aligned/unaligned store, splat, add, sub, and/or/andnot,
shift-by-immediate, zero) with NEON, SSE2, RVV and plain-C backends, plus a generic
clamp+convert built from those primitives. `SNES_NTSC_FORCE_SCALAR_SIMD` selects the
plain-C backend on any machine for testing it. One blitter,
`snes_ntsc_blit_hires_vec`, runs on every platform - there is no longer a separate
non-SIMD fallback call in `RenderSurface`. Net: about 830 lines removed. Review-wise,
this means the four-to-five-way per-ISA duplication that made the old file hard to
audit (four `CHECK_ALL_EQUAL` variants, three `CLAMP_AND_CONVERT` variants, etc.) no
longer exists to compare against.

### 14.6 Unrelated: the phase-continuity fix (`0bdb9ff`)

Not a filter-algorithm or SIMD change - flag it separately in review. `draw_frame()`
used to advance the shared burst-phase counter only on the "top half" call and let the
concurrently-running "bottom half" render thread read it unsynchronised, with no
row-count offset applied. Fixed by moving the phase advance to the main thread before
either render thread is released (matching the pattern already used for
`flush_ntsc_palette()`), and having `blargg_filter()` compute each section's own
correct starting phase. Verified directly against `snes_ntsc_blit_hires_vec`: a split
render using the new formula is bit-identical to a continuous single-call render at
every phase; the old same-phase approach provably isn't.

### 14.7 What a reviewer should NOT expect to have changed

- The FIR structure itself: 14 taps, 6-in/7-out grid, `t0(j)`/alignment formulas,
  clamp/bias constants, S16 decode, YIQ generation, `correct_errors`,
  `merge_kernel_fields`. Byte-identical generation code throughout.
- The lores path (`snes_ntsc_blit`): untouched except for the slot-vs-colour
  indexing change in 14.2 (which applies uniformly) and one incidental correctness fix
  (its black-padding sentinel now correctly points at the reserved black slot instead
  of colour value 0 - the old value happened to still resolve correctly under the old
  table, so this was latent, not visibly broken).
- Bit-exactness: every stage gates on matching the original `_fast` output (post the
  14.4 bugfix) across synthetic, stress, and 24 real captured game frames, on
  NEON/SSE2/plain-C, including MSVC. The one deliberate, documented exception is the
  14.4 bugfix itself.

### 14.8 Quick reference

| Old (086591c) | New (`ntsc-rearch`) |
|---|---|
| `snes_ntsc_blit_hires_fast` (+ separate scalar `snes_ntsc_blit_hires`) | `snes_ntsc_blit_hires_vec` (one path, all platforms) |
| `table[0x10000][128]`, colour-indexed, 32 MiB | `plane[3][8194][slot_words]`, slot-indexed, ~15.8 MiB (with ladder tables) |
| `rgb_blargg[]` lookup per pixel per frame | removed; blitter reads the emulator's slot buffer directly |
| Full table rebuilt on any palette/setting change | incremental per-dirty-slot regen at a lock-free frame-boundary point |
| Per-ISA `SIMD_REGISTER_t`/`CHECK_ALL_EQUAL_*`/`CLAMP_AND_CONVERT_*` macro families | `snes_ntsc_simd.h`: one `v4u32` type, ~12 ops, 4 backends |
| Output lanes filled by scalar gather + `INSERT_SIMD_REGISTER_VALUE` | Output lanes filled by unaligned vector loads straight from the table |
| One repeat-shortcut: 24-pixel uniform check, broadcast previous iteration | Three-tier ladder (flat run / uniform chunk / pair) on precomputed derived tables, 6-pixel granularity |
| Bottom-half render thread: same phase as top half (race + wrong value) | Bottom-half phase computed and synchronized correctly (`0bdb9ff`) |

## 15. Notable bugs found via this harness

Three real, independent bugs were caught during this re-architecture by the bench's
checksum gate against real captured game content or a stress pattern - none of them
were caught by the synthetic `random`/`mixed` patterns alone. Kept here as technique
examples: the common thread is that real or adversarial content is what exposed each
one, and a precise diff/state-comparison method (not just "checksum mismatch") is what
localised the cause each time.

### 15.1 `ROTATE_OUT_SSE4` lane-order bug (pre-existing code, fixed at `137ad66`)

- **Symptom.** The host/SSE4 build of `snes_ntsc_blit_hires_fast` diverged from the
  Pi/NEON build's output on the 24 captured real game frames only (1671 of 23,740,416
  compared pixels differed, columns 0-735) - small, mostly 1-a-few-LSB errors confined
  to 23 scanlines out of 448 across 11 of the 24 frames, clustered in runs of 11-15
  columns. The existing synthetic `random`/`mixed` benchmarks had always agreed
  bit-for-bit between the SSE and NEON builds; this was the first input that exposed a
  real SSE4-vs-NEON divergence.
- **Root cause.** `CHECK_ALL_EQUAL_SSE4` (the repeat-block shortcut's uniformity test)
  needs the two input pixels straddling a 4-pixel load boundary gathered into one
  vector via `ROTATE_OUT(A, B, 2)`. `ROTATE_OUT_NEON` (`vext_u16(A, B, 2)`) gets this
  right (`[A[2], A[3], B[0], B[1]]`); `ROTATE_OUT_SSE4(A, B, N)` had its two byte-shifts
  swapped (`slli(A, N*2) | srli(B, 8-N*2)` instead of `srli(A, N*2) | slli(B, 8-N*2)`),
  so for `N=2` it produced `[B[2], B[3], A[0], A[1]]` - the wrong two pixels from *each*
  side of the boundary - meaning the SSE4 build's "is this 24-pixel block flat" check
  silently tested four pixels that were not even part of the current block, on every
  invocation.
- **Why synthetic patterns missed it.** Random/mixed patterns have essentially no
  chance of an accidental 4-pixel coincidence spanning a block boundary being uniform
  when the wrong-but-adjacent 4 pixels are sampled instead of the right ones. Real
  content is the opposite case: 65% flat 6-pixel chunks and 91% identical even/odd
  pairs mean the *wrong* 4-pixel sample is very often just as uniform as the *right*
  one would have been, except right at the edges of a run - exactly where the two
  samples can disagree, producing the small, sparse, edge-clustered error pattern seen.
- **How found.** `diff_output.py A.bin B.bin --cols 736` (excluding the trailing
  columns where the two blitters' edge handling legitimately differs) on
  `--dump-output` dumps: host `_fast` vs host scalar (ground truth) showed the 1671-pixel
  diff; Pi `_fast` vs Pi scalar showed zero diffs; host `_fast` vs Pi `_fast` (all 752
  columns) reproduced the *same* 1671-pixel set byte-for-byte. That three-way match
  pinned the bug on the host/SSE4 build specifically, with the Pi/NEON build already
  correct.
- **Fix.** Swap which operand gets `slli` vs `srli` in `ROTATE_OUT_SSE4` so it matches
  `vext_u16`'s semantics for any `N`, not just the `N=2` this code happens to call it
  with.

### 15.2 Stage 3 double-clamp/convert cost bug

- **Symptom.** The first implementation of the run-length ladder (Part C) was *slower*
  on real captured frames (12.10 ms) than the Stage 2 baseline it was meant to improve
  on (9.65 ms) and than the retired `_fast` (8.55 ms) - despite being fully bit-exact
  and despite per-path statistics confirming the ladder was correctly routing 77% of
  chunks onto the free/cheap skip-or-uniform paths.
- **Root cause.** `snes_ntsc_simd_clamp_convert` (a clamp plus a ~20-op
  ABGR-build-then-`bswap32` convert) is roughly half of the general kernel's
  per-output cost. The first-attempt ladder made every flat/steady chunk pay it
  *twice*: once when the group's normal path clamped/converted/stored all 7
  accumulators, and again when the FLAT overwrite pass clamped/converted 2 more
  vectors on top of that - so a steady-run chunk, the ladder's whole reason to exist,
  cost at least as much as the general path it was meant to avoid.
- **How found.** Not a checksum failure - a cost bug, found on review of the
  disappointing Pi timings by inspecting where cycles were actually spent in the emit
  path, given that the per-path classification statistics were already confirmed
  correct.
- **Fix (three changes, see `README.md`'s "Stage 3 review" section for the full
  detail).** (1) Store `FLAT` pre-clamped and pre-converted, with `Alevel` folded in
  afterward as a plain OR against a precomputed splat, so a flat chunk's overwrite is
  2 loads + 2 ORs + 2 stores with no clamp or convert at all. (2) A group-level fast
  path: when all 4 chunks of a 24-pixel group are covered by `FLAT`, skip accumulation
  and clamp/convert/store entirely - just 4 FLAT emits. (3) Replace the ~20-op
  ABGR-build-then-`bswap32` convert everywhere with a direct 3-shift/3-and/3-or
  construction of the final little-endian RGBA word (verified bit-for-bit equivalent
  by tracing each output byte through the old route). Closed the gap: 12.10 -> 9.26 ms
  on `--input` (6.77 ms filter-only), ahead of the Stage 2 baseline and at the
  architecture's "at or under 6 ms" filter-only gate (section 11).

### 15.3 Stage 4 retro-add double-counting bug

- **Symptom.** `--pattern runs` failed its checksum
  (`0x3df0769d80b04648` instead of the reference `0x0f9860292b10dd73`) under any
  `SNES_NTSC_LADDER_MASK` with bit0 (flat/skip) set. `--pattern random` never
  triggered multi-chunk runs and passed throughout, giving no warning.
- **Root cause.** Stage 4's fused, lookahead-free classifier (replacing Stage 3's
  row-wide pre-pass, see the "Lookahead-free classifier" bullet in section 13) carries
  "was chunk C-1/C-2 skipped" state forward across chunks and, when a run ends,
  retroactively adds the skipped chunks' contribution back in. The first
  implementation shifted that carried state forward every chunk regardless of whether
  a retro-add round had already consumed it, so a chunk already retro-added at one
  run's end could be retro-added *again* at a later run's end (reached via "C-2" from
  a chunk further along) - silently double-summing its contribution.
- **How found.** By comparing raw accumulator state (`A[0..2]`, via a temporary debug
  trace) at the first group boundary after a skip, between a known-correct mask=0 run
  and the failing mask=1 run: the two matched exactly through the retro-add itself but
  diverged immediately after the *next* chunk's retro check re-fired on
  already-resolved state.
- **Fix.** Reset (not shift) the carried skip state whenever a retro-add round
  consumes it this chunk.

## 16. Memory-system experiments (general Cortex-A53 findings)

Two measurements from this project's benchmarking are general findings about the
Cortex-A53 memory system, useful beyond this one filter, not just evidence for this
filter's specific design choices.

### 16.1 Working-set sweep

test mule, 640x448, 1 thread, performance governor, core 3, 2 s runs. ms per frame
versus number of distinct colours N (table working set = N x 512 B):

| N | 1 | 16 | 64 | 256 | 1024 | 4096 | 65536 |
|---|---|---|---|---|---|---|---|
| scalar `snes_ntsc_blit_hires` (086591c) | 10.98 | 13.01 | 19.03 | 22.20 | 47.79 | 83.95 | 93.53 |
| `snes_ntsc_blit_hires_fast` (086591c) | 1.95 (repeat-block path) | 11.89 | 15.41 | 19.60 | 48.23 | 82.97 | 95.08 |

Reading: N=1 is the pure compute floor (about 33 cycles per output for scalar at
1 GHz). The step 16 -> 64 is the table outgrowing L1 (32 KB); 256 -> 1024 is it
outgrowing L2 (512 KB). Page size on the target is 4 KB. Transparent huge pages are
NOT available on its kernel (`6.18.39+rpt-rpi-v8` has no
`/sys/kernel/mm/transparent_hugepage` and no `HugePages` in `/proc/meminfo`);
`madvise(MADV_HUGEPAGE)` fails there. Produced with `tools/ntsc_bench/sweep_pi.sh`
(runs on the Pi against a prebuilt binary).

### 16.2 Page-spread vs cache-line experiment

test mule, scalar `snes_ntsc_blit_hires` @ 086591c, 3 s runs, core 3, performance
governor. `--palette contiguous` keeps exactly the same N table rows touched, with
exactly the same RNG-driven pixel placement, as `--palette random` - only *where in
the 32 MiB table* those N rows live changes: `random` scatters them pseudo-randomly
across all 65536 rows (up to N distinct 4 KB pages), `contiguous` packs them into the
first N rows (N/8 4 KB pages). So any timing difference between the two isolates
page/TLB locality from cache-line traffic, which is identical in both modes (same
N x 512 B fetched per frame).

This kernel (`6.18.39+rpt-rpi-v8`) has no `/sys/kernel/mm/transparent_hugepage` at all
- `CONFIG_TRANSPARENT_HUGEPAGE` is not built in, not just disabled.
`madvise(MADV_HUGEPAGE)` therefore failed (`EINVAL`) on every `--hugepages` run below,
and `/proc/self/smaps_rollup` read back `AnonHugePages: 0 kB` every time - THP was
never actually granted on this box. The "on" column differs from "off" only in that
the table was allocated with `posix_memalign(2 MiB)` instead of `malloc()`.

ms per frame:

| N | random, THP off | random, THP on | contiguous, THP off | contiguous, THP on |
|---|---|---|---|---|
| 64   | 18.59 | 18.56 | 13.27 | 13.11 |
| 256  | 22.10 | 22.01 | 20.29 | 19.90 |
| 1024 | 48.46 | 45.02 | 37.86 | 34.21 |

Repeat of N=256/random/THP off, run last: 22.26 ms (vs 22.10 ms first; +0.7%, well
inside the ~4% min/max jitter seen within a single run) - the sweep is run-to-run
stable. `vcgencmd get_throttled` read `0x0` before and after the whole sequence (no
thermal throttling).

Interpretation: since `hugepages` never actually took effect here (THP unsupported by
the kernel), those two columns are noise around the same allocation strategy and
should be read as such, not as a THP effect. The real signal is `random` vs
`contiguous`: at identical cache-line traffic, packing the same N touched rows into
N/8 pages instead of scattering them over up to N pages is 9-29% faster (largest
relative gain at N=64, where the touched set - one page's worth of TLB reach at 8
rows/page - fits the Cortex-A53's small L1 dTLB reach when contiguous but not when
scattered across 64 separate pages). That the *same bytes moved* run measurably faster
purely from being packed into fewer pages says a real share of the scalar filter's
stall time at these working sizes is page/TLB-spread overhead (TLB misses, page-table
walks) rather than pure cache-line-fetch traffic - cache-line volume alone cannot
explain the gap, since both modes fetch identical volume.

---

## 17. Reproducibility of the reference checksums

The bench's `checksum` is bit-exact across machines and SIMD ISAs **for captured frame
input**, which is what matters: it is the gate that proves the blitter's integer work is
portable. It is *not* bit-exact for the synthetic patterns across builds, and the reason
is worth recording because it looks alarming and is not a bug in the filter.

### 17.1 Floating-point contraction changes the synthetic-pattern checksums

Reproduced 2026-09-08 on x86-64 (GCC 13.3, current working tree, `snes_ntsc_blit_hires_vec`):

| Build | random/256 | mixed/256 | runs/256 | captured frames |
|---|---|---|---|---|
| `-march=native` (reference) | `0x95f5c92e642a686f` | `0x2921e1e8ab232056` | `0x0f9860292b10dd73` | `0xe3557fa06310379d` |
| `-march=x86-64` | `0xc928ffe2395b1fa7` | `0x0616580b4a8a3d6e` | `0xfead91f93878fb60` | `0xe3557fa06310379d` |
| `-march=native -ffp-contract=off` | `0xc928ffe2395b1fa7` | — | — | `0xe3557fa06310379d` |

Two things follow. First, **the captured-frame checksum is unaffected by any of it** — the
blitter itself is bit-portable, which is the property the gate exists to prove. Second, the
divergence is not SSE2-vs-SSE4 (every run above reports `simd=SSE2`): turning FP contraction
off on the *native* build reproduces the baseline value exactly. So the cause is
**FMA contraction in the floating-point table generation** (`snes_ntsc_init` /
`snes_ntsc_impl.h`), which `-march=native` enables and a baseline target does not. Slightly
different table entries survive quantisation on high-entropy synthetic content and are
absorbed on real game content, which is flat and low-entropy by comparison.

Consequence for anyone re-running the gate: **the synthetic-pattern reference checksums in
`README.md` are specific to an FMA-capable build.** Compare captured-frame checksums when
comparing across platforms or `-march` levels; treat a synthetic-pattern mismatch as a
question about the build's FP contraction before treating it as a filter defect.

### 17.2 Ladder routing by content type

Per-path routing measured with the same builds, showing how differently real and synthetic
content exercise the run-length ladder. This is the evidence behind the claim that real
frames are dominated by the cheap paths:

| Input | chunks | skip (steady) | uniform (CK) | pair (PK) | general (K) |
|---|---:|---:|---:|---:|---:|
| 24 captured frames | 211,434,496 | 59.2% | 17.8% | 69.5% | 30.5% |
| `--pattern runs`, 256 colours | 167,824,384 | 5.5% | 22.3% | 40.4% | 59.6% |

`skip` and `uniform` are percentages of all chunks; `pair`/`general` are the split of the
remaining pair decisions (172,429,932 and 517,834,326 respectively). Real content takes the
steady-state skip on nearly 60% of chunks and reaches the general per-pixel kernel on under
a third of its pair decisions; the `runs` pattern was designed to invert that, and does.
