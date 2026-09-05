# Reviewer's guide: NTSC filter re-architecture (branch `ntsc-rearch`)

Audience: reviewers with intricate working knowledge of the filter as it stands at the
public GitHub head (commit `086591c`): `snes_ntsc_blit_hires_fast`, the per-ISA
`SNES_NTSC_HAVE_SIMD` dispatch, `CHECK_ALL_EQUAL_SSE4/NEON/AARCH64/RVV`,
`INSERT_/EXTRACT_SIMD_REGISTER_VALUE`, `SNES_NTSC_CLAMP_AND_CONVERT_*`, and the
65536-entry colour-indexed `snes_ntsc_t::table`. This guide assumes that context and
does not re-derive it. It covers commits `137ad66`..`0bdb9ff` on `ntsc-rearch`.

Commit sequence: `137ad66` (SSE4 bugfix, pre-existing code) → `7ec34e5` (table) →
`810a4be` (vector kernel) → `a6dfa98` (run-length ladder) → `583b689` (retire old
paths) → `6a46028` (review fixes) → `0bdb9ff` (unrelated threading fix, see §6).

## 1. The one-line answer

The old code's SIMD idea — *pack four output accumulators, but fill each lane by
scalar table-gather-then-insert* — is **replaced**, not extended. The new kernel
instead *loads a contiguous 128-bit window straight out of the table into the
accumulator*, which only works because the table layout changed first (§2) to make
that window exist and be safe to read. The old code's *chunking* (24 input pixels /
28 outputs per iteration, 7 output accumulators) and its *repeat-run shortcut concept*
are both preserved, but the run-length mechanism is generalised into a three-tier
ladder driven by precomputed derived tables rather than one runtime vector-compare
(§4). The per-ISA macro scaffolding itself is deleted outright and replaced by one
~12-op portable vector layer (§5).

## 2. Table: colour-indexed → slot-indexed (not a SIMD change at all)

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
  either render thread is released — this also closes a real pre-existing unlocked
  race, where the old code wrote two `rgb_blargg[]` words directly into a table a
  render thread might be concurrently reading.
- Consequence for reviewers: `gen_kernel`/`correct_errors`/`merge_kernel_fields` are
  untouched byte-for-byte; only what surrounds one generated entry (how it's
  addressed, and that it can be regenerated one slot at a time) changed.
- Each slot's block gained padding (16 words, later 20) around the 14 real kernel
  words. The old table had no such padding — it didn't need it, because the old
  kernel never issued a vector *load* against table memory (see §3). The padding
  exists solely so the new kernel's unaligned windows never read outside a slot's
  own memory or need masking.

## 3. The kernel: `snes_ntsc_blit_hires_fast` → `snes_ntsc_blit_hires_vec`

### What the old kernel actually vectorised

Worth stating precisely, because it's the crux of "preserved vs. replaced": the old
kernel already grouped work into **4 sub-blocks of 6 input pixels → 28 outputs**, held
in **7 accumulator registers of 4 lanes each** (`xmm10..xmm16`) — the same chunk
granularity the new kernel uses. But within that structure, every output value was
computed as **12 scalar table reads and adds** (`SNES_NTSC_COLOR_IN` doing pointer
arithmetic per pixel, `SNES_NTSC_HIRES_OUT_SIMD_` summing scalars), and the resulting
scalar sum was then placed into its accumulator lane with
`INSERT_SIMD_REGISTER_VALUE`. The *only* place the vector unit did real arithmetic on
more than one output at a time was the final clamp/convert/store. In other words: SIMD
was used as a **packing container for scalar results**, not as compute. This is also
why `_fast2` existed as an abandoned, uncommitted A7-tuned variant before this project
started — its stated goal was to stop *extracting input lanes back to GPRs* for the
gather (which stalls an in-order core), while still keeping the same scalar-sum,
insert-into-lane structure for the outputs. It's a half-measure along the same axis,
not the axis change described next, and none of its code survives on this branch (it
also had a bit-exactness bug in its repeat-block reuse, unrelated to the phase fix in
§6).

### What the new kernel does instead

`snes_ntsc_blit_hires_vec` exploits a fact about the table layout that the old code
never used (it's proven in `ALGORITHM.md` §6.2): **adjacent output positions read
adjacent words within a kernel block.** So instead of computing a scalar sum and
inserting it, each pixel contributes 4–5 **unaligned 128-bit loads** taken directly
from its table block, added straight into the 7 accumulators with vector adds. There
is no per-tap scalar gather-and-insert; the "gather" is reduced to one 16-bit load per
pixel (to find its slot) plus integer address arithmetic, and the FIR summation itself
is genuine vector work. Loads per output fall from 12 scalar reads to roughly 3.6
vector loads. The 24-pixel/28-output/7-accumulator chunk shape carries over unchanged
from the old kernel, including how a group's tail 3 accumulators are carried into the
next group's leading 3 — that bookkeeping pattern is inherited, not reinvented.

### Portability consequence

Because the new kernel's only primitives are unaligned load, vector add, and a handful
of bitwise/shift ops for clamp+convert, it needs no per-ISA gather trick and no
SSE4.1-specific instruction (`_mm_extract_epi16`/`insert` are gone). One kernel body,
one `snes_ntsc_simd.h` layer (§5), covers NEON/SSE2/RVV/plain-C.

## 4. The repeat-run shortcut: generalised, not just ported

- Old: `CHECK_ALL_EQUAL_{SSE4,NEON,AARCH64,RVV}` vector-compares a fixed 24-pixel
  window each iteration; if uniform, it broadcasts the *previous* iteration's already-
  clamped 7-vector output instead of recomputing. One shortcut, one granularity
  (whole 4-chunk group), detected fresh every iteration by a vector reduction. (The
  SSE4 feeder for this check — `ROTATE_OUT_SSE4` — had a real, independent bug found
  and fixed at `137ad66`: its two byte-shifts were swapped, so on x86 it sampled four
  pixels from the wrong side of the load boundary. NEON was correct. Synthetic test
  patterns never triggered it; real game content with long flat runs did. This is a
  fix to pre-existing code, unrelated to the redesign, and is the one place bit-
  exactness against the *original* `_fast` output required accepting a corrected
  reference rather than the shipped value.)
- New: a three-level ladder, evaluated per **6-pixel chunk** (finer than the old
  24-pixel granularity) with run-length carried as scalar state across chunks (no
  per-chunk vector reduction needed):
  1. **Flat** — 3+ consecutive uniform chunks of one slot: output is a precomputed,
     already-clamped-and-converted RGBA pattern per slot/phase (`plane_flat`),
     applied as 2 loads + 2 ORs (for alpha) + 2 stores, with no accumulation at all.
     A group whose four chunks are all flat skips the general kernel entirely.
  2. **Chunk kernel** — one uniform 6-pixel chunk not (yet) in a run: one precomputed
     19-word window (`plane_ck`) replaces six separate pixel-window adds.
  3. **Pair kernel** — an identical even/odd pixel pair: one precomputed 15-word
     window (`plane_pk`) replaces two pixel-window adds. Measured to cost more than
     it saves on the Cortex-A53 target (extra branch/lookup not repaid), so it's
     compiled in but **off by default** (`SNES_NTSC_LADDER_MASK` selects it).
  All three derived tables are *exact integer sums* of the same base kernel words
  generated in `gen_slot()` — `PK[n] = K[n] + K[n-1]`, `CK = PK0 + PK1<<2 + PK2<<4`,
  `FLAT` from `CK` — proven exact via the table's associative wrap-around addition,
  not approximated. A run-length classifier tracks state across chunks and applies a
  retroactive correction for one or two chunks that turn out not to extend into a
  flat run (the "skip now, add back later if wrong" case) — there is no equivalent
  bookkeeping in the old code, whose shortcut only ever looked backward one iteration.

Net effect: the old code had a binary flat/not-flat decision at coarse granularity;
the new code recognises runs, isolated uniform chunks, and pairs separately, at finer
granularity, backed by precomputed algebra instead of a runtime vector reduction.

## 5. Scaffolding: deleted, not refactored

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
`snes_ntsc_blit_hires_vec`, runs on every platform — there is no longer a separate
non-SIMD fallback call in `RenderSurface`. Net: about 830 lines removed. Review-wise,
this means the four-to-five-way per-ISA duplication that made the old file hard to
audit (four `CHECK_ALL_EQUAL` variants, three `CLAMP_AND_CONVERT` variants, etc.) no
longer exists to compare against.

## 6. Unrelated: the phase-continuity fix (`0bdb9ff`)

Not a filter-algorithm or SIMD change — flag it separately in review. `draw_frame()`
used to advance the shared burst-phase counter only on the "top half" call and let the
concurrently-running "bottom half" render thread read it unsynchronised, with no
row-count offset applied. Fixed by moving the phase advance to the main thread before
either render thread is released (matching the pattern already used for
`flush_ntsc_palette()`), and having `blargg_filter()` compute each section's own
correct starting phase. Verified directly against `snes_ntsc_blit_hires_vec`: a split
render using the new formula is bit-identical to a continuous single-call render at
every phase; the old same-phase approach provably isn't.

## 7. What a reviewer should NOT expect to have changed

- The FIR structure itself: 14 taps, 6-in/7-out grid, `t0(j)`/alignment formulas,
  clamp/bias constants, S16 decode, YIQ generation, `correct_errors`,
  `merge_kernel_fields`. Byte-identical generation code throughout.
- The lores path (`snes_ntsc_blit`): untouched except for the slot-vs-colour
  indexing change in §2 (which applies uniformly) and one incidental correctness fix
  (its black-padding sentinel now correctly points at the reserved black slot instead
  of colour value 0 — the old value happened to still resolve correctly under the old
  table, so this was latent, not visibly broken).
- Bit-exactness: every stage gates on matching the original `_fast` output (post the
  §4 bugfix) across synthetic, stress, and 24 real captured game frames, on
  NEON/SSE2/plain-C, including MSVC. The one deliberate, documented exception is the
  §4 bugfix itself.

## 8. Quick reference

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
