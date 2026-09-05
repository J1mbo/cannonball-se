# Sprite renderer re-plan: from the measured S4 regression to an accelerated A7/A53 path

Status: 2026-09-05, branch `sprite-rework`, HEAD `9c26c6d` (S0-S4 committed). Design deliverable
only; no renderer code changed. Inputs: `MEASURED_S4_INSITU.md`, `MEASURED_S1_INSITU.md`,
`MEASURED_INSITU_SPRITE.md`, `MEASURED_A53_PI3.md`, `DESIGN_REVIEW_BANDWIDTH.md`,
`DESIGN_REVIEW_STAGING.md`, `DESIGN_FABLE.md`, `FABLE_ASSESSMENT.md`, `MICROARCH_A7_A53.md`,
`SPRITE_IMPLEMENTATION_SPEC.md`, `PROBLEM_STATEMENT.md`; the S4 renderer
(`src/main/hwvideo/hwsprites.cpp/.hpp` at HEAD), the shipped renderer (`bdf7e9b`),
`hwarch.hpp`, `hwtiles.cpp`'s NEON block, the top-level and driver `CMakeLists.txt`, and the
test-mule notes. Figures are tagged **[M]** measured (with source) or **[E]** estimated here.

Targets, as now stated: accelerate on Cortex-A53 (AArch64 NEON: Pi 2 v1.2, Pi 3, Zero 2 W)
**and** on Cortex-A7 (ARMv7, 32-bit NEON: Pi 2 v1.1); a corrected scalar fallback for ARMv6
(Pi 1 / Zero v1) that does not regress against the shipped renderer.

---

## 0. Verdict

**The load-shape reading is right; the attribution is wrong.** A regression that is worst at
light load and narrows without closing is a fixed per-row cost, and the S4 code has one. But it
is not the on-the-fly flip/shadow classification that replaced `sprites_flipped` /
`sprites_shadowinfo`. Counted from the code, `nibrev()` plus `classify_mixed()` plus the two
word-class compares are at most ~21 ALU ops per source *word* (eight source pixels), which is
0.02-0.05 ms per light frame and ~0.05 ms per heavy frame **[E, section 1.2]** -- a tenth to a
fifth of the light-band deficit, under a tenth of the heavy one -- and the thing they replaced
was a dependent byte load per row from a table that is cold in L2 every frame (M-B measured
1,073-2,109 load-side L2 refills per frame in the shipped kernel **[M]**). Restoring the tables
would cost more than it saves and would bring back F1's history dependence.

The cost that fits both bands is the **glue around the LINEBUF kernel**: per source row, two
function calls, two `sched()` lookups, `int64` per-word bounds arithmetic, a `std::min`/
`config` reload, the S3 shadow-prefetch scan run for every sprite, and a `(d*zoom)>>9` multiply
per destination row; per span-row, a libc `memcpy` call (S1; still the `n < 8` path in S4), a
D/D' branch, the glow bookkeeping, and NEON tail handling; plus alias-driven reloads of
`this`-members and `r.*` fields after every 8-byte `memcpy` store inside `decode_row`. A uniform
~150 cycles of excess per source row reproduces the measured +174k / +357k / +625k cycles per
frame (light / moderate / heavy, pi2-local at 900 MHz) if frames have roughly 1.2k / 2.4k / 4.2k
source rows -- i.e. rows of ~45 output pixels at light and ~75 at heavy **[E; one counter run
settles it, R0]**. The bench LINEBUF kernel that measured 5.8 cyc/px (`MEASURED_A53_PI3.md`
**[M]**) had none of that glue; the in-situ S4 is ~12 cyc/px on the same structure.

**Recommendation: route (c), a hybrid.**

| Target | Kernel | Stage |
|---|---|---|
| A53 (AArch64) and A7 (ARMv7 + NEON) | LINEBUF, glue rebuilt, per-sprite specialised, NEON span copy/OR made ISA-portable | R1, R2 |
| ARMv6 (no NEON) | the shipped kernel (`render_ref` twin) with F1, F2, the 3-row double-draw fix and inline glow; side tables kept | R3 |
| x86-64 / MSVC / other | LINEBUF scalar (`CB_FORCE_SCALAR` twin), correctness only | -- |
| light-band residue, if R1 leaves it | direct-emit rows (`r_v = 1`, zoom >= 0x100) inside LINEBUF | R4, conditional |

Do **not** restore the side tables; do **not** pursue S4c (NEON `tbl` decode: AArch64-only and
not the bottleneck); do **not** abandon LINEBUF (route b) for the accelerated path -- a
per-output-pixel DDA has no SIMD formulation that does not read the destination, and the bench
already showed LINEBUF 2x ahead of it. Expected after R1 (A53, pi2-local basis): light
0.77-0.92x, moderate 0.71-0.85x, heavy 0.65-0.80x of the shipped frame time; gate at <= 0.90x
in every band. Sections 1-9 justify each part.

---

## 1. Re-reading the measurement

### 1.1 What is on the table

pi2-local (Pi 2 v1.2, A53, 900 MHz, throttled 0x0) **[M, MEASURED_S4_INSITU]**, ns per frame:

| band | shipped | S1 | S4-D | S4-D' | S4-D excess (cycles @0.9 GHz) |
|---|---|---|---|---|---|
| light | 777,616 | 1,077k (1.385x) | 970k (1.248x) | 965k (1.241x) | +174k |
| moderate | 1,408,879 | 1,934k (1.373x) | 1,806k (1.282x) | 1,764k (1.252x) | +357k |
| heavy | 3,374,385 | 4,360k (1.292x) | 4,069k (1.206x) | 3,972k (1.177x) | +625k |

S1 -> S4-D saved 107 / 128 / 290 us per frame (light / moderate / heavy). That is the cost the
libc `memcpy` per span-row and the S4b word classes removed; NEON as such is a small part of it
(section 1.3).

sl-d303 (Zero 2 W, 1 GHz) cycles per *opaque* pixel **[M, MEASURED_S1_INSITU]**: typical
shipped 21.26 vs S1 28.24 (+7.0); extreme 13.71 vs 15.24 (+1.5). Same shape.

The shipped kernel's own in-situ profile **[M, MEASURED_INSITU_SPRITE]**: exposed destination
memory term 0.10 ms typical / 0.37 ms extreme; L2 refills 60-80% destination-side; load-side
1,073-2,109 lines per frame (ROM rows plus `sprites_shadowinfo`), roughly flat across load.

### 1.2 Why the side-table recompute cannot carry the regression

Per source word in `decode_row` (hwsprites.cpp, `decode_row()`):

| work | ops | when |
|---|---|---|
| `nibrev(w)` = 2 shifts, 2 ands, 1 orr, 1 `rev` | 6 | flipped sprites only |
| `w == 0`, `r.shadow && w == 0xAAAAAAAA` | ~4 | every word |
| `classify_mixed(w, smask)` (three SWAR any-zero-nibble tests, mask, or, test) | ~11 | every word not GAP/all-shadow |

Worst case ~21 ALU ops per 8 source pixels; A53 dual-issues most of these, A7 fewer. Words
decoded per frame = visited output pixels / (8 * rho_h * rho_v): with ~60k visited px at light
and a pixel-weighted rho^2 of 2-4, that is 2-4k words, so 40-80k ops = 20-50k cycles =
**0.02-0.05 ms** per light frame; at heavy (~300k px, rho^2 ~ 9) ~4k words = **~0.05 ms**. Against
excesses of 0.19 ms (light) and 0.69 ms (heavy) that is 10-25% and ~7%. Real, secondary, and --
because the alternative is a byte load per row from a 256 KB table that `DESIGN_REVIEW_BANDWIDTH`
section 1 shows cannot survive the NTSC threads' ~1 MB/ms L2 traffic between frames -- cheaper
than what it replaced. The shipped kernel's pre-pass walks `sprite_height` rows of that table per
sprite per frame (`hwsprites.cpp` at `bdf7e9b`, the two loops before the row loop) on top of the
per-row-group `spriterom_shadowinfo[addr]` load, so the tables were never "read cheaply" in-situ;
M-B's load-side line count is partly them.

Conclusion: the recompute stays. If R0 shows the flipped-sprite share is large and the light band
still needs help after R1, the cheap mitigation is a per-sprite `FLIP` template (section 3.1), not
a table.

### 1.3 What does fit: fixed per-source-row and per-span-row glue

Reading `draw_sprite_fast` -> `decode_row` -> `replay` -> `replay_colour_span` at HEAD, per
**source row**:

1. `prefetch_rom_row()` call: two `int64` bounds checks and two prefetches.
2. `decode_row()` call: `sched()` and `wsum()` (two `unique_ptr` lookups), `int64 rowbase`, then
   per word an `int64` `widx` select-and-two-compares, and per nibble an 8-byte `memcpy` store into
   `line[]` followed by a `run[words*8+k]` byte load. `memcpy` stores have alias set 0, so
   `r.color`, `r.shadow`, `r.flip`, `nrec`, `rec`, the `run` pointer are all legally reloadable
   after each one; `nrec` is a member, so `emit()` round-trips it through memory.
3. `replay()` call: `config.s16_width` and `config.s16_height` loads, `std::min` for `d_hi`/
   `pf_hi`, then the S3 shadow-prefetch scan over every record **for every sprite, shadow flag or
   not**, then the record loop.
4. In `draw_sprite_fast`, one `int64` multiply per destination row to find the group end.

Per **span-row**: the `replay_mode` branch; for `n >= 8` the NEON loop with its `min()`-rebased
tail (one mispredict per span-row on the exit); for `n < 8` a libc `memcpy` call (~20-30 cycles
including the size-class branches); the glow test, `shadow_rows` bit test and `glow_push`.

Budget **[E]**: 100-150 cycles per source row and 25-40 per span-row over what the shipped
kernel spends on its row group (~40-60 instructions plus one indirect-branch mispredict,
`FABLE_ASSESSMENT` 2.2). At ~45 output px per source row (light) that is 3-4 cyc/px; at ~75
(heavy) about 2. That is the whole measured excess, in both bands, and it explains why the
shipped kernel's *lower* fixed cost wins even though its per-pixel cost is 2x LINEBUF's. It also
explains why the bench never saw it: `sprite_store_bench.c` drew 26 sprites of ~12k px each --
rows of hundreds of pixels replayed 3-4 times.

Two other legitimate contributions to keep in mind when reading the R0 numbers: S1-S4 draw
**glow** (the shipped build never compiled it; S2 estimated 0.01-0.03 ms but the flush is a
chain of dependent loads on an in-order core, 17 cycles each when they hit L2 -- ~0.1 ms at heavy
is possible **[E]**), and the F1 fix draws up to one extra source row per affected flipped sprite
(a few percent more pixels, `MEASURED_S1_INSITU` "pixel count check").

### 1.4 Ranked suspects, with the R0 test that confirms each

| # | suspect | where | expected size **[E]** | R0 test |
|---|---|---|---|---|
| 1 | per-source-row glue (calls, `sched()` x2, `int64` bounds, `config` reloads, S3 scan on non-shadow sprites, per-dest-row multiply) | `draw_sprite_fast`, `decode_row` head, `replay` head | 100-150 cyc/row = 0.1-0.6 ms/frame | R0c row counter: excess/frame tracks rows/frame, not px/frame |
| 2 | alias reloads and member round-trips in the nibble loop | `decode_row`, `emit` | 1-3 extra LS slots per source px | R0a objdump: `ldr` from `[this]`/`[r]` inside the unrolled loop; R0e instructions/px |
| 3 | `memcpy` call for `n < 8` spans; NEON tail mispredict; D/D' branch | `replay_colour_span` | 20-40 cyc/span-row | R0c span-row histogram by `n` class; R0a `bl memcpy` |
| 4 | eight `run[]` byte loads per word | `decode_row` | 7 LS slots per word | R0a |
| 5 | glow flush dependent-load chain | `glow_flush` | 0.02-0.1 ms/frame | R0b `-noglow` A/B |
| 6 | `sched_tab` first-touch misses (one per distinct zoom per frame) | `sched()` | ~0.03 ms/frame | R0e L2 refills vs shipped |
| 7 | F1 fix extra rows; classify/nibrev recompute | -- | 2-5%; 3-7% light | R0c px/frame vs shipped; arithmetic |

If R0 contradicts this ranking (in particular, if the excess tracks pixels rather than rows at
heavy load), the R1 item list below is re-ordered, not abandoned: every item is a strict work
reduction with no output change.

---

## 2. Route assessment

**(a) as phrased -- keep LINEBUF, restore a cheap cached flip/shadow representation.** Rejected.
Section 1.2: the recompute is 0.02-0.05 ms per frame; a cache is a cold load per row (M-B) and
reintroduces the mechanism behind F1. A one-time source transform (pre-mirrored ROM) doubles the
ROM working set on the contested L2 for zero compute saving (`DESIGN_REVIEW_BANDWIDTH` section 5
already made this call and the in-situ data reinforced it).

**(a') keep LINEBUF, eliminate the per-row and per-span glue, specialise per sprite.** Adopted
as the accelerated path. The structure is proven (bit-exact against the oracle, F1/F2 fixed,
glow correct) and its per-pixel advantage is measured (bench 2.0x compute, 1.76x under a
thrasher **[M]**). What failed was the wrapping, and every fix is local.

**(b) abandon LINEBUF; optimise the shipped renderer; port F1/F2 + glow + SIMD.** Rejected for
the accelerated path, adopted for the ARMv6 fallback. The shipped kernel's per-output-pixel DDA
has a loop-carried `xacc` chain and two data-dependent branches per output pixel; the only NEON
formulation that fits it is `FABLE_ASSESSMENT` 4.8's masked blend, which loads the destination
per 16 pixels -- a load stall on every cold line, the exact thing the store-only LINEBUF avoids
and the bench penalised. "Shipped + fixes" tops out at perhaps 10-20% from the double-draw fix and
dropping the pre-pass misses. That is a fine fallback and the wrong ceiling for A53/A7.

**(c) hybrid.** Chosen; the table in section 0 says what runs where. The extra maintenance is
one already-existing kernel (`render_ref`, compiled today under `CB_RENDER_TEST` and already
proven equal to `render_px(glow=false)` by `selftest` phase B) promoted to a game path for one
ISA, plus one conditional stage.

---

## 3. The accelerated kernel: mechanism (R1)

All changes are inside `draw_sprite_fast` and what it calls; output is bit-identical to
`render_px` (the harness gates it). Nothing below changes the record semantics, the glow rule,
the shadow bitmap or the fallback envelope.

### 3.1 Per-sprite context and specialisation

Build once per sprite a plain struct (`SpriteCtx`): `line`, `rec` (pointers to the scratch),
`run`, `wsum` (from one `sched()` call), `color`, `smask`, `pitch`, `pend`, `cx0`, `cx1`,
`base = pixels + top*stride + xpos`, `stride`, `d_hi`, `shadow`, `rtl`, `glow`, the ROM row-0
pointer and the per-sprite ROM bounds (below). Pass it by reference to `static inline` free
functions (or private statics) for decode/replay/emit; the hot scalars (`pos`, `words`, `prev`,
`nrec`) are locals whose address is never taken, so the alias set of the `memcpy` stores no
longer forces reloads. `decode_row` and the vertical loop are templated on `<bool FLIP, int
MAXRUN>` with `MAXRUN in {2, 4}` chosen from zoom (`>= 0x100` -> 2, else 4; `>= 0x200`, the
run-0 regime, keeps the generic path and is not hi-res-default content). Four instantiations,
~1-1.5 KB each **[E]**, selected by one `switch` per sprite. `shadow` stays a runtime `smask`
(already branch-free in `classify_mixed`).

Why this is the fix for the measured cause: it removes the two calls, both `sched()` lookups,
every `config` load and every `r.*`/member reload from the per-row and per-nibble paths, and lets
the compiler keep ~10 live values in registers -- which on the A7 (13 usable GPRs) is the
difference between a loop that spills and one that does not.

### 3.2 Decode loop

* **ROM bounds once per sprite, not per word.** The envelope already routes odd records to
  `draw_sprite_px`; extend it with "the sprite's ROM footprint `[bank<<16 + addr + pitch*j_first
  (- pitch + 1 if FLIP), ... + pitch*j_last + SPR_WMAX)` lies inside `[0, SPRITES_LENGTH)`". Real
  ROM data always passes (`stat_fallback` must stay 0 in the stats build); the per-word `int64`
  select-and-compare disappears and the loop condition becomes `pos < pend && words < SPR_WMAX`
  on 32-bit pointers. On A32 this also removes the register pairs `int64` costs.
* **Run bytes as one load.** Per word, one 8-byte `memcpy` load of `run[words*8 .. +7]` into a
  `uint64_t`, then `(runs >> 8k) & 0xff` per nibble (A64: one `ldr x`; A32: `ldrd` -- the table
  base is 8-aligned and `words*8` keeps it so -- or two `ldr`). Replaces eight byte loads.
* **Fill width by class.** `MAXRUN == 2`: `uint32_t v2 = v | v << 16`, one 4-byte `memcpy`
  store. `MAXRUN == 4`: the existing 8-byte store (A64 `str x`; A32 two `str` or `vst1_u16` -- never
  a `strd`, which faults on 2-byte alignment). The safety argument is unchanged: overrun lands on
  positions the next source pixel overwrites or beyond the sentinel.
* **Class tests unchanged** (`w == 0`, all-shadow, `classify_mixed`), but `FLIP` is a template
  constant so `nibrev` and the pointer step are compiled in, not branched on.
* **`emit` on a local record pointer and a local count**, inlined; the sentinel rule is
  unchanged.

### 3.3 Vertical loop

`SCHEDULE_VERIFICATION` C4: the same `run[]` serves both axes, so destination rows of source row
`j` are `[N(j), N(j) + run[j])`. Replace the per-destination-row `((int64_t)d*zoom) >> 9` search
with: `j0 = (d_lo*zoom) >> 9` (one multiply), `da = d_lo`, `db = min(N(j0+1), d_hi)` where
`N(j0+1) = ceil(512*(j0+1)/zoom)` (one division per sprite), then per source row `da = db; db =
min(da + run[j], d_hi)`. One multiply and one division per sprite instead of one multiply per
destination row; identical `da/db` by C1-C4.

### 3.4 Replay

* **Copy ladder, no libc call:** `n >= 8` the existing NEON chunk loop with the re-based tail;
  `4 <= n < 8` two overlapped 8-byte copies (at `lo` and at `hi-4`; `vld1_u16`/`vst1_u16` or
  `memcpy` of 8); `2 <= n < 4` two overlapped 4-byte copies; `n == 1` a halfword. Four-way branch on
  a per-span-row size class instead of `memcpy`'s internal dispatch.
* **D/D'** stays a switch through R1 and is decided at the R1 gate (D' removes line loads and
  interleaves row streams; it edged D on pi2-local, lost on sl-d303 -- both within a few percent).
  Whichever wins becomes the compiled default; the other stays behind `-spritereplay` for the
  harness.
* **Shadow bookkeeping only when `ctx.shadow`.** The S3 prefetch scan and the `shadow_rows` marking
  are skipped for sprites without the flag (the majority); the shadow bitmap semantics are
  unchanged because a non-shadow sprite never sets a bit.
* **Glow** keeps the S2 rule and the S3 filter. Unroll `glow_flush` by four with the four loads
  issued before the four `and`/`store` pairs so the in-order core overlaps the misses; keep the ring
  at 64 (L1 residency argument of `DESIGN_REVIEW_BANDWIDTH` section 4). Measure with `-noglow`
  (R0b); if glow is still > 0.05 ms at heavy after the unroll, halve the ring.

### 3.5 What stays exactly as it is

The fallback envelope and `draw_sprite_px`; `parse_record`; the record rules and `SpanRec`; the
glow rule, deferral point and ring; `shadow_rows` and `dest_clean`; the `sched` cache (2.3 KB per
zoom; ~160 zooms; first-touch misses ~0.03 ms/frame **[E]** -- a tie against per-sprite generation
per `DESIGN_REVIEW_BANDWIDTH` section 5, revisit only if R0e shows it); the whole S0 harness
(`selftest`, `unit`, `fuzz`, `replay`, `golden`, the `_s1` twins, S0.8 checksums).

### 3.6 What is not done

S4c (NEON expansion via `tbl`): `vqtbl1q_u8` is AArch64-only and the A7's `vtbl` returns 8 bytes;
decode is amortised over rho^2 and is not where the time went. S5 (shrink regime): hi-res default
never shrinks. S6.1 (left-edge fast-forward): an optimisation for widescreen-off left-crossing
sprites; keep for later, it is orthogonal. No occlusion, no multi-core, no band composition.

### 3.7 Codegen checklist (part of the R1 gate, both ISAs)

`objdump -d` of the four `draw_sprite_fast` instantiations: no `bl` inside them except to
`glow_flush` at sprite end and to `draw_sprite_px` never; no `sdiv`/`udiv`; no `mul x`/`smull` in
the nibble loop; no loads from `[this]`-relative or `[r]`-relative addresses inside the unrolled
nibble body; on A32 no `str rN, [sp, #..]` spills inside the nibble body (one or two in the MIXED
path is acceptable); no `strd`/`ldrd` on a 2-byte-aligned pointer.

---

## 4. Expected effect by load band

Basis: pi2-local shipped 0.778 / 1.409 / 3.374 ms (light / moderate / heavy). Model: shipped
compute ~11-13 cyc/visited px falling to ~8 at heavy **[M, INSITU M-C]**; tight LINEBUF compute
~5.8 cyc/opaque px on big sprites **[M, bench]** rising toward ~8-9 at rho ~ 1 **[E]**; the
destination memory term (0.10-0.37 ms) and the shadow RMW are common to both.

| band | S4-D now | after R1 (expected) | after R1 (gate) | after R4 if done |
|---|---|---|---|---|
| light | 1.248x | 0.77-0.92x | <= 0.90x | -5 to -10% more |
| moderate | 1.282x | 0.71-0.85x | <= 0.90x | -3 to -5% |
| heavy | 1.206x | 0.65-0.80x | <= 0.90x | ~0 |

Why heavy improves most: once the fixed per-row term is small, the per-pixel structural
advantage (decode once per source row, 16-byte store-only replay) dominates, diluted only by the
common memory term (11% of the shipped heavy frame). Why light improves least: rows are short, the
remaining per-row cost is amortised over fewer pixels, and at rho ~ 1 the line-buffer round trip
(store + load + store) is more memory ops per output pixel than the shipped single store. If R1
lands at the bottom of the light range, R4 exists for exactly that class.

On the A7 the same code runs with the same NEON copy (section 5); the scalar decode has fewer
registers and no dual issue, and the shipped kernel was tuned on this core (`COMPARISON.md`), so
the margin is expected to be thinner: 0.80-0.95x **[E, unmeasured]**. Gate <= 0.90x there too;
if it cannot be reached on the A7 while the A53 passes, the A7 ships whichever of R1-LINEBUF and
R3-legacy measures faster on that box.

Honesty note: every number in this table is a model; R0 turns the light/heavy row and span
counts into measured inputs and the R1 gate replaces the table.

---

## 5. SIMD portability: AArch64 and ARMv7 from one source

### 5.1 What S4 uses and what else the tree uses

Sprite NEON at HEAD: `vld1q_u16`, `vst1q_u16`, `vorrq_u16`, `vdupq_n_u16` -- all in the
ARMv7/AArch64 common subset. The only AArch64-only intrinsics in `src/main` are in
`hwtiles.cpp::tile_row_hires`: `vzip1q_u16`, `vzip2q_u16`, `vzip1_u8`. `hwroad.cpp` uses
`vdupq_n_u32`/`vst1q_u32` (portable). `snes_ntsc_simd.h` already detects NEON with
`defined(__ARM_NEON) || defined(__ARM_NEON__)` and is the pattern to follow.

### 5.2 `hwarch.hpp` restructure

```cpp
#if !defined(CB_FORCE_SCALAR)
  #if defined(__ARM_NEON) || defined(__ARM_NEON__)   // AArch64: always. AArch32: only with
    #include <arm_neon.h>                            //   -mfpu=neon* or -march=...+simd
    #define CB_HAVE_NEON 1                           // portable ARMv7/AArch64 subset only
    #if defined(__aarch64__)
      #define CB_HAVE_NEON_A64 1                     // AArch64-only dialect, if ever needed
    #endif
  #endif
#endif
#if defined(CB_HAVE_NEON_A64)     #define CB_ARCH_NAME "NEON-A64"
#elif defined(CB_HAVE_NEON)       #define CB_ARCH_NAME "NEON-A32"
#else                             #define CB_ARCH_NAME "scalar"
#endif

#if defined(CB_HAVE_NEON)
// Shims for the three AArch64-only zips hwtiles uses; identical codegen on A64.
static inline uint16x8_t cb_zip1q_u16(uint16x8_t a, uint16x8_t b) {
#if defined(CB_HAVE_NEON_A64)
    return vzip1q_u16(a, b);
#else
    return vzipq_u16(a, b).val[0];
#endif
}
// cb_zip2q_u16 -> vzip2q_u16 / vzipq_u16(a,b).val[1];  cb_zip1_u8 -> vzip1_u8 / vzip_u8(a,b).val[0]
#endif
```

Rule for the file: anything under plain `CB_HAVE_NEON` must compile on both dialects; anything
AArch64-only goes under `CB_HAVE_NEON_A64` with a `CB_HAVE_NEON` alternative beside it. The sprite
kernel needs nothing under `_A64`. `hwtiles.cpp` swaps its three intrinsics for the shims and
`__builtin_bswap32` for `cb_bswap32` (three lines; the tile NEON path then also runs on the A7,
which is a free win for that layer). MSVC ARM64 stays scalar as today.

### 5.3 Build flags

* Game: `WITH_MARCH_NATIVE=ON` (default) gives `-march=native`, which on a Cortex-A7 with GCC >= 8
  resolves to `armv7ve+simd` and defines `__ARM_NEON` (confirmed on the Rev 1.1 box, test-mule
  note 2026-09-03); on ARMv6 it resolves to `armv6kz+fp` -> scalar; on AArch64 to
  `cortex-a53+crc`. Raspberry Pi OS armhf **defaults to ARMv6 with no NEON**, so a Release build
  is required on the A7 and Debug builds silently go scalar. Add a configure-time message printing
  the detected arch (`check_cxx_source_compiles` on `#ifndef __ARM_NEON #error`), and print
  `CB_ARCH_NAME` in the `-perftest` report so a measurement can never again be taken on the wrong
  path.
* Add `option(WITH_NEON32 "Force -mfpu=neon-vfpv4 on armv7l when not using -march=native" OFF)`
  for non-native 32-bit builds.
* Driver (`tools/sprite_bench/test/CMakeLists.txt`): add `-mcpu=cortex-a7 -mfpu=neon-vfpv4` for
  `CMAKE_SYSTEM_PROCESSOR MATCHES "armv7"` on the `sprite_test` target so the native driver
  actually exercises NEON there; `sprite_test_scalar` unchanged.
* One binary cannot serve Pi 1 and Pi 2 v1.1 with NEON enabled: NEON instructions SIGILL on
  ARMv6. Keep per-machine builds (current practice). Runtime dispatch via `getauxval(AT_HWCAP) &
  HWCAP_NEON` plus `__attribute__((target("fpu=neon")))` is possible in GCC but fragile with
  `arm_neon.h` on AArch32 and is not recommended.

### 5.4 32-bit caveats that bite this kernel

1. **Registers.** 13 usable GPRs (AAPCS32). No `int64` in any per-word or per-nibble path (each
   costs a register pair) -- section 3.2's per-sprite bounds hoist is as much an A7 register fix as
   a speed fix. Keep the live set of the nibble loop <= 12: `pos`, `wp`, `wp_end`, `pend`,
   `color`, `runs` (pair on A32), `w`, `line`, `rp`, `prev`, `nrec`, one scratch. Check the
   disassembly for spills (section 3.7).
2. **Alignment.** `LDRD`/`STRD`/`LDM`/`STM` fault on non-word-aligned addresses regardless of
   `SCTLR.A`; `LDR`/`STR`/`LDRH`/`STRH` and NEON `VLD1`/`VST1` with element size are fine at 2-byte
   alignment. Therefore: never cast `uint16_t*` to `uint32_t*`/`uint64_t*`; use `memcpy` (GCC emits
   `ldr`/`str` pairs at known 2-byte alignment) or `vld1_u16`/`vst1_u16`. The S1/S4 code already
   obeys this; keep it a review rule.
3. **Q-register cost.** The A7's NEON datapath is 64 bits wide, so `vld1q`/`vst1q` take two cycles
   each -- the same 8 bytes/cycle as the A53's two-issue-slot 128-bit ops (`MICROARCH` sections 3,
   6). The S4 loop shape is right for both. For the `4 <= n < 8` tail, `uint16x4_t` (`vld1_u16`)
   is the natural form on both.
4. **NEON is single-issue on the A7 and moving data NEON -> core registers is slow.** The copy
   path never does it; do not add a NEON reduction that feeds a scalar branch.
5. **Prefetch.** `__builtin_prefetch(p, 1, 3)` is `pldw` under `armv7ve` (Cortex-A7 has the MP
   extensions) and `pld` under plain `armv7-a`; either is fine.
6. **`cb_bswap32`** -> `rev` (ARMv6+), fine on all three ARM targets.
7. **Intrinsics present on both dialects** (used or proposed): `vld1q_u16`, `vst1q_u16`,
   `vld1_u16`, `vst1_u16`, `vorrq_u16`, `vdupq_n_u16`, `vzipq_u16`, `vzip_u8`, `vmovl_u8`,
   `vbslq_u16`, `vtstq_u16`, `vshr_n_u8`, `vand_u8`, `vreinterpret_u8_u32`, `vdup_n_u32`,
   `vaddq_u16`. **Absent on A32**: `vzip1q/vzip2q/vzip1`, `vqtbl*`, `vaddv*`, `vpaddq`,
   `vuzp1/2`, `vtrn1/2`.
8. **Toolchain.** GCC 14.2 armhf on the Rev 1.1 box; C++23 (`std::byteswap`) fine. `-Ofast` is
   irrelevant to this integer code.
9. **Measurement.** No `perf` tool on the box and the ARMv7 PMU needs `perf_event_open` with
   raw event codes that differ from the A53's (`0xE8`/`0xE7`/`0xC7` are A53 impdef events); the
   in-situ gate only needs `prof_ns_sprites`, which is ISA-independent.

---

## 6. ARMv6 fallback: correct and non-regressing

**Kernel:** the existing `render_ref` twin (a verbatim copy of the shipped `render()`, today under
`CB_RENDER_TEST`), promoted to a game path `render_legacy()` compiled when `!CB_HAVE_NEON &&
defined(__arm__)` (CMake option `SPRITE_KERNEL=legacy|linebuf`, default `legacy` there, `linebuf`
elsewhere). Edits, each a strict work reduction or a per-sprite constant:

1. **F1:** `ref_prepass_fix` forced true (`y <= sprite_height`). With it the two lazy tables are
   history-independent -- `selftest` phase B already proves `render_ref(fix=1) ==
   render_px(glow=false)` over 20,000 lists -- so the tables can stay, which is the zero-risk
   choice for a core we cannot measure today. Cost: one extra row of pre-pass per sprite.
2. **F2:** replace the engine `clip` bit as the unclipped-kernel selector with a renderer-computed
   exact bound: `wmax = ceil(512 * 8 * pitch / zoom)`; unclipped iff `xpos >= x1 && xpos + wmax <=
   x2`. One division per sprite (~3 us/frame **[E]**); no per-pixel cost; the `_nc` kernels can then
   never write outside the row. (The engine bit may be ANDed in as belt-and-braces; the clipped
   kernels are always correct.)
3. **3-row double draw:** `for (int i = 1; i < count; i++)` (advance `count-1` in-case) at the four
   sites `FABLE_ASSESSMENT` 4.1 names. Output identical (same source row, same values), 25% fewer
   stores on `r_v >= 3` rows -- the closest, largest sprites.
4. **`zoom == 0` guard** (the shipped loop never terminates on it; `parse_record` already skips).
5. **Glow, inline, at source-pixel granularity:** in each `draw_pixel_*` macro, before the
   `while (xacc < 0x200)` loop, if the source pixel is colour and the previous source pixel of this
   row was not colour and `x > x1`: clear bit 12 at `pPix[-1]` on each of the group's rows; the
   `rtl` amendment (`DESIGN_GLOW` section 2) suppresses it when the previous pixel was this row's
   shadow. ~3 instructions per *source* pixel, ~0.3 per output pixel at lo-res rho **[E]** -- 2-3%
   on an ARM11. Default on only if the ARMv6 gate measures <= 3%; otherwise default off there with
   `-glow` to enable (glow is a fidelity feature the shipped ARMv6 build never had).
6. Dead instrumentation is already gone in the twin; `x1/x2` are initialised by `init()` at HEAD.

**Why it does not regress:** items 2-4 remove work; item 1 adds one table probe per sprite; item 5
is opt-out. Nothing changes the per-output-pixel loop the shipped renderer spends its time in, and
the tables' behaviour is exactly the shipped one on a core whose 16 KB L1D and 128 KB L2 are the
reason not to introduce the line/record scratch there untested.

**Gate:** frame-count-matched cumulative `prof_ns_sprites` (the `MEASURED_S4` method) on a Pi 1
B+ / Zero W if one can be found; otherwise both kernels built `-march=armv6zk -mfpu=vfp` and run
on the A7 box (an instruction-count proxy; ARM11 cache behaviour is unmodelled -- say so in the
result). Correctness: `fuzz`/`replay` of `render_legacy` against `render_px` at both glow values,
plus S0.8 checksums in lo-res (the ARMv6 mode).

**Alternative kept available:** the R1-tightened LINEBUF scalar path (`SPRITE_KERNEL=linebuf`,
what `sprite_test_scalar` tests). If it measures <= 0.9x shipped on an A53 built
`CB_FORCE_SCALAR`, it is a candidate for ARMv6 too; it is not the default there until measured on
an ARM11.

---

## 7. Stages, order, gates

Every stage ends with: driver `unit` (all sections), `fuzz 200000` (both glow values, both replay
modes), `replay` (F1/F2 classifier, zero unclassified), `golden`, on both driver targets, on WSL
x86-64 and on an ARM box; then S0.8 whole-game checksum equality with the `SPRITE_REF_PX` build in
all four modes. Then the **in-situ gate**: the `MEASURED_S4_INSITU` procedure -- three clean
`git archive` builds (shipped `bdf7e9b`, previous stage, this stage), `-60 -t 4` attract,
`SDL_VIDEODRIVER=offscreen`, fixed 9000-frame cap, cumulative `prof_ns_sprites` per 300-frame
window, terciles by shipped cost, `throttled=0x0` before and after -- on `pi2-local` (A53, 900 MHz,
cool) as the deciding box and `sl-d303` as the cross-check. Make the frame cap and banded log a
permanent `CB_SPRITE_STATS` feature (there is already `CB_MAX_FRAMES` under `CB_FRAME_CKSUM` in
`main.cpp`) so the gate is one script and `patch_main.py` retires; the S1 measurement burned five
attempts on throwaway instrumentation.

| stage | content | effort **[E]** | gate / kill rule |
|---|---|---|---|
| **R0 attribute** | (a) `objdump` checklist on the S4 A53 build; (b) S4 `-glow` vs `-noglow`; (c) `CB_SPRITE_STATS` counters per 300-frame window: sprites, source rows, span-rows by type and `n` class, words by class, visited px, glow pushes/flushes, fallback; (d) cycle split via `cntvct_el0` deltas accumulated per frame around decode / colour replay / shadow replay / glow flush; (e) PMU group (cycles, `0x08` instructions, `0x10`, `0x03`, `0x17`, `0xE8`) around `render()` for shipped vs S4, from the existing `MEASURED_INSITU` build | 1 day | exit when >= 80% of the +0.19 / +0.40 / +0.70 ms is attributed; decides whether excess tracks rows or pixels and re-orders R1 |
| **R1a glue** | `SpriteCtx`, locals not members, per-sprite ROM bounds, one `sched()`, run bytes as one load, vertical loop via `run[j]`, shadow bookkeeping gated on `ctx.shadow`, glow flush unrolled | 2 days | correctness gates; in-situ <= 1.00x every band (must at least reach parity); codegen checklist |
| **R1b specialise + replay** | `<FLIP, MAXRUN>` templates, fill width by class, copy ladder replacing `memcpy`, D/D' decided | 2-3 days | in-situ **<= 0.90x every band**, expected <= 0.85x; D/D' picked by the heavy band; kill: if light > 1.0x with rows/frame no longer the top R0 term, go to R4 |
| **R2 portable NEON** | `hwarch.hpp` restructure + shims, `hwtiles` swap, CMake arch message and `WITH_NEON32`, driver flags; build and run `selftest`/`unit`/`fuzz`/`replay` both targets on the A7 box; in-situ on the A7 vs shipped built identically | 1 day + A7 access | 32-bit driver green; A7 in-situ <= 0.90x; if unreachable, A7 ships the faster of R1 and R3 on that box |
| **R3 ARMv6 legacy** | `render_legacy` (section 6), `SPRITE_KERNEL` option, harness coverage at both glow values, lo-res S0.8 | 1-2 days | correctness gates; ARMv6 (or A7-proxy) in-situ <= 1.00x; glow default decided by its measured cost |
| **R4 direct rows (conditional)** | inside `draw_sprite_fast<FLIP,2>`: when `run[j] == 1` (one destination row) emit straight to the row -- per source pixel: type test, two overlapped `strh` for `r_h in {1,2}` (exact width, no overrun), shadow RMW inline, glow clear on the gap/shadow -> colour transition, clip by `cx0/cx1` in index space with first/last pixel width clamped; no records, no replay | 2-3 days | only if R1b's light band is > 0.85x; kill if light gain < 8% or any other band regresses |
| **R5 polish** | S6 items 2-4 (canaries, flags, `PIXEL_ACCURACY` text), `PROBLEM_STATEMENT` 11a note, retire `_s1` twins' `replay_mode` if one mode is dropped, S6.1 if widescreen-off left clipping shows in R0c | 1 day | correctness gates only |

Order rationale: R0 is cheap and makes R1's list evidence-based rather than argued; R1a alone
should reach parity and shows whether the model in section 1.3 is right before the templates are
written; R2 is independent of R1's outcome and unblocks the A7 gate; R3 is independent of both and
is the only path for ARMv6; R4 is the safety valve for the one band the structure is weakest in.

---

## 8. Risks and unknowns

* **The attribution is a model until R0 runs.** If the excess tracks pixels at heavy load (R0c),
  the top suspect becomes item 2 (alias reloads in the nibble loop) and R1a's ordering flips;
  the fix set is the same.
* **A7 is unmeasured.** No LINEBUF number exists on a Cortex-A7. Register pressure and the weaker
  predictor could compress the margin to nothing; section 7's fallback (ship the faster of R1/R3
  on that box) bounds the downside at "no worse than shipped".
* **ARMv6 has no mule.** The non-regression claim for R3 rests on the edits being work-reducing
  plus a proxy measurement; the LINEBUF-scalar alternative for ARMv6 is explicitly not proven.
* **D/D' is not robust across A53 SKUs** (D won on sl-d303, D' on pi2-local). Both are correct;
  the compiled default is a few-percent decision and the switch stays for the harness.
* **Glow's true in-situ cost is unknown** (R0b measures it). If it is large and the unroll does
  not fix it, the alternatives are a smaller ring or a per-row flush; both keep the S2 rule.
* **`sched_tab` misses** are estimated at ~0.03 ms; if R0e shows L2 refills well above the
  shipped kernel's, compacting the table (only `8*pitch` entries are ever touched per sprite) or
  per-sprite generation into L1 scratch is a half-day change.
* **Code growth:** four template instantiations plus the legacy kernel. L1I is 32 KB on all three
  cores; the hot set per frame stays under ~8 KB **[E]**.

---

## 9. Test-mule facts relevant to this plan

* `pi2-local` was **re-pointed on 2026-09-05** from a Pi 2 Model B **Rev 1.1** (Cortex-A7, CPU part
  0xc07, armhf, 10.240.1.125, under-voltage PSU) to a Pi 2 Model B **Rev 1.2** (BCM2837 Cortex-A53,
  aarch64, 10.240.1.138, new PSU, `throttled=0x0`). The S4 verdict was taken on the Rev 1.2 with
  `CB_HAVE_NEON` active, so it stands. The Rev 1.1 board is the ARMv7 gate box for R2 if it is still
  connected; it needs the PSU fixed first (`0x50005` at setup).
* `sl-d303` (Zero 2 W, A53 1 GHz) is the cross-check box and has the PMU wrapper and the M-A/M-B/M-C
  instrumented builds in `~/cbgame`.
* `pi3mule` is under-voltage throttled to 600 MHz under load; cycle counts only.
* No ARMv6 board is on record.
