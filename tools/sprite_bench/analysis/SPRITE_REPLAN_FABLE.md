# Sprite renderer re-plan: from the measured S4 regression to an accelerated A7/A53 path

Written after the S4 NEON rewrite was measured to still regress in-game
(`PERFORMANCE_MEASUREMENTS.md` campaign 5) despite winning an idle bench
(campaign 1). This document re-attributes the cause, chooses the route that
became R1/R2, and lays out the ARMv6 fallback plan (R3). It has been
condensed from its original form: session narrative, the pre-R0 ranked-
suspects table, the pre-implementation "expected effect by load band"
projections, the staging/gate schedule, and test-mule status notes have all
been stripped as superseded by what was actually measured (see
`PERFORMANCE_MEASUREMENTS.md` campaigns 5-7) and actually built (see
`SPRITE_R1_LLD.md`, in particular its section 9 "deviations from the
replan, and why" for exactly how the shipped R1/R2 code differs from the
sketch below). Section numbering is preserved from the original document
because `hwarch.hpp` cites section 5.2 by number.

Targets: accelerate on Cortex-A53 (AArch64 NEON: Pi 2 v1.2, Pi 3, Zero 2 W)
**and** on Cortex-A7 (ARMv7, 32-bit NEON: Pi 2 v1.1); a corrected scalar
fallback for ARMv6 (Pi 1 / Zero v1) that does not regress against the
shipped renderer.

---

## 0. Verdict

**The load-shape reading is right; the attribution is wrong.** A regression
that is worst at light load and narrows without closing is a fixed per-row
cost, and the S4 code has one. But it is not the on-the-fly flip/shadow
classification that replaced `sprites_flipped`/`sprites_shadowinfo`.
Counted from the code, `nibrev()` plus `classify_mixed()` plus the two
word-class compares are at most ~21 ALU ops per source *word* (eight source
pixels) — a tenth to a fifth of the light-band deficit, under a tenth of
the heavy one — and the thing they replaced was a dependent byte load per
row from a table that is cold in L2 every frame. Restoring the tables would
cost more than it saves and would bring back the F1 history-dependence bug.

The cost that fits both bands is the **glue around the LINEBUF kernel**: per
source row, two function calls, two `sched()` lookups, `int64` per-word
bounds arithmetic, a `std::min`/`config` reload, the S3 shadow-prefetch scan
run for every sprite, and a `(d*zoom)>>9` multiply per destination row; per
span-row, a libc `memcpy` call, a D/D' branch, the glow bookkeeping, and
NEON tail handling; plus alias-driven reloads of `this`-members and `r.*`
fields after every 8-byte `memcpy` store inside `decode_row`. A uniform
~150 cycles of excess per source row reproduces the measured per-frame
excess at every load band if frames have roughly 1.2k/2.4k/4.2k source rows
(light/moderate/heavy) — i.e. rows of ~45 output pixels at light and ~75 at
heavy. The bench LINEBUF kernel that measured 5.8 cyc/px
(`PERFORMANCE_MEASUREMENTS.md` campaign 1) had none of that glue; the
in-situ S4 is ~12 cyc/px on the same structure. This diagnosis was
subsequently confirmed by direct measurement — see
`PERFORMANCE_MEASUREMENTS.md` campaign 6 (R0 attribution): the deficit
tracks source rows per frame, not pixels, exactly as predicted here.

**Recommendation: route (c), a hybrid.**

| Target | Kernel | Stage |
|---|---|---|
| A53 (AArch64) and A7 (ARMv7 + NEON) | LINEBUF, glue rebuilt, per-sprite specialised, NEON span copy/OR made ISA-portable | R1, R2 |
| ARMv6 (no NEON) | the shipped kernel (`render_ref` twin) with F1, F2, the 3-row double-draw fix and inline glow; side tables kept | R3 |
| x86-64 / MSVC / other | LINEBUF scalar (`CB_FORCE_SCALAR` twin), correctness only | -- |
| light-band residue, if R1 leaves it | direct-emit rows (`r_v = 1`, zoom >= 0x100) inside LINEBUF | R4, conditional |

Do **not** restore the side tables; do **not** pursue NEON `tbl`-based
decode (AArch64-only and not the bottleneck); do **not** abandon LINEBUF
(route b) for the accelerated path — a per-output-pixel DDA has no SIMD
formulation that does not read the destination, and the bench already
showed LINEBUF 2x ahead of it.

---

## 1. Why: the glue-cost diagnosis

Reading `draw_sprite_fast` -> `decode_row` -> `replay` -> `replay_colour_span`
at the S4 HEAD used for this analysis, per **source row**:

1. `prefetch_rom_row()` call: two `int64` bounds checks and two prefetches.
2. `decode_row()` call: `sched()` and `wsum()` (two `unique_ptr` lookups), an
   `int64 rowbase`, then per word an `int64` `widx` select-and-two-compares,
   and per nibble an 8-byte `memcpy` store into `line[]` followed by a
   `run[words*8+k]` byte load. `memcpy` stores have alias set 0, so
   `r.color`, `r.shadow`, `r.flip`, `nrec`, `rec`, the `run` pointer are all
   legally reloadable after each one; `nrec` is a member, so `emit()`
   round-trips it through memory.
3. `replay()` call: `config.s16_width`/`config.s16_height` loads, `std::min`
   for `d_hi`/`pf_hi`, then the S3 shadow-prefetch scan over every record
   **for every sprite, shadow flag or not**, then the record loop.
4. In `draw_sprite_fast`, one `int64` multiply per destination row to find
   the group end.

Per **span-row**: the `replay_mode` branch; for `n >= 8` the NEON loop with
its `min()`-rebased tail (one mispredict per span-row on the exit); for
`n < 8` a libc `memcpy` call (~20-30 cycles including the size-class
branches); the glow test, `shadow_rows` bit test and `glow_push`.

Estimated at the time: 100-150 cycles per source row and 25-40 per
span-row over what the shipped kernel spends on its row group. At ~45
output px per source row (light) that is 3-4 cyc/px; at ~75 (heavy) about
2 — which is the whole measured excess in both bands, and explains why the
shipped kernel's *lower* fixed cost wins even though its per-pixel cost is
2x LINEBUF's. It also explains why the idle bench never saw it: the bench
drew large sprites (rows of hundreds of pixels replayed 3-4 times), which
amortises exactly this per-row cost away.

Why the side-table recompute cannot carry the regression: per source word
in `decode_row`, `nibrev`/word-class tests/`classify_mixed` are at most
~21 ALU ops per 8 source pixels, which works out to roughly 0.02-0.05ms
per light frame and ~0.05ms per heavy frame — a tenth to a fifth of the
light-band deficit, under a tenth of the heavy one. That is real but
secondary, and it is cheaper than what it replaced: the shipped kernel's
own pre-pass walks `sprite_height` rows of the shadow-info table per
sprite per frame on top of a per-row-group table load, so that table was
never "read cheaply" in the shipped kernel either.

---

## 2. Route assessment

**(a) as phrased -- keep LINEBUF, restore a cheap cached flip/shadow
representation.** Rejected. The recompute costs ~0.02-0.05ms per frame; a
cache is a cold load per row and reintroduces the mechanism behind the F1
history-dependence bug. A one-time source transform (pre-mirrored ROM)
doubles the ROM working set on the contested L2 for zero compute saving.

**(a') keep LINEBUF, eliminate the per-row and per-span glue, specialise
per sprite.** Adopted as the accelerated path. The structure is proven
(bit-exact against the oracle, F1/F2 fixed, glow correct) and its
per-pixel advantage is measured (bench 2.0x compute, 1.76x under a
thrasher). What failed was the wrapping, and every fix is local.

**(b) abandon LINEBUF; optimise the shipped renderer; port F1/F2 + glow +
SIMD.** Rejected for the accelerated path, adopted for the ARMv6 fallback
(section 6). The shipped kernel's per-output-pixel DDA has a loop-carried
`xacc` chain and two data-dependent branches per output pixel; the only
NEON formulation that fits it is a masked blend that loads the destination
per 16 pixels — a load stall on every cold line, the exact thing the
store-only LINEBUF avoids. "Shipped + fixes" tops out at perhaps 10-20%
from the double-draw fix and dropping the pre-pass misses. That is a fine
fallback and the wrong ceiling for A53/A7.

**(c) hybrid.** Chosen; the table in section 0 says what runs where. The
extra maintenance is one already-existing kernel (`render_ref`, compiled
under `CB_RENDER_TEST` and already proven equal to `render_px(glow=false)`)
promoted to a game path for one ISA, plus one conditional stage.

---

## 5. SIMD portability: AArch64 and ARMv7 from one source

This is the mechanism that lets the same accelerated LINEBUF kernel serve
both halves of the "A53 and A7" row of the hybrid table in section 0.

### 5.1 What S4 uses and what else the tree uses

Sprite NEON at the time: `vld1q_u16`, `vst1q_u16`, `vorrq_u16`,
`vdupq_n_u16` — all in the ARMv7/AArch64 common subset. The only
AArch64-only intrinsics in `src/main` were in `hwtiles.cpp::tile_row_hires`:
`vzip1q_u16`, `vzip2q_u16`, `vzip1_u8`. `hwroad.cpp` uses
`vdupq_n_u32`/`vst1q_u32` (portable). `snes_ntsc_simd.h` already detects
NEON with `defined(__ARM_NEON) || defined(__ARM_NEON__)` and is the
pattern to follow.

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

Rule for the file: anything under plain `CB_HAVE_NEON` must compile on both
dialects; anything AArch64-only goes under `CB_HAVE_NEON_A64` with a
`CB_HAVE_NEON` alternative beside it. The sprite kernel needs nothing under
`_A64`. `hwtiles.cpp` swaps its three intrinsics for the shims and
`__builtin_bswap32` for `cb_bswap32` — the tile NEON path then also runs on
the A7, a free win for that layer. MSVC ARM64 stays scalar.

**Two-tier detection, in one place (the fact `hwarch.hpp` cites this
section for):** `CB_HAVE_NEON` is the portable ARMv7/AArch64 NEON subset —
defined whenever the compiler offers NEON at all: AArch64 always (when not
force-scalar); AArch32 only when built with `-mfpu=neon*` or an
`-march=...+simd` that turns NEON on (Raspberry Pi OS armhf defaults to
ARMv6 with no NEON, so this is off there unless asked for). Code under
plain `CB_HAVE_NEON` must compile on both dialects. `CB_HAVE_NEON_A64` is
the AArch64-only dialect layered on top (`vzip1q_u16` etc.); anything gated
on it needs a `CB_HAVE_NEON`-only alternative beside it, which is exactly
what the `cb_zip1q_u16`/`cb_zip2q_u16`/`cb_zip1_u8` shims above provide for
`hwtiles.cpp`'s `tile_row_hires`.

### 5.3 Build flags

* Game: `WITH_MARCH_NATIVE=ON` (default) gives `-march=native`, which on a
  Cortex-A7 with GCC >= 8 resolves to `armv7ve+simd` and defines
  `__ARM_NEON` (confirmed on a Rev 1.1 test box); on ARMv6 it resolves to
  `armv6kz+fp` -> scalar; on AArch64 to `cortex-a53+crc`. Raspberry Pi OS
  armhf **defaults to ARMv6 with no NEON**, so a Release build is required
  on the A7 and Debug builds silently go scalar. A configure-time message
  prints the detected arch, and `CB_ARCH_NAME` is printed in the
  `-perftest` report so a measurement can never be taken on the wrong path
  unknowingly.
* `option(WITH_NEON32 "Force -mfpu=neon-vfpv4 on armv7l when not using
  -march=native" OFF)` for non-native 32-bit builds.
* One binary cannot serve Pi 1 and Pi 2 v1.1 with NEON enabled: NEON
  instructions SIGILL on ARMv6. Per-machine builds are required. Runtime
  dispatch via `getauxval(AT_HWCAP) & HWCAP_NEON` plus
  `__attribute__((target("fpu=neon")))` is possible in GCC but fragile
  with `arm_neon.h` on AArch32 and is not recommended.

### 5.4 32-bit caveats that bite this kernel

1. **Registers.** 13 usable GPRs (AAPCS32). No `int64` in any per-word or
   per-nibble path (each costs a register pair) — hoisting ROM bounds to
   per-sprite (rather than per-word) is as much an A7 register fix as a
   speed fix. Keep the live set of the nibble loop <= 12 values.
2. **Alignment.** `LDRD`/`STRD`/`LDM`/`STM` fault on non-word-aligned
   addresses regardless of `SCTLR.A`; `LDR`/`STR`/`LDRH`/`STRH` and NEON
   `VLD1`/`VST1` with element size are fine at 2-byte alignment. Therefore:
   never cast `uint16_t*` to `uint32_t*`/`uint64_t*`; use `memcpy` (GCC
   emits `ldr`/`str` pairs at known 2-byte alignment) or
   `vld1_u16`/`vst1_u16`.
3. **Q-register cost.** The A7's NEON datapath is 64 bits wide, so
   `vld1q`/`vst1q` take two cycles each — the same 8 bytes/cycle as the
   A53's two-issue-slot 128-bit ops. The LINEBUF loop shape is right for
   both. For short tails, `uint16x4_t` (`vld1_u16`) is the natural form on
   both.
4. **NEON is single-issue on the A7 and moving data NEON -> core registers
   is slow.** The copy path never does it; no NEON reduction should feed a
   scalar branch.
5. **Prefetch.** `__builtin_prefetch(p, 1, 3)` is `pldw` under `armv7ve`
   (Cortex-A7 has the MP extensions) and `pld` under plain `armv7-a`;
   either is fine.
6. **Toolchain.** GCC 14.2 armhf on the Rev 1.1 test box; C++23
   (`std::byteswap`) fine there. `-Ofast` is irrelevant to this integer
   code.
7. **Intrinsics present on both dialects**: `vld1q_u16`, `vst1q_u16`,
   `vld1_u16`, `vst1_u16`, `vorrq_u16`, `vdupq_n_u16`, `vzipq_u16`,
   `vzip_u8`, `vmovl_u8`, `vbslq_u16`, `vtstq_u16`, `vshr_n_u8`, `vand_u8`,
   `vreinterpret_u8_u32`, `vdup_n_u32`, `vaddq_u16`. **Absent on A32**:
   `vzip1q`/`vzip2q`/`vzip1`, `vqtbl*`, `vaddv*`, `vpaddq`, `vuzp1/2`,
   `vtrn1/2`.
8. **Measurement.** No `perf` tool was available on the A7 test box and its
   PMU needs raw event codes that differ from the A53's; the in-situ gate
   only needs `prof_ns_sprites`, which is ISA-independent.

---

## 6. ARMv6 fallback (R3): planned, but not implemented

> Tracked in `tools/BACKLOG.md`. Re-verified as still unimplemented: no
> `SPRITE_KERNEL` CMake option and no `render_legacy` symbol in the tree.

**Status: this plan was never carried out.** No `SPRITE_KERNEL` CMake
option and no `render_legacy` symbol exist in the current tree (confirmed
by search). Only R1 and R2 (the A53/A7 accelerated path) shipped; the
ARMv6-specific fallback below remains an open item, recorded here because
this document is the only place the plan exists. No ARMv6 board has ever
been available to build or measure it against.

**Planned kernel:** the existing `render_ref` twin (a verbatim copy of the
shipped `render()`, compiled under `CB_RENDER_TEST`), to be promoted to a
game path `render_legacy()` compiled when `!CB_HAVE_NEON && defined(__arm__)`
(a `SPRITE_KERNEL=legacy|linebuf` CMake option, default `legacy` there,
`linebuf` elsewhere). Planned edits, each a strict work reduction or a
per-sprite constant:

1. **F1 fix:** force `ref_prepass_fix` true (`y <= sprite_height`). With it
   the two lazy tables become history-independent — `selftest` phase B
   already proves `render_ref(fix=1) == render_px(glow=false)` over 20,000
   lists — so the tables can stay, the zero-risk choice for a core that
   cannot be measured. Cost: one extra row of pre-pass per sprite.
2. **F2 fix:** replace the engine `clip` bit as the unclipped-kernel
   selector with a renderer-computed exact bound (`wmax = ceil(512 * 8 *
   pitch / zoom)`; unclipped iff the sprite's x-range fits inside the clip
   window). One division per sprite; no per-pixel cost; the unclipped
   kernels can then never write outside the row.
3. **3-row double draw fix:** advance the row-group loop by `count-1`
   instead of a fixed step, at the four sites this bug occurs. Output
   identical (same source row, same values), 25% fewer stores on the
   closest, largest sprites (where 3-row groups occur).
4. **`zoom == 0` guard**, since the shipped loop never terminates on it
   (parse_record already skips this case upstream, but the guard was
   planned as belt-and-braces).
5. **Glow, inline, at source-pixel granularity:** in each `draw_pixel_*`
   macro, before the DDA loop, if the source pixel is colour and the
   previous source pixel of this row was not colour, clear bit 12 at the
   preceding destination pixel on each of the group's rows (with the
   direction-dependent `rtl` suppression `DESIGN_GLOW.md` section 2
   describes). Estimated ~3 instructions per source pixel. Planned to
   default on only if the ARMv6 gate measures <= 3% cost; otherwise
   default off with a `-glow` opt-in (glow is a fidelity feature the
   shipped ARMv6 build never had).
6. `x1`/`x2` initialisation: already fixed at `init()` regardless (see
   `hwsprites.cpp`'s constructor comment).

**Why it was not expected to regress:** items 2-4 remove work; item 1 adds
one table probe per sprite; item 5 is opt-out. Nothing in the plan changes
the per-output-pixel loop the shipped renderer spends its time in.

**Planned gate:** frame-count-matched cumulative `prof_ns_sprites` on a
Pi 1 B+/Zero W if one could be found; otherwise both kernels built
`-march=armv6zk -mfpu=vfp` and run on an A7 box as an instruction-count
proxy (ARM11 cache behaviour unmodelled). Correctness: fuzz/replay of
`render_legacy` against `render_px` at both glow values, plus the S0.8
checksum gate in lo-res (the ARMv6 display mode).

**Alternative that remains available if R3 is picked up:** the R1-tightened
LINEBUF scalar path (`CB_FORCE_SCALAR`) is a candidate for ARMv6 too if it
measures <= 0.9x shipped on an A53 built scalar; it was not intended to be
the default there until actually measured on an ARM11-class core.
