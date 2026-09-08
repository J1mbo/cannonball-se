# Engineering backlog

Open work, indexed. Each item names the document that holds the full reasoning, so this file
stays a list rather than becoming a second source of truth.

Last reviewed 2026-09-08.

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

## GPU / shaders

Ranked future work in [`GLSL_SHADER_PERFORMANCE_ANALYSIS.md`](GLSL_SHADER_PERFORMANCE_ANALYSIS.md)
section 4. Note the constraint from section 1: roughly 46% of the full-shader frame at 1280x1024 is
the framebuffer store, which is memory-bound. No shader change touches that floor — only a narrower
framebuffer format (RGB565) would.

---

## Documentation

- `ntsc_bench/analysis/GRAPHICS_SETUP.md` covers palette mechanics and hi-res-vs-doubled rendering,
  which outlive the NTSC filter work it currently sits under. It is a candidate for relocation to
  general engine documentation. It is cited by section number from
  `src/main/sdl2/rendersurface.cpp`, so any move must update that comment in the same commit.
