# Engineering backlog

Open work, indexed. Each item names the document that holds the full reasoning, so this file
stays a list rather than becoming a second source of truth.

Last reviewed 2026-09-09.

---

## Sprites

### R3 — ARMv6 / no-NEON legacy kernel promotion

**Status: planned, never implemented.** Full plan in
[`sprite_bench/analysis/SPRITE_REPLAN_FABLE.md`](sprite_bench/analysis/SPRITE_REPLAN_FABLE.md)
section 6.

R1/R2 delivered the de-glued LINEBUF kernel with portable NEON covering both AArch64 and 32-bit
ARMv7 (Cortex-A7). R3 was to add a path for targets with **no** NEON at all — ARMv6 boards, i.e.
the Pi 1, Pi Zero (W) and the 2014 Pi B+ — by promoting a scalar legacy kernel behind a build
selector.

Evidence it did not ship, re-verified 2026-09-08:

- No `SPRITE_KERNEL` option in `CMakeLists.txt` (the file declares 14 `option()` entries; this is
  not among them).
- No `render_legacy` symbol anywhere in `src/`.

Consequence today: ARMv6 boards run the same kernel as everything else, relying on the compiler's
scalar codegen. Those boards are already restricted to 30fps at original resolution with the NTSC
filter disabled (applied automatically at launch — see `docs/PERFORMANCE-TUNING.md`), so this is a
question of whether there is headroom to reclaim there, not a correctness gap.

**Decide before doing anything:** whether ARMv6 is still a target worth kernel-level work. If it
is not, delete section 6 of `SPRITE_REPLAN_FABLE.md` and this entry rather than leaving a plan
that reads as pending indefinitely.

### Open questions on sprite priority and working set

See the "Open questions" section of [`sprite_bench/README.md`](sprite_bench/README.md). Both
predate the rewrite and neither was closed by it.

---

## Audio

### F1.1 and F1.2 — free, bit-identical CPU wins

Full detail in [`AUDIO_PERF_PLAN.md`](AUDIO_PERF_PLAN.md) section 4.

- **F1.1** — inline `read_buffer`/`write_buffer` (`soundchip.cpp:57-66`). LTO is off in the default
  Release build, so these cross-translation-unit calls are real.
- **F1.2** — hoist the loop-invariant PCM increment (`segapcm.cpp:142-143`): evaluated 5632 times
  per slot where 16 would do.
- Related: `SoundChip::clear_buffer()` (`soundchip.cpp:51-55`) is still a byte loop and could be a
  `memset`.

F1.3 shipped in `c94f086`.

**Blocked on:** the `-audiodump` bit-exactness harness (`AUDIO_PERF_PLAN.md` section 3). These
changes are meant to be bit-identical, and that claim needs to be checkable before they land.

---

## Per-frame memory work

Tiers 2 and 3 of [`PERFRAME_MEMORY_INVENTORY.md`](PERFRAME_MEMORY_INVENTORY.md) remain open;
Tier 1 shipped in `c94f086`. Tier 2 items are conditional and want confirming measurements first.

---

## Test gates

### Golden frame checksums are stale

**Status: confirmed stale, needs a decision then a re-baseline.** The four values in
[`road_tiles/analysis/GOLDEN_CHECKSUMS.md`](road_tiles/analysis/GOLDEN_CHECKSUMS.md) no longer
match the tree and have not for some time. Established 2026-09-09 while gating `0f1fe9a`: the run
recipe is still correct (rebuilding `fcf9d05`, the commit that introduced `CB_FRAME_CKSUM`, from a
pristine `git archive` reproduces all four documented values exactly), so this is real code drift,
not a broken procedure. The pixels moved at least twice — somewhere in the sprite S0-R2 / Tier 1
series, all of which were described as bit-exact, and again at `bd06b6e`, which changed
default-path sprite geometry and where a move was expected.

Current values at `0f1fe9a` (600 frames, `-perftest -t 1`, offscreen SDL), measured independently
by three agents in separate build trees:

| mode | hires | widescreen | checksum |
|---|---|---|---|
| hires_nw | 1 | 0 | `0x05d98764ff87acd5` |
| lores_nw | 0 | 0 | `0x6630b4b099cf1157` |
| hires_wide | 1 | 1 | `0x432cb013834a6d87` |
| lores_wide | 0 | 1 | `0x20674347e186b8e3` |

Why it needs a decision rather than a straight overwrite: the "bit-exact" claim on the S0-R2 /
Tier 1 commits is either wrong or the gate was not run on them, and blessing the current numbers
without knowing which would bake in whatever changed. Worth identifying the commit that moved the
pixels first. Until then the gate is only usable as a *relative* check between two commits, which
is how `0f1fe9a` was verified.

Also unrecorded: the document does not say what the four config files contain, so the recipe has to
be reconstructed each time. Record them alongside the values.

---

## GPU / shaders

Ranked future work in [`GLSL_SHADER_PERFORMANCE_ANALYSIS.md`](GLSL_SHADER_PERFORMANCE_ANALYSIS.md)
section 4. Note the constraint from section 1: roughly 46% of the full-shader frame at 1280x1024 is
the framebuffer store, which is memory-bound. No shader change touches that floor — only a narrower
framebuffer format (RGB565) would.

---

## NTSC filter

### Synthetic-pattern checksums are not build-portable

**Status: understood, not fixed. Decide whether it needs fixing.** Full detail and figures in
[`ntsc_bench/analysis/ARCHITECTURE.md`](ntsc_bench/analysis/ARCHITECTURE.md) section 17.1.

Building `ntsc_bench` for a baseline target (`-march=x86-64`) produces different checksums from
`-march=native` on all three synthetic patterns:

| Pattern | `-march=native` | `-march=x86-64` |
|---|---|---|
| random, 256 | `0x95f5c92e642a686f` | `0xc928ffe2395b1fa7` |
| mixed, 256 | `0x2921e1e8ab232056` | `0x0616580b4a8a3d6e` |
| runs, 256 | `0x0f9860292b10dd73` | `0xfead91f93878fb60` |

Captured frames give `0xe3557fa06310379d` on **both**, so the blitter's integer work is
bit-portable and the shipped game is unaffected. The cause is FMA contraction in the
floating-point table generation (`snes_ntsc_init` / `snes_ntsc_impl.h`): building native with
`-ffp-contract=off` reproduces the baseline value exactly. Reproduced 2026-09-08 on x86-64,
GCC 13.3.

Why it is still worth a decision:

- The synthetic reference checksums in `ntsc_bench/README.md` are only valid for an
  FMA-capable build. Anyone running the gate on a different target sees three failures and has
  to work out that they are benign. The table now says so, but a gate that needs a caveat is a
  weaker gate.
- It means the generated NTSC table is not bit-identical across build targets. That is
  invisible on real content today, but it is an assumption worth knowing is false if anyone
  later tries to cache, precompute or cross-check the table between platforms.

Options, cheapest first: leave it documented; make the bench print its FP-contraction state in
the RESULT line so a mismatch is self-explaining; or make the table generation reproducible
(build the table with `-ffp-contract=off` on a dedicated translation unit, or in fixed point),
which would cost a re-baseline of every synthetic checksum.

Related, same section: `--colours 65536` can no longer be requested - the slot-indexed table
caps at `snes_ntsc_slot_count` (8192), and the bench clamps and warns. That row of the
reference table is a historical record, not a runnable check.
