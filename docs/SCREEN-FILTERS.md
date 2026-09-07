# Screen Filters

Modern LCDs render OutRun's low-res graphics as oversized, razor-sharp pixels — which makes the
game feel clinical. CannonBall-SE recreates the vintage feel with a three-stage CPU+GPU pipeline.

## 1. NTSC filtering (CPU)

The game's output is put through a SIMD-accelerated port of Blargg's NTSC filter, which softens
the image and corrects the colours. In *Composite* mode it also adds artifacts such as shimmer
and dot crawl. Those were not present on the RGB arcade monitors, but the reduction in fidelity
improves the visuals for many players.

Controlled by `video.blargg`: `0` off, `1` composite, `2` s-video, `3` RGB. Composite shows the
most artifacts; RGB is the cleanest, and is closest to what the arcade cabinet's own RGB monitor
would have shown. All three cost the same. Tuning is under `video.saturation`, `contrast`,
`brightness`, `sharpness`, `resolution`, `gamma` and `hue`.

## 2. CRT shader (GPU)

The filtered image is then processed on the GPU with a shader that adds barrel distortion and
vignette to simulate the curved glass of an arcade screen, along with light analogue noise and a
configurable shadow-mask grille effect visible in mid-tones.

Two shaders ship:

| | Full | Fast |
|---|---|---|
| Barrel distortion, vignette, desaturation | yes | yes |
| Shadow mask | yes | yes |
| Analogue noise | yes | no |
| Cost on a Pi 2 at 1280x1024 | 9.03 ms/frame | 7.23 ms/frame |

Select with `video.shader_mode`: `0` off, `1` fast, `2` full.

## 3. Bezel and aperture overlay (GPU)

Finally an overlay simulates the CRT bezel shape by fading the border softly, further enhancing
the curved-glass effect. It follows the `warpX`/`warpY` curvature settings automatically.

Controlled by `video.crt_shape`.

---

By using multi-threading, SIMD and the GPU in parallel, CannonBall-SE hits 60fps on a 32-bit Pi 2
despite the weight of this processing.

Press **F8** in game to toggle all video processing on and off, which is the quickest way to see
what the pipeline is doing.

## See also

- [CONFIGURATION.md](CONFIGURATION.md) — every setting, with defaults and ranges
- [PERFORMANCE-TUNING.md](PERFORMANCE-TUNING.md) — what each effect costs, and what to turn down
- [NTSC-FILTER-BACKGROUND.md](NTSC-FILTER-BACKGROUND.md) — why composite video looks the way it does
