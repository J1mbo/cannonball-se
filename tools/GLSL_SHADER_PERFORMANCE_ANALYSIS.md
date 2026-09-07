# CRT GLSL Shader — Performance Analysis and Hardware Notes (Raspberry Pi 2 / VideoCore IV)

Audience: maintainers and tools (human or AI) working on the CRT post-processing shaders.
Scope: `res/Cannonball-Shader-Fragment.glsl` (full look), `res/Cannonball-Shader-Fragment-Fast.glsl`
(fast look), `res/Cannonball-Shader-Vertex.glsl`, and their invocation in
`src/main/sdl2/gl_backend.hpp` and `src/main/sdl2/rendersurface.cpp`.

All figures below were measured on Raspberry Pi 2 class hardware (Cortex-A7, VideoCore IV / "VC4"
V3D 2.1, Mesa `vc4` driver, OpenGL ES 2.0) at a 1280x1024 output and a 500 MHz V3D clock unless
stated otherwise. The Pi 2 is the tightest target the project supports for the full CRT look, so it
is the reference platform for this analysis; the same shaders run on the other VideoCore IV boards
(Pi Zero 2 W, Pi 3) and on the V3D of the Pi 4/5, where headroom is larger.

---

## 1. Why the shader is the bottleneck, and how it maps to the hardware

The game frame is small (320x224 upscaled through the Blargg/NTSC path to ~752x224 or ~948x224),
but the CRT shader runs **once per output pixel** — 1.31 Mpx at 1280x1024, 2.07 Mpx at 1920x1080.
On the Pi 2 the full shader is what forces the GPU overclock: at the stock 250 MHz V3D clock the
full look does not hold 60 fps, while the lighter "fast" shader does. Everything in this document
is about closing that gap.

Key properties of the VideoCore IV that shape every optimisation decision:

- **fp32-only QPU.** The shader ALU is 32-bit float only. `mediump`/`lowp` qualifiers are ignored by
  the compiler — they change nothing, so do not spend effort on precision qualifiers for speed.
  (There is a portability caveat; see §5.)
- **Throughput.** 12 QPUs, each a 16-wide SIMD executed over 4 clocks, so the array retires ~48
  fragment-shader instructions per V3D clock. Measured slope: **~0.050 ms per FS instruction per
  frame** at 1280x1024 / 500 MHz (theory 1.31 Mpx / 48 / 500 MHz = 0.055). The ALU part of these
  shaders runs at essentially the QPU's theoretical throughput. At the stock 250 MHz clock the slope
  roughly doubles.
- **No native `floor`/`fract`.** Each becomes an integer-convert pair (`ftoi`/`itof`) plus sign
  fix-ups. This is why the shadow-mask pattern was so expensive (see §3).
- **No dependent-read penalty.** Every texture fetch — whether its coordinate is a varying or was
  computed in-shader — is issued by the QPU writing the TMU coordinate registers, and its latency is
  hidden by a single hardware thread switch. A fetch costs roughly 10–12 instruction slots
  regardless of where its coordinate came from. Consequence: **replacing ALU with an extra texture
  fetch only pays when the ALU removed is well above ~12 instructions.**
- **No branches, no loops, no transcendentals in these shaders.** The disassembly shows 0 branches
  and 0 SFU (special-function) ops; `step`/`mix` compile to compare + conditional select, which is
  the VC4-preferred form. `fastSin` is already a polynomial; divisions are pre-inverted uniforms.
  There is nothing to gain under these headings.
- **A hard, memory-bound floor.** ~46% of the full-shader frame at 1280x1024 (~5.1 ms) is not shader
  ALU at all — it is the RGBA8888 tile store of the backbuffer, which is memory-bound on the shared
  SDRAM bus (Pi 2 memcpy benches at roughly 0.7–1.0 GB/s). **No shader change can remove this floor;
  only a narrower framebuffer format can** (see §4, RGB565).

### Cost model (use this to predict other clocks / resolutions)

```
t_frame  ~=  t_floor(resolution, fb_format)  +  (N_instr - 15) * pixels / (48 * f_v3d)

t_floor(1280x1024, RGBA8888) ~= 5.1 ms       t_floor(1280x1024, RGB565) ~= 3.3 ms
slope @ 500 MHz ~= 0.050 ms/instr            slope @ 250 MHz ~= 0.10 ms/instr
```

This model reproduces the field behaviour (fast holds 60 fps at stock clock, full needs the
overclock), which is the sanity check on it. Treat its outputs as GPU-time budgets, not fps — a real
frame also carries the per-frame texture upload and vsync.

---

## 2. Where the cost lives (stock shaders, 1280x1024 / 500 MHz, RGBA8888)

Full shader: 134 FS instructions, ~11.1 ms/frame. Fast shader: 89 FS instructions, ~8.7 ms/frame.
Per-effect cost, from ablation (removing exactly one effect at a time):

| Effect | FS instr | ms | Notes |
|---|---|---|---|
| **Shadow mask** (`fastmask()`) | ~50 | **2.54** | The single most expensive effect. Present in the fast shader too. |
| Noise (ALU hash) | ~26 | 1.36 | Time-dependent; inherent. A noise *texture* saves only ~0.25 ms (a fetch ≈ the hash it replaces). |
| Desaturation + vignette | ~17 | 0.86 | Pure function of screen position. |
| Warp (`Warp()`) | ~16 | 0.83 | Quadratic in screen position; feeds the game-texture fetch coordinate. |
| Overlay sample (CRT shape/vignette texel) | ~12 | 0.74 | One texture fetch. |
| Framebuffer store floor | — | ~5.1 | Memory-bound; not ALU. |

The decomposition is self-consistent: the per-effect deltas sum to ~6.3 ms against a full-minus-
minimal difference of 6.0 ms. The important insight is that the mask alone costs more than noise and
desaturation/vignette combined — and it is exactly the kind of work that bakes cleanly.

---

## 3. What has been done: the shadow-mask bake

**The general principle.** Any per-pixel work that depends only on screen position and the
(rarely changing) configuration can be computed once, at config-change time, and delivered through a
texture fetch the shader either already makes or can afford to add. Only work that depends on the
frame's content (luminance) or time (noise) must stay per-pixel.

**The shadow mask fits this perfectly.** `fastmask()` produced a fixed `{0,1}` pattern in screen
pixels via a `floor` + three `fract`s + four `step`s + two `mix`es — ~50 instructions, roughly a
dozen of them `ftoi`/`itof` conversions that VC4 has no native op for. The pattern depends only on
the output-pixel position and `mask_size`.

**The change** (`src/main/sdl2/shadow_mask_bake.hpp`, `gl_backend.hpp`, `rendersurface.cpp`, both
fragment shaders):

- The `{0,1}` mask weight `w` is baked, per output pixel, into an 8-bit `GL_LUMINANCE` texture on a
  dedicated sampler unit, sized to the output rectangle, `GL_NEAREST` / `CLAMP_TO_EDGE` so it maps
  1:1 to screen pixels. The CPU bake is pure integer arithmetic (`shadow_mask_bake.hpp`), proven
  equal to the shader's float `fastmask()` for the whole supported `mask_size` range.
- `fastmask()` becomes a single `texture2D()` fetch followed by `mix(baseOff, baseOn, w)`.
- The dim/boost (`baseOff`/`baseOn`) stay **uniforms**, and the luminance wash-out
  `pCol *= mix(mask, vec3(1.0), lum)` stays **in the shader**. This is what preserves the shader
  mask's defining property — it can over-saturate a bright pixel to white, which a plain multiply
  overlay cannot — so the result is pixel-identical to the old shader for the shader-mask mode.
- The bake re-runs only when its inputs change: the output resolution (a video restart) or the live
  `mask_size` (cycled from the in-game menu, with no restart). Dim/boost/on-off are uniform-only and
  never trigger a re-bake.

**Why it maps well to VC4.** It removes the `floor`/`fract`/`step` ladder — the work VC4 is worst at
— and replaces it with a fetch whose latency is hidden by the existing thread switch. It also
removes a latent portability defect (§5).

**Measured result** (Raspberry Pi 2, Cortex-A7, 500 MHz V3D, 1280x1024, RGBA8888):

| Shader | integer-convert ops (`ftoi`+`itof`) | ms/frame | change |
|---|---|---|---|
| Full: stock → baked | 18 → 8 | 10.997 → 9.031 | **−1.97 ms (−17.9%)** |
| Fast: stock → baked | 14 → 4 | 8.290 → 7.228 | **−1.06 ms (−12.8%)** |

The output is bit-exact for the shader-mask mode (verified on both x86 and ARM against the shader's
float reference), so there is no fidelity cost. In practice this recovered enough headroom to run
the full look and back the CPU clock off while still holding 60 fps.

---

## 4. Future improvement areas (ranked by return vs. fidelity risk)

These are analysed but **not yet implemented**. They compound with the mask bake already in place.

1. **Bake vignette + desaturation (incl. edge desaturation) + CRT shape into the overlay.** These
   are all functions of screen position only. Because the shader's multiplicative chain commutes,
   the whole position-only part reduces to a per-pixel affine `c*R + G` plus the shape multiply,
   which the CPU can bake into the overlay texture (e.g. `R` = combined multiplier, `G` = pre-scaled
   offset, plus the mask weight). The CPU can bake at the *warped* coordinate, so the result matches
   the shader exactly rather than approximately. Projected saving with noise kept: ~−23%; without
   noise: ~−30%. Store the small offset channel pre-scaled (e.g. ×4) and undo with one uniform to
   avoid 8-bit banding in dark areas. This is the single largest remaining shader win. It requires
   the overlay to be (re)built whenever any of mask/vignette/desaturation/shape settings change
   (today it is skipped when CRT shape and vignette are both off).

2. **RGB565 backbuffer.** Roughly half of the frame is the memory-bound framebuffer store. A 16-bit
   backbuffer halves that store (5.2 → 2.6 MB/frame) and the scanout traffic, saving a **flat
   ~1.8 ms on every frame regardless of shader**, and relieving the shared bus for the CPU-side
   Blargg/sprite work. VC4 has a hardware-dithered 565 tile-store mode (used when dithering is
   enabled, which is the GLES2 default), so banding risk is low — vignette gradients are where to
   check. The risk is on the windowing side: the KMSDRM/EGL path must actually create a 16-bit
   surface; verify the granted config at runtime rather than assuming it.

3. **Move the warp into the vertex shader over a tessellated grid.** Replace the single fullscreen
   triangle with a modest grid (e.g. 16x8); the vertex shader applies `Warp()` per vertex and the
   fragment shader samples at the interpolated coordinate. This deletes ~16 ALU instructions and
   costs ~4 back for a second (unwarped) varying the overlay needs — net ~−12 instructions
   (~0.6 ms at 500 MHz, ~1.2 ms at stock clock). Interpolation error of the quadratic warp over such
   a grid is well under a pixel — invisible.

4. **Noise.** Inherently ~26 instructions / ~1.4 ms with the current hash, and a noise *texture*
   does not help on VC4 (a fetch costs about what the hash costs). Options: keep it (the baked bundle
   affords it even at stock clock), simplify the hash (dropping `fastSin` saves ~10 instructions, at
   some pattern-quality risk), or expose it as the one effect a user turns off for headroom.

5. **Output resolution** is the largest lever of all — cost scales with pixel count. 1080p full does
   not reach 60 fps even at 500 MHz; 1280x1024 is the practical ceiling for the full look on VC4.

---

## 5. Portability note (not a Pi 2 performance issue)

The original `fastmask()` did `floor(maskpos)` on a value of screen-pixel magnitude (up to ~1920)
declared `mediump`. GLSL ES `mediump` guarantees only a 10-bit mantissa, so on any GPU/driver that
*honours* the qualifier (Pi 4/5 V3D, ARM Mali, Adreno, ANGLE-on-desktop) the mask pattern would
break in the right/lower part of the screen. VC4 happens to ignore the qualifier (fp32), which is
why it worked there. **The mask bake removes this exposure entirely** for the mask path, since the
pattern is now a texel fetch at the unwarped screen UV rather than a large-magnitude `floor`. If any
new effect reintroduces large-coordinate math in the fragment shader, declare the relevant varying
`highp` — it is free on VC4 and correct elsewhere.

---

## 6. Anti-patterns on VideoCore IV (measured or structural — do not do these)

- **Precision qualifiers for speed.** No effect; the QPU is fp32 and the compiler ignores them.
- **A 1x1 white texture to "skip" an unused overlay.** Measured *slower* — the fetch is the cost,
  not the bytes; the sampler is read unconditionally either way.
- **A noise/LUT texture to replace the hash.** +1 fetch ≈ the ALU removed (~−0.25 ms only).
- **Pushing per-pixel products into varyings on the existing 3-vertex triangle.** Each varying
  component costs ~2 fragment instructions to interpolate, so it only pays when it replaces ≥3 ALU
  ops per component (the vertex-mesh warp qualifies; a single `coord * uniform` does not).
- **Any intermediate render target / second pass.** Each one adds a ~5 ms-class store+load at this
  resolution. The current path is single-pass; keep it that way.
- **RGB5_A1 vs RGBA8 game texture.** No measurable GPU difference; the source is small and
  cache-resident. Choose whichever the CPU path prefers.

---

## 7. Reference material (public)

- Broadcom VideoCore IV 3D Architecture Reference Guide (QPU = 12 × 16-way SIMD over 4 clocks;
  add/mul dual issue; SFU for recip/rsqrt/exp/log): https://docs.broadcom.com/doc/12358545 ;
  community summary: https://github.com/hermanhermitage/videocoreiv
- VideoCore IV performance recommendations (dual-issue ALU, two hardware threads hide texture
  latency, prefer conditional moves over branches):
  https://github.com/Yours3lf/rpi-vk-driver/wiki/Broadcom-Videocore-IV-Performance-Recommendations
- Mesa `vc4` driver author on multithreaded fragment shaders (texture latency ~20 QPU instructions;
  thread switch around fetches): https://anholt.livejournal.com/49712.html
- Mesa handling of precision qualifiers (ignored, fp32): https://docs.mesa3d.org/drivers/vc4.html
- VC4 QPU pipeline / instruction costs: https://jbush001.github.io/2016/03/02/videocore-qpu-pipeline.html ;
  https://github.com/doe300/VC4CL/wiki/Performance
- VC4 tile-store colour formats incl. dithered 565 (kernel `drivers/gpu/drm/vc4/vc4_packet.h`):
  https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/vc4/vc4_packet.h
- SDL2 KMSDRM and 16-bit GBM formats (RGB565 configs need care on KMSDRM — verify at runtime):
  https://github.com/libsdl-org/SDL/issues/2491
