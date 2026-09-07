# Sprite renderer work: what it was for, and where things are

This is the entry point for `tools/sprite_bench/`. It condenses the original
design brief (`PROBLEM_STATEMENT.md`) and the pipeline audit (`PIPELINE.md`),
both since removed as superseded by the shipped code and by
`analysis/SPRITE_R1_LLD.md`; it also carries forward the one piece of
`COMPARISON.md` (also removed) that was not duplicated anywhere else: the
upstream algorithm semantics. For the measured numbers behind every design
decision below, see `analysis/PERFORMANCE_MEASUREMENTS.md`.

## What the sprite work was for

The task was not to optimise `hwsprites.cpp` incrementally. It was to go
back to the upstream (MAME-derived) renderer, treat it as the functional
specification, and work out what the routine should look like to run fast
on Cortex-A7 and Cortex-A53 given the memory system those machines actually
have and the other work running concurrently (game logic, NTSC filtering,
GPU compositing, all sharing one contended L2 and memory bus).

**The contract**, boiled down to one paragraph: **a uniform
nearest-neighbour magnifier/minifier from 4bpp ROM source to 16-bit
destination, with two transparent codes, a shadow rule that is a
read-modify-write OR of bit 12 rather than a colour write, an optional
horizontal mirror, and edge clipping. Everything else is implementation.**
Input per frame is up to 122 sprite records (source address, pitch, height,
zoom, screen position, mirror flag, palette base, shadow-enable), read in
list order with later sprites overwriting earlier ones. Source format is
4 bits/pixel, 8 pixels/word, nibble 0 transparent, nibble 15 transparent
and (in the second-to-last word position) a row terminator, nibble 10 a
shadow pixel when the sprite's shadow flag is set.

### Upstream semantics (what the reference algorithm actually does)

`hwsprites::render(priority)` in the upstream (djyt/cannonball) tree loops
over the fixed 128-entry sprite table. For each entry it terminates the
whole loop on the list-end bit; filters by a 2-bit priority field against
the caller's argument (the caller always passes 8, so only priority-3
sprites are ever drawn — true in this fork too, one call site, fixed
priority); decodes hide flag, height, bank, top scanline, ROM address,
pitch, x-position, shadow flag, vertical/horizontal zoom straight out of
the record; clamps `vzoom`/`hzoom` to a floor of `0x40` (an artifact of
unconfirmed original hardware behaviour — this fork drops the clamp
entirely, since it treats horizontal and vertical zoom as always equal and
the shipped zoom tables never reach a value low enough to need it); adjusts
for widescreen and hi-res; then runs an inner y-loop, y-clipped to the
screen, with separate near-identical x-loops for the flipped and
non-flipped case. Each x-loop reads one 32-bit ROM word per iteration,
unpacks 8 nibbles, and for each nibble runs a fractional-zoom DDA
(`while (xacc < 0x200) { ...; x += xdelta; xacc += hzoom; }`) that can
stretch one source nibble into zero, one, or several output pixels.
`draw_pixel()` tests the clip window, rejects nibble 0 (transparent) and
15 (special), then either ORs in the shadow flag or writes
`pPixel[x] = pix | color`. Row end applies the same fractional accumulator
vertically: `yacc += vzoom; addr += pitch * (yacc >> 9); yacc &= 0x1ff;` —
this skips ROM rows when zoomed down and repeats them when zoomed up. Rough
per-pixel cost: a nibble extract, a 2-4 term clip/transparency test, a
branch, and usually either nothing or one 16-bit store.

This is still an accurate description of the reference semantics the
shipped renderer (`SpriteCtx`/`draw_sprite_rows_t`, see
`analysis/SPRITE_R1_LLD.md`) is required to reproduce bit-for-bit, and of
what the `render_px` oracle in `hwsprites.cpp` implements.

### The closed-form scaling law

The upstream accumulator (`xacc`/`yacc` start at 0, add `zoom` per output
step, subtract `0x200` on carry) is equivalent to a closed form: destination
offset at which source pixel `i` starts, `N(i) = ceil(i * 512 / zoom)`;
source index sampled at destination offset `d`, `src(d) = (d * zoom) >> 9`;
run length of source pixel `i`, `r(i) = N(i+1) - N(i)`. Both axes use the
same `zoom` value (the engine provably writes the same value to both
fields), so **one schedule serves both axes**, and the horizontal schedule
is identical for every row of a sprite — it can be computed once per
sprite rather than re-derived per pixel per row. This closed form is
**proven exact** by brute force over every integer zoom value in both zoom
tables, tens of millions of samples, no mismatches — see
`analysis/SCHEDULE_VERIFICATION.md`. Two consequences: the run-length
sequence's period (up to 949 in lo-res) is longer than any real sprite, so
a small per-zoom schedule table is pointless — the closed form is evaluated
directly instead; and **a run length of zero is legitimate whenever
`zoom > 0x200`** — a source pixel can contribute no output pixels at all,
and any fast path must treat that as a normal case, not an error.

### Zoom regimes

Extracted from `src/main/engine/data/ozoom_lookup.hpp`:

| Configuration | Table | Halved by renderer? | Effective zoom range | Regime |
|---|---|---|---|---|
| lo-res | `ZOOM_LOOKUP` | no | 0x100 .. 0x3F0 | mixed: magnify / unity / shrink |
| **hi-res, `hiresprites` off (shipping default)** | `ZOOM_LOOKUP` | yes | **0x80 .. 0x1F8** | **magnification only, 1.01x to 4x. Never shrinks** |
| hi-res, `hiresprites` on | `ZOOM_LOOKUP_HIRES` | yes | 0x80 .. 0x3F0 | mixed, most rows magnify |

In the default shipping configuration every sprite is magnified — one
decoded source pixel produces an axis-aligned rectangle averaging roughly
2x2 (measured mean run length 2.27 per axis in hi-res default, so ~5.1
output pixels per source pixel). This is the single most important fact
for the design: the routine is not a per-pixel plotter, it is a filler of
small constant-colour rectangles, and a design should be judged first on
memory operations and destination cache lines touched per source pixel,
not on arithmetic. This is what shaped the LINEBUF (decode source row
once, replay to `r_v` destination rows) structure that shipped — see
`analysis/SPRITE_R1_LLD.md`.

### The F7 hotkey edge case

For `zoom >= 0x402`, two adjacent runs can both be zero, and the excess of
a one-pixel overrunning store then lands in the *following* span rather
than being safely absorbed (the overrun-safety inequality that normally
guarantees this, `r_i + r_(i+1) >= MAXRUN`, does not hold at that extreme).
This is reachable: 32 raw `ZOOM_LOOKUP_HIRES` values exceed `0x402`, used
unhalved whenever `hiresprites` is on while `hires` is off. That
combination is guarded in the menu (which refuses it and clears the flag
when hi-res is turned off) but **not** by the F7 hotkey, which toggles
`hiresprites` unconditionally at runtime, nor by the config file, which
reads the two settings independently. The shipped kernel must therefore
skip the store entirely when a run's final pixel has run length zero,
rather than relying on the inequality alone — this is exactly what
`draw_sprite_rows_t` does (see `analysis/SPRITE_R1_LLD.md` sec. 3.5/4.1).
The F7 hotkey itself remains unguarded (a separate, still-open item, not a
renderer defect).

## Threading model (from the former `PIPELINE.md`)

`hwsprites::render()` is called once, synchronously, from
`Video::prepare_frame()`. Which thread that runs on depends on `-t`:

| `-t` | `using_threading` | `render_threads` | `prepare_threads` | Where `hwsprites::render()` runs | Where NTSC/RGB conversion runs |
|---|---|---|---|---|---|
| 1 | false | — | — | main thread, sequential | main thread, right after |
| 2 | true | 1 | false | main thread | 1 dedicated render thread (whole frame) |
| 3 | true | 2 | false | main thread | 2 render threads (top/bottom half) |
| 4 (Linux only) | true | 2 | true | dedicated `prepare_thread` | 2 render threads |

**On Windows, `-t` is capped to 3** (input must stay on the main thread), so
a dedicated prepare thread — and therefore any thread other than main doing
sprite/tile/road drawing — never exists on Windows; only Linux can reach
`-t 4`. This is why every in-situ sprite measurement in
`analysis/PERFORMANCE_MEASUREMENTS.md` was taken on Linux ARM boards at
`-t 4`, matching the deploy configuration that actually contends for L2.

Sprites are drawn directly into the shared S16 index buffer, after road and
tile layers and before text, with no intermediate layer — the buffer
`hwsprites::render()` writes into is the same one `RenderSurface::draw_frame()`
later reads for NTSC/RGB conversion on a different thread.

### Open questions (still unresolved, carried forward from `PIPELINE.md`)

- Does the single fixed-priority (`8`) call to `sprite_layer->render()`
  silently drop sprites whose ROM-sourced `shadow` field (two call sites
  read priority bits from ROM data whose value cannot be bounded
  statically) carries a priority tier other than the one that field always
  produces for every hardcoded engine assignment? If the original
  hardware/MAME design used multiple priority-tier render passes
  interleaved with road drawing and this fork collapsed them to one pass,
  is that collapse verified safe for all ROM-authored level-object data, or
  only for the tracks tested?
- The per-frame sprite-ROM *touched* working set (as opposed to the ~2.25MB
  resident `sprites[]`/`sprites_flipped[]`/`sprites_shadowinfo[]`, if those
  side tables still exist in whatever kernel is active) is inherently
  data-dependent and was never derived from static code alone; the in-situ
  measurements in `analysis/PERFORMANCE_MEASUREMENTS.md` are the closest
  thing to an answer.

## Test harness

The standalone driver lives in `tools/sprite_bench/test/` — a CMake project
separate from the game build, with reference (`_ref`) twins of the kernel
kept in the tree, a pixel-serial oracle (`render_px`), and checked-in
`golden/` fixtures. This is what every optimisation in this directory was
gated against for bit-exactness (see `analysis/SPRITE_IMPLEMENTATION_SPEC.md`
S0 for the harness design, and `tools/road_tiles/analysis/GOLDEN_CHECKSUMS.md`
for the whole-game checksum gate that runs alongside it).

## Document index

| Document | What it is |
|---|---|
| `analysis/SPRITE_IMPLEMENTATION_SPEC.md` | The formal S0-S4 buildable spec for the sprite kernel rewrite (LINEBUF, glow, shadow bitmap): files, macros, invariants, tests. The authoritative historical build record — heavily cited from `src/main/hwvideo/`. |
| `analysis/SPRITE_R1_LLD.md` | The low-level design for R1 (de-glue) and R2 (portable NEON) — the best available description of the *current* shipped kernel's actual architecture (`SpriteCtx`, the `<FLIP,MAXRUN,SHRINK>` templates, decode/replay). Read this first to understand how the kernel works today. |
| `analysis/SPRITE_REPLAN_FABLE.md` | The route decision behind the hybrid A7/A53 (LINEBUF, accelerated) / ARMv6 (legacy kernel, **planned but not implemented**) architecture, the glue-cost diagnosis that led to R1, and the NEON portability mechanism (`hwarch.hpp`'s two-tier detection) shared by A7 and A53. |
| `analysis/DESIGN_GLOW.md` | Derives and proves the minimal, cheap reproduction of the System 16 "glowy edge" shadow quirk (one deferred bit-12 clear per visible colour span). Still-accurate, load-bearing reasoning behind `glow_push`/`glow_flush`. |
| `analysis/DESIGN_FABLE.md` | The original clean-sheet LINEBUF design proposal, including a still-relevant "alternatives considered and rejected" section (occlusion bitmap, masked-blend replay, per-sprite NEON shuffle tables). |
| `analysis/SCHEDULE_VERIFICATION.md` | The brute-force proof of the closed-form scaling law referenced above, plus full run-length distributions by zoom regime. |
| `analysis/MICROARCH_A7_A53.md` | Sourced Cortex-A7/Cortex-A53 microarchitecture reference facts (register counts, NEON table-lookup capability, memory-op timing) — general ARM optimisation reference, not sprite-specific. |
| `analysis/PERFORMANCE_MEASUREMENTS.md` | Every hardware measurement taken during the sprite rewrite and the road/tiles work it redirected effort towards, in chronological order: the idle-bench win, the in-situ regression, why NEON alone didn't fix it, the root-cause attribution, and the fix that did work. |

Road/tile renderer documentation lives in `tools/road_tiles/analysis/`; see
`tools/road_tiles/analysis/ROAD_TILE_RENDERER.md` for that story.
