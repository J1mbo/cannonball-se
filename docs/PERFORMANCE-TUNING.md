# Performance Tuning

**The short version: CannonBall-SE runs at 60fps on every quad-core Raspberry Pi, and at
30fps on the single-core boards.** On the slower quad-core boards you may need to choose the
fast shader, or raise the GPU clock, to get there. This document explains which setting to
change, and in what order.

If you only read one thing: run

```bash
build/cannonball-se -perftest
```

which measures the maximum frame rate your own board sustains with your current settings. It
is a far better guide than any table, because it accounts for your screen resolution, your
clocks and your cooling.

---

## 1. What to expect, by board

Boards are grouped by the silicon that matters. Within a group, behaviour is essentially the
same — a Pi 3, a Pi 2 v1.2 and a Pi Zero 2 W are all the same Cortex-A53 class part and
perform alike.

Figures assume a 1280x1024 screen and hi-res mode enabled. Higher resolutions cost more; the
GPU work scales with output pixels, so 1920x1080 is roughly 1.6x the shader cost of
1280x1024.

| Tier | Boards | 30fps | 60fps |
|---|---|---|---|
| **Single-core** | Pi 1, Pi Zero (W), 2014 Pi B+ | Yes, with the clock bump below. The game applies reduced settings automatically. | Not supported |
| **Quad-core, 32-bit** | Pi 2 v1.1 (ARMv7 Cortex-A7) | Yes, stock clocks | Yes, with the fast shader and GPU at 450MHz. The full shader needs roughly 1.1GHz CPU / 500MHz GPU. |
| **Quad-core, 64-bit** | Pi 2 v1.2, Pi 3 (all models), Pi Zero 2 W | Yes, stock clocks | Yes, with the fast shader. The full shader needs the GPU at 400MHz. A Pi 2 v1.2 may also want its CPU at 1GHz. |
| **Quad-core, fast** | Pi 4, Pi 5 | Yes, stock clocks | Yes, stock clocks, either shader |
| **PC** | Any x86/x64 machine capable of running Windows 11, or Ubuntu | Yes | Yes |

On any multi-core Pi, fit a CPU heatsink. A board that thermally throttles will miss frames
regardless of settings.

> These figures are reported from the maintainer's own boards. Version 1.42 cut GPU shader
> cost by 13–18% (see section 5), so they are, if anything, now conservative — some boards
> that previously needed an overclock for the full shader may no longer need one. Use
> `-perftest` on your own hardware rather than treating the table as a promise.

### Single-core boards

On Pi 1, Pi Zero (W) and the 2014 Pi B+ the game detects the board at launch and applies
reduced settings automatically: original resolution, 30fps, full shader, NTSC filter
disabled, and a 16ms sound callback. You do not need to configure anything.

These boards do need their clocks raised to reach 30fps. Use HDMI audio.

---

## 2. Which lever to pull

First work out whether you are CPU-limited or GPU-limited, then pull the levers on that side.
Turn on the frame-rate counter (`video.fps_counter`, or *Menu → Settings → Video*) and change
one thing at a time.

**A quick test:** set `shader_mode` to 0 (pass-through). If the frame rate jumps, you are
GPU-limited. If it barely moves, you are CPU-limited.

### If you are GPU-limited

In order of effect:

| Change | Setting | Effect |
|---|---|---|
| Full shader → fast shader | `video.shader_mode` 2 → 1 | Largest single GPU saving. Drops the analogue noise; keeps the mask, curvature and vignette. |
| Shader → off | `video.shader_mode` 1 → 0 | Removes the CRT look entirely. Use only as a diagnostic, or on the weakest hardware. |
| Shader mask → overlay mask | `video.shadow_mask` 2 → 1 | Cheaper mask. The overlay cannot over-saturate bright pixels to white, so highlights look slightly flatter. |
| Turn off analogue noise | `video.noise` → 0 | ~1.4ms/frame on a Pi 2 at 1280x1024. The noise is time-varying, so it cannot be pre-computed. |
| Turn off curvature/bezel | `video.crt_shape` → 0 | Removes the warp and the overlay fetch, ~1.5ms/frame combined on a Pi 2. |
| Lower the screen resolution | your display settings | GPU cost is directly proportional to output pixels. |
| Raise the GPU clock | `/boot/firmware/config.txt` | See section 3. |

### If you are CPU-limited

| Change | Setting | Effect |
|---|---|---|
| Turn off hi-res mode | `video.hires` 1 → 0 | The biggest CPU lever by a wide margin — roughly quarters the pixel work in the game renderer. Reverts to the original arcade resolution. |
| Turn off hi-res sprites | `video.hiresprites` 1 → 0 | Only relevant if you had turned it on; it is off by default. |
| Turn off the NTSC filter | `video.blargg` → 0 | Removes the CPU-side composite filter. Costs you the colour bleed and dot crawl. |
| Use HDMI audio | your display/audio setup | HDMI audio is handled by the GPU and is nearly free. Analogue and USB audio are processed on the CPU. |
| Raise the CPU clock | `/boot/firmware/config.txt` | See section 3. |
| Give the game more threads | `-t 1` … `-t 4` | The game detects core count automatically; this only overrides it. Rarely useful. |

---

## 3. Clock recipes

These go in `/boot/firmware/config.txt` (on older Raspberry Pi OS releases, `/boot/config.txt`).
Reboot to apply.

**Overclocking is at your own risk.** It can shorten hardware life and cause instability or
data loss. CannonBall-SE hooks the hardware watchdog on Raspberry Pi boards, so a hang caused
by an aggressive overclock will reboot the machine rather than leaving it stuck — but that is
a safety net, not a licence. Raise clocks in small steps and test for stability. Every
multi-core Pi being overclocked needs a heatsink.

**Single-core boards (Pi 1, Pi Zero, 2014 B+) — needed to reach 30fps:**

```ini
arm_freq=1000
gpu_freq=400
core_freq=350
```

The Pi Zero (W) already runs its CPU at 1GHz, so only the GPU and core settings are needed.

**Pi 2 v1.2 / Pi 3 / Pi Zero 2 W — to run the full shader at 60fps:**

```ini
gpu_freq=400
```

**Pi 2 v1.1 — 60fps with the fast shader:**

```ini
gpu_freq=450
```

**Pi 2 v1.1 — full shader at close to 60fps:**

```ini
arm_freq=1100
gpu_freq=500
```

Pi 4 and Pi 5 need no changes.

---

## 4. Every performance-relevant setting

All of these live in `res/config.xml` and most can also be changed from *Menu → Settings*.
See [CONFIGURATION.md](CONFIGURATION.md) for the full reference including the non-performance
settings.

| Setting | Values | Side | Cost |
|---|---|---|---|
| `video.hires` | 0, 1 | CPU | High. Hi-res roughly quadruples game-renderer pixel work. |
| `video.hiresprites` | 0, 1 | CPU | Moderate, sprite renderer only. No effect unless `hires` is 1. |
| `video.blargg` | 0-3 | CPU | Moderate. Auto-disabled on single-core boards. |
| `video.shader_mode` | 0, 1, 2 | GPU | High. Measured on a Pi 2 at 1280x1024: full 9.03ms/frame, fast 7.23ms/frame. |
| `video.shadow_mask` | 0, 1, 2 | GPU | Low since 1.42 — the pattern is now baked into a texture at startup rather than computed per pixel. |
| `video.noise` | 0–20 | GPU | ~1.4ms/frame on a Pi 2 at 1280x1024 when enabled. |
| `video.crt_shape` | 0, 1 | GPU | ~1.5ms/frame on a Pi 2, combining the warp maths and the overlay fetch. |
| `video.mask_size` | 3–6 | none | Visual only; changing it re-bakes the mask texture once. |
| `video.fps` | 0, 1 | — | Starting frame rate. The game auto-switches upward if the hardware sustains it. |
| `sound.callback_rate` | 0 (8ms), 1 (16ms) | CPU | 16ms halves the callback frequency. Use it if you get audio dropouts. |
| `sound.rt_priority` | 0, 1 | CPU | Off by default. Setting 1 gives the audio threads real-time priority, which can *cost* frame rate by pre-empting the render threads. Prefer `callback_rate=1`. |

Command-line flags that affect performance:

| Flag | Effect |
|---|---|
| `-perftest` | Measure the maximum sustainable frame rate, then quit |
| `-30` / `-60` | Lock the frame rate, disabling auto-selection |
| `-t 1` … `-t 4` | Override detected thread count |
| `-1` | Single-core mode (game in one thread, plus sound) |
| `-x` | Disable single-core board auto-detection |

---

## 5. Why the GPU is usually the limit

On Raspberry Pi boards up to the Pi 3, the CRT shader is normally what caps the frame rate.
The game frame itself is small — 320x224, upscaled through the NTSC filter — but the shader
runs once per *output* pixel: 1.31 megapixels at 1280x1024, 2.07 at 1920x1080.

Measured per-effect cost on a Pi 2 (VideoCore IV at 500MHz, 1280x1024):

| Effect | Cost per frame |
|---|---|
| Shadow mask, before the 1.42 bake | 2.54ms |
| Analogue noise | 1.36ms |
| Desaturation and vignette | 0.86ms |
| Screen warp | 0.83ms |
| Bezel/overlay sample | 0.74ms |
| Framebuffer store | ~5.1ms |

The last row is the important one. Roughly 46% of the frame is not shader arithmetic at all —
it is writing the finished image to memory, which is limited by the shared memory bus. **No
shader setting can reduce that floor.** It is why turning effects off gives diminishing
returns, and why a board either comfortably clears 60fps or does not.

Version 1.42 bakes the shadow-mask pattern into a texture at startup instead of computing it
per pixel, cutting 1.97ms from the full shader (−17.9%) and 1.06ms from the fast shader
(−12.8%) with bit-identical output.

The full engineering analysis, including the cost model used to predict other clocks and
resolutions, is in `tools/GLSL_SHADER_PERFORMANCE_ANALYSIS.md`.

---

## 6. Audio dropouts

Stuttering audio is usually a scheduling problem rather than a performance one. See
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
