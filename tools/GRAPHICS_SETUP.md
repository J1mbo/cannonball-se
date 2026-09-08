# CannonBall-SE Graphics Setup and Palette-Table Mechanics — Read-Only Code Audit

Scope: static-code audit only, no files under `src/` were modified. All line
numbers refer to the tree at commit `086591c` (see repo git log) plus
whatever uncommitted `snes_ntsc.cpp/.h` edits are in the working tree; the
palette/render-pipeline files discussed here (`video.cpp`, `renderbase.*`,
`rendersurface.*`, `hwvideo/*`, `opalette.cpp`) were unmodified at audit
time. A second, concurrent agent is wiring a frame-dump hook into
`rendersurface.cpp`/`main.cpp`; any such edits observed in those two files
were ignored in favour of the mechanics described below.

## 1. Startup sequence and table lifetime

`Video::init()` (`src/main/video.cpp:58-92`) allocates the two 640x448 (in
hires) `uint16_t` "S16 pixel" buffers (`pixel_buffers[0/1]`, 64-byte
aligned, offset by `alignment=64` elements — `video.hpp:87` — "so the
sprite renderer can avoid an `x>0` bounds check", comment at
`video.cpp:64`), then calls `set_video_mode()` → `renderer->init_palette()`
(currently a no-op stub, `renderbase.cpp:121-123`) → `renderer->init()`.

`RenderSurface::init()` (`rendersurface.cpp:52-104`) does, in order:
1. Capture Blargg setup fields from `config.video.*` (saturation, contrast,
   brightness, sharpness, resolution, gamma, hue) — lines 61-67.
2. `last_blargg_config = get_blargg_config()` then `init_blargg_filter()`
   (lines 76-77) — this is what actually builds the 32 MiB kernel table,
   *before* SDL/GL is initialised.
3. `init_sdl()`, pixel-format capture, `init_overlay()` (CRT mask LUTs),
   `create_buffers()` (allocates the single working buffer `rgb_pixels`,
   `rendersurface.cpp:146-153`).

`RenderSurface::init_blargg_filter()` (`rendersurface.cpp:171-222`) is
gated by `if (blargg)` (i.e. `config.video.blargg != BLARGG_DISABLE`,
default 1/COMPOSITE — `config.cpp:235`). When enabled it: computes
`snes_src_width` from `src_width` (640 in hires) via
`SNES_NTSC_OUT_WIDTH_SIMD`/`SNES_NTSC_OUT_WIDTH`, selects one of
`snes_ntsc_composite/svideo/rgb` as `setup` (lines 196-204), resets
`phase = 0; phaseframe = 0`, copies the seven user sliders into `setup`
again (lines 210-217), then calls `snes_ntsc_init(ntsc, &setup)` (line 219)
— the single call that (re)builds the whole kernel table.

**Table build / OpenMP.** `snes_ntsc_init()` lives in
`src/main/sdl2/snes_ntsc.cpp:107-166`. Under `#ifdef _OPENMP` it wraps the
per-entry loop in `#pragma omp parallel` / `#pragma omp for schedule(static)`
(lines 118-146), giving each thread a private copy of the float/YIQ
working state (`init_t impl_local = impl;`) so the 65536 iterations are
independent; a non-OpenMP serial fallback (lines 151-166) is identical.
Each iteration decodes `entry` (0..0xFFFF) into 5-bit R/G/B plus a shadow
flag (`entry>>15`), maps them through the S16 DAC curve (`S16_rgbVals[64]`,
`snes_ntsc.cpp:61-68`), converts to YIQ, and calls `gen_kernel()` +
optionally `merge_kernel_fields()` + `correct_errors()` to fill the 128
`uint32_t` (512-byte) row `ntsc->table[entry]`
(`snes_ntsc.h:258-263`: `enum { snes_ntsc_entry_size = 128 };`
`enum { snes_ntsc_palette_size = 0x10000 };` `struct snes_ntsc_t { snes_ntsc_rgb_t table[snes_ntsc_palette_size][snes_ntsc_entry_size]; };`)
— 0x10000 × 128 × 4 bytes = 32 MiB exactly, matching the original brief.
`ntsc` is heap-allocated with plain `malloc` (`rendersurface.cpp:56`) and
freed with `free(ntsc)` in `disable()` (line 143) — its lifetime spans one
`init()`/`disable()` cycle of `RenderSurface`, effectively the whole run
unless a full video restart occurs (`config.videoRestartRequired`,
`main.cpp:604-611`).

**Settings-change re-entry.** `RenderSurface::draw_frame()`
(`rendersurface.cpp:1261-1357`) checks, once per frame and only on the
`fastpass!=1` (top-half or single-threaded) call, whether
`get_blargg_config()` (`rendersurface.cpp:1249-1258`, a sum of the seven
Blargg sliders plus the on/off mode) has changed since `last_blargg_config`.
If so it copies the new slider values into `setup`, takes `drawFrameMutex`,
and calls `init_blargg_filter()` again (lines 1327-1345) — a **full**
65536-entry rebuild, OpenMP-parallelised as above, done synchronously while
holding the same mutex that `swap_buffers()` (`rendersurface.cpp:105-110`)
also takes, so a slider change stalls buffer-swapping for the duration of
the rebuild. Nothing here does anything incremental — there is no
per-palette-entry regeneration path in the existing code; every rebuild is
"blow away all 65536 rows and start again".

## 2. Palette mechanics

**RAM layout.** Palette RAM is 8 KiB (`palette[]`, addressed 0x0000-0x1FFE,
big-endian 16-bit words), `S16_PALETTE_ENTRIES = 0x1000` (4096,
`globals.hpp:43`) 16-bit entries. `Video::write_pal8/16/32`
(`video.cpp:428-500`) mask the address into that range, byte-swap, memcpy
into `palette[]`, then call `refresh_palette(adr)` for each 16-bit word
touched (a 32-bit write calls it twice — `write_pal32`, lines 459-470).

`Video::refresh_palette()` (`video.cpp:585-604`) decodes one big-endian
S16 palette word `a` into three 5-bit channel values, each built from a
4-bit nibble plus a separate "brightness" flag bit that is *per channel*
(bit 12 for red, 13 for green, 14 for blue):

```cpp
uint8_t r = (((a >> 0) & 0x000Fu) << 1) | ((a >> 12) & 1u);   // bits 0-3, flag bit 12
uint8_t g = (((a >> 4) & 0x000Fu) << 1) | ((a >> 13) & 1u);   // bits 4-7, flag bit 13
uint8_t b = (((a >> 8) & 0x000Fu) << 1) | ((a >> 14) & 1u);   // bits 8-11, flag bit 14
renderer->convert_palette(palAddr, r, g, b);
```
i.e. each of r/g/b is a genuine independent 5-bit value (0..31), so all
32×32×32 = 32768 standard RGB combinations are representable by *some*
palette word (there is no hardware restriction limiting colours to fewer
combinations — 4-bit-plus-shared-flag would have; this design does not
share the flag).

**Immediate, per-entry update — no dirty tracking, no per-frame rebuild.**
`RenderBase::convert_palette()` (`renderbase.cpp:95-113`) is called
synchronously, in-line, from the write path above (i.e. from whatever
thread executes engine code — `prepare_thread`/main thread — not from a
render thread). It updates **exactly two** entries of two parallel 8192-
entry tables (`S16_PALETTE_ENTRIES*2`, `renderbase.hpp:43-45`):
```cpp
void RenderBase::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;                                    // word index, 0..4095
    s16_rgb555[adr] = (S16_rgbVal_5bit[r1] << 11) | (S16_rgbVal_5bit[g1] << 6)
                     | (S16_rgbVal_5bit[b1] << 1) | RGB1555_ALPHA;
    rgb_blargg[adr]  = CURRENT_RGB_BLARRG_STD();          // (r1<<10)|(g1<<5)|b1 | 0x0000

    s16_rgb555[adr + S16_PALETTE_ENTRIES] = (S16_shadowVal_5bit[r1] << 1) | ... ;
    rgb_blargg[adr + S16_PALETTE_ENTRIES] = CURRENT_RGB_BLARRG_SHADOW();  // ... | 0x8000
}
```
There is no dirty flag, no batching, no double buffering of `rgb_blargg`/
`s16_rgb555` themselves — the write lands directly in the live table that
the render threads are (concurrently, unsynchronised) reading during
`blargg_filter()`. This is the same "no dirty tracking" answer for
question 2 of the task and is the exact hook point for question 6(b).

**Shadow intensity: `set_shadow_intensity` is effectively dead code.**
`Video::set_shadow_intensity()` (`video.cpp:171-174`) forwards to
`RenderBase::set_shadow_intensity()` (`renderbase.cpp:116-119`), which only
sets `shadow_multi = round(255*f)`. A repo-wide search shows
**`shadow_multi` is never read anywhere** — it is write-only. The actual
shadow colour values come from the hard-coded `S16_shadowVal[32]`/
`S16_shadowVal_5bit[32]` DAC tables (`renderbase.cpp:67-73`, the fixed 63%
"ORIGINAL" hardware intensity described at `video.cpp:154-169`) and the
duplicate `S16_rgbVals[64]` "shadow levels" half used at table-build time
(`snes_ntsc.cpp:61-68`). `Video::set_video_mode()` (`video.cpp:146`) still
calls `set_shadow_intensity(settings->shadow==0 ? shadow::ORIGINAL(0.63f) : shadow::MAME(0.78f))`
(`video.hpp:21-24`), but since its only consumer is dead, toggling the
in-game "shadow" setting currently has **no visible effect** — worth
flagging since a redesign of the shadow path should not assume the MAME
value is reachable today.

**`rgb_blargg` bit layout** (`renderbase.cpp:89-92`):
```
bit 15      : 0 = standard, 1 = shadow   (S16_STANDARD / S16_SHADOW)
bits 14-10  : 5-bit red   (post S16_rgbVal_5bit mapping)
bits 9-5    : 5-bit green
bits 4-0    : 5-bit blue
```
This feeds straight into the filter via `SNES_NTSC_S16(ktable, n)`
(`snes_ntsc.h:280-283`, `#define SNES_NTSC_S16(ktable,n) (ktable + (n * entry_size_bytes))`)
— the macro does **no** bit shuffling of `n` (unlike `SNES_NTSC_RGB16`);
the 16-bit value is used directly as the row number. The comment above it
(`snes_ntsc.h:277-279`) still documents a *two-bit* flag field: `Bits
15-16: 00b = standard, 01b = shadow, 10b = highlight` — a leftover from
the un-trimmed original design, which would need `n` up to ~96K (3×32768
R×G×B combinations) to express highlight. Two things structurally prevent
that: (1) `SNES_NTSC_IN_T` is `unsigned short` (`snes_ntsc_config.h:20`),
so no caller can construct a value ≥ 0x10000 (bit 16) to select
"highlight"; (2) `snes_ntsc_t::table` is sized to
`snes_ntsc_palette_size = 0x10000` rows (`snes_ntsc.h:259`), so
`gen_kernel` only ever runs for `entry` 0..0xFFFF, and the init loop's
decode (`red_index = (entry>>10 & 0x1F) + ((entry>>15)*32)` etc.,
`snes_ntsc.cpp:129-131`) only ever treats bit 15 as the std/shadow
selector into the 64-entry `S16_rgbVals[]` (32 standard + 32 shadow,
`snes_ntsc.cpp:61-68`) — no third ("highlight") block exists to index
into even if bit 16 were set. So the guarantee is structural — the type
and the fixed table dimension jointly rule it out — confirming the 96K
(conceptual) vs 64K (actual, allocated) distinction from the brief.

## 3. Per-frame pipeline

`Video::prepare_frame()` (`video.cpp:176-204`) is the S16 emulation: it
calls `tile_layer->update_tile_values()`, `hwroad`'s background/foreground
render function pointers, `tile_layer->render_tile_layer()` (background
then foreground pass), `sprite_layer->render()`, and
`tile_layer->render_text_layer()`, all writing S16 palette-index values
(0..8191 — see §4/§6c) into `video.pixels`, which at that moment points
into whichever of `pixel_buffers[0/1]` is "current"
(`Video::current_pixel_buffer`, flipped by `Video::swap_buffers()` at
`video.cpp:94-100`). `Video::render_frame(int fastpass)`
(`video.cpp:206-211`) always reads the **other** buffer:
`pixel_buffers[current_pixel_buffer ^ 1]`. This is the double-buffer
contract: `prepare_frame()` (game logic thread) writes frame N+1 while
`render_frame()`/the render threads consume frame N.

**Threading** (`main.cpp:396-521`, `555-611`). Up to 4 threads: a
`prepare_thread` (game tick + `prepare_frame()`, only if `threads>=4`,
`main.cpp:422-431,492`), one or two `render_thread`s
(`render_threads = threads>2 ? 2 : 1`, line 491), coordinated by semaphores
(`renderReady0/1`, `renderDone0/1`, `prepareReady`, `prepareDone`). Each
`render_thread(renderthreads, id)` (`main.cpp:412-419`) calls
`video.render_frame(renderthreads==1 ? -1 : id)` →
`renderer->draw_frame(pixels, fastpass)` with `fastpass` = -1 (whole frame),
0 (top half) or 1 (bottom half). In `main_loop()` (`main.cpp:555-611`),
`video.present_frame()` (GPU upload via `finalize_frame()`) runs on the
**main thread concurrently** with the just-released render threads, and it
uploads `GameSurface[current_game_surface ^ 1]` — the surface written by
the *previous* iteration's `draw_frame` calls, not the one currently being
written. `video.swap_buffers()` (flips both `current_pixel_buffer` and
`current_game_surface`) only runs after `renderDone0/1`/`prepareDone` have
all been acquired (`main.cpp:592-606`). So the pipeline overlaps
prepare(N+1) || render(N) || present(N-1), synchronised by four semaphores
and one flip per loop iteration.

**Conversion loop** — `RenderSurface::blargg_filter()`
(`rendersurface.cpp:954-1018`). For the requested `section` (0=top half,
1=bottom half, -1=whole frame) it computes `spix`/`bpix` offsets into
`gamePixels` (the raw S16 `pixels`) and `rgb_pixels` respectively, then:
```cpp
long pixel_count = (src_pixel_count) >> 2;      // unroll 4:1  (rendersurface.cpp:985)
while (pixel_count--) {
    *(bpix+0) = rgb_blargg[*(spix+0)];          // rendersurface.cpp:989-992
    *(bpix+1) = rgb_blargg[*(spix+1)];
    *(bpix+2) = rgb_blargg[*(spix+2)];
    *(bpix+3) = rgb_blargg[*(spix+3)];
    bpix += 4; spix += 4;
}
```
This is **the** lookup that turns an S16 palette-slot value into the
packed 15-bit-RGB+shadow "colour" value `SNES_NTSC_S16` expects.
Alignment: `pixels`/`writePixels` are only asserted 4-byte aligned via
`__builtin_assume_aligned(ptr, 4)` (`rendersurface.cpp:1281-1282`), though
the underlying allocations are actually 64-byte aligned
(`rgb_pixels`/`create_buffers()`, `rendersurface.cpp:146-153`;
`pixel_buffers[]`, `video.cpp:68-69`, `CB_PIXEL_ALIGNMENT`/`alignment=64`).
After the lookup, `snes_ntsc_blit_hires_fast()` (SIMD) or
`snes_ntsc_blit_hires()`/`snes_ntsc_blit()` (scalar) is called with
`bpix`, `phase`, `src_width`, `block_height` (`rendersurface.cpp:1000-1017`).

**Phase.** `phase` is a `RenderSurface` member (`rendersurface.hpp:85`),
updated once per frame **only** when `fastpass != 1`
(`rendersurface.cpp:1287-1288`):
```cpp
if (fastpass!=1) {
    if (config.fps == 60) phase = (phase + 1) % 3;
    else                  phase = (phase + 2) % 3;
}
blargg_filter(pixels, writePixels, fastpass);
```
The bottom-half call (`fastpass==1`) skips the update and uses whatever
value the top-half call just set — by design (the two halves of one
physical frame must share one burst phase), but note there is **no
ordering guarantee** enforced here between the two render threads beyond
whatever the caller (`main_loop`) already provides by releasing both
`renderReady0`/`renderReady1` together; both threads read the same
`phase` member with no lock, which is benign only because `fastpass==1`
never writes it.

**Scanlines / GL upload.** After `blargg_filter()`, `draw_frame()`
optionally calls `apply_scanlines()` on the just-produced 32-bit buffer
(`rendersurface.cpp:1291-1294`, non-Blargg path at 1309-1312). GL upload
happens later, in `RenderSurface::finalize_frame()`
(`rendersurface.cpp:822-951`), via `glb::update_game_texture(localGameSurface->pixels, ...)`
(lines 851-856) — this runs on the main thread inside `present_frame()`,
decoupled from `draw_frame()` by the double-buffered `GameSurface[2]` and
one frame of latency as described above.

## 4. Hires mode: true resolution vs pixel-doubling

- **Tiles/text**: always pixel-doubled, never truly hires. `hwtiles::render8x8_tile_mask_hires`/`_clip_hires`
  (`hwtiles.cpp:458-540`) call `set_pixel_x4()` (`hwtiles.cpp:449-452`):
  `buf[0]=buf[1]=buf[width]=buf[1+width]=data;` — each native 8x8 tile
  texel becomes a 2x2 block of identical values in the 640x448 buffer.
  Text layer (`render_text_layer`, `hwtiles.cpp:328-353`) uses the same
  lores/hires function-pointer pair. So for tiles and text, even/odd pixel
  pairs in a row are **always** bit-identical, both horizontally and
  vertically, regardless of the `hiresprites` setting.
- **Road**: has dedicated `_hires` render functions (`hwroad.cpp:136-139`,
  `521-593` background, `595-...` foreground). Background (sky/ground
  solid fill) is a flat per-scanline colour fill written 2 lines and 2
  columns at a time via `std::fill_n` on a `uint32_t*` alias
  (`hwroad.cpp:578-587`) — identical even/odd pairs again. Foreground (the
  road stripes) computes **genuine full-640-width per-pixel horizontal
  positions** every scanline (`hwroad.cpp:695` onward), but only
  recomputes colour/`hpos` tables on **even** output scanlines and
  *interpolates* the odd scanline from the ones above/below
  (`hwroad.cpp:643-668`), so vertically the road is not independently
  simulated at 448 lines, only horizontally.
- **Sprites**: the only layer with a real toggle, `config.video.hiresprites`
  (`config.hpp:134`, default 0 — `config.cpp:216`). Sprite y-drawing always
  scales `top/ytarget <<= 1; zoom >>= 1;` in hires mode regardless of the
  toggle (`hwsprites.cpp:478-484`), giving continuous (not doubled)
  vertical positioning from the existing zoom accumulator. The toggle
  instead controls **horizontal sub-pixel placement**: when
  `hiresprites==1`, `osprites.cpp:632-636` doubles the ROM-derived
  `sprite_width`/`sprite_height` and an offset table
  (`ramBuff[data+15]`, applied at `hwsprites.cpp:600-601`, `xpos += offset`)
  nudges the draw position by a precomputed sub-pixel amount. Either way
  sprites are not simply 2x2-block-doubled the way tiles are — they can
  produce distinct even/odd columns.

**Net effect for the CT/NTSC discussion**: because tile, text and
road-background pixels are always duplicated pairs, a large fraction of
the 640-wide row seen by the filter is not independent per-pixel
information — relevant to any scheme exploiting repeated pixel runs, but
orthogonal to the palette-index question in §6.

## 5. How often OutRun rewrites palette RAM

Two call sites run unconditionally **every emulated frame**, from
`Outrun::vint()` (`outrun.cpp:178-187`, invoked once per tick):
- `opalette.cycle_sky_palette()` (`opalette.cpp:221-247`) — gated by
  `sky_palette_init & BIT_1`; when active it writes 0x20 (32) palette
  words via `write_pal32` (64 `refresh_palette` calls) every *other*
  frame (the `(d0^d1)&1` check).
- `opalette.fade_palette()` (`opalette.cpp:258-282`) — gated by
  `pal_manip_ctrl & BIT_0`/`BIT_1` and `game_state`; when active it calls
  `write_fade_to_palram()` (`opalette.cpp:471-490`), 8+16=24 `write_pal16`
  calls per road (road1/road2), i.e. 48 `refresh_palette` calls, every
  frame the fade runs (decremented by `fade_counter`).

Both are otherwise no-ops (early `return`), so in ordinary driving with no
sky-cycle Easter egg and no stage-end fade in progress, **zero** palette
writes happen per frame from these two functions. Other palette writers
found (`otiles.cpp:71,92-95,110-124,129,769-772,829`, `osprites.cpp:304`,
`ohiscore.cpp:55,66`, `omusic.cpp:419`, `outrun.cpp:760-763`) are all tied
to level/stage load, hi-score entry, music-test screens, or service mode —
one-off/rare, not per-frame. **How many palette entries are typically
"live" at once cannot be determined from static code** — the code only
bounds the *address space* (4096 entries, 8192 with shadow), not the
runtime working set. (This is consistent with the original brief and idea shortlist's
empirically-measured "a few hundred distinct input values" per frame, but
that number comes from runtime measurement, not this audit.)

## 6. Implications for the CT (compact table) design

**(a) Can the conversion pass emit a slot index instead of a colour
value?** Yes — and in a sense it already does, one layer earlier than the
filter. The S16 `pixels` buffer `prepare_frame()` produces is *not* raw
colour data; it already holds values in the exact 0..8191 range that
indexes `rgb_blargg`/`s16_rgb555` directly: tile/text/road pixel values
are `nPalette + nibble` with `nPalette <= (0x7f<<3)` (tiles,
`hwtiles.cpp:365`) or a road colour-table entry `<= color_offset3|0x7f`
(`hwroad.cpp:349-361`, `color_offset3=0x780`), all `< 0x1000`; sprite
pixels are `COLOR_BASE(0x800) + ((colourfield&0x7f)<<4) + nibble`,
`< 0x1000`, with the **shadow flag applied as
`pPix |= S16_PALETTE_ENTRIES (0x1000)`** directly on the pixel value
(`hwsprites.cpp:199,224,240-241,258-260,...`). So "the palette index, or
palette-index+0x1000 for shadow" *is* the CT slot number, already
computed by the emulation layers and already sitting in `pixels[]`. The
one remaining conversion step is `rgb_blargg[pixels[i]]` in
`blargg_filter()` (`rendersurface.cpp:989-992`) — exactly the "remap load"
the idea shortlist proposed removing. To make the filter
consume slot indices directly: change the kernel-table addressing from
`SNES_NTSC_S16` treating `n` as a packed-RGB+flag code
(`snes_ntsc.h:280-283`) to a CT variant treating `n` as a direct row index
already in slot space (same `ktable + n*entry_size_bytes` arithmetic, just
a smaller `snes_ntsc_palette_size`, e.g. `S16_PALETTE_ENTRIES*2 = 8192`
rows = 4 MiB instead of 32 MiB); and remove the
`bpix[i] = rgb_blargg[spix[i]]` copy loop (`rendersurface.cpp:984-993`)
entirely, handing `pixels`/`spix` straight to
`snes_ntsc_blit_hires_fast/_hires/blit()` as `SNES_NTSC_IN_T` (can stay
`unsigned short`, `snes_ntsc_config.h:20`, since 8192 fits in 16 bits).
The old `0x8000` shadow bit becomes irrelevant to the filter — shadow is
already folded into the slot number via `+S16_PALETTE_ENTRIES`.
`s16_rgb555[]` (the non-Blargg direct-RGB555 path,
`rendersurface.cpp:1298-1305`) is unaffected — a separate table and code
path.

**(b) Where would per-entry kernel regeneration hook in, and worst case
per frame?** The natural hook is exactly `RenderBase::convert_palette()`
(`renderbase.cpp:95-113`), called once per palette RAM write via
`Video::refresh_palette()` (`video.cpp:585-604`), itself called once per
`write_pal8/16/32` (`video.cpp:428-500`, a 32-bit write calls it twice).
Today this function writes 2 table entries (std + shadow) per call; under
CT it would instead run `gen_kernel()`/`correct_errors()` (currently only
invoked from `snes_ntsc_init()`, `snes_ntsc.cpp:107-166`) for the 1-2
affected CT rows (`adr` and `adr+S16_PALETTE_ENTRIES`). Per §5, the
worst observed *per-frame* volume from game-logic is `cycle_sky_palette()`
at 64 `refresh_palette` calls (128 CT rows) every other frame while the
sky-cycle Easter egg runs, or `fade_palette()` at 48 calls (96 CT rows)
every frame while an end-of-stage/road-colour fade is active — both are
transient, not steady-state. One-off level-load writes (`otiles.cpp`,
`osprites.cpp` PALETTE_EXPANSION) can be larger bursts but happen at
stage-change cadence, not per-frame steady state.

**(c) Obstacles.**
- *Palette writes happen mid-frame, on the game-logic thread, into a table
  the render threads concurrently read with no lock or double-buffering* —
  this race already exists today (`rgb_blargg[]` is a plain array written
  in place by `convert_palette()`), so CT does not introduce it, but CT's
  regeneration work per write (`gen_kernel`/`correct_errors`, float math
  over ~128 words) is much larger than today's two `uint16_t` stores, so
  the torn-read window and CPU cost both grow. The existing
  settings-change reinit path already takes `drawFrameMutex` around a full
  `init_blargg_filter()` (`rendersurface.cpp:1338-1341`) — incremental
  per-write regeneration would need the same discipline, but invoked from
  the engine/prepare thread rather than a render thread, a new
  cross-thread interaction not present today.
- *Normal and shadow slots are never shared* — every palette write always
  produces both a standard row and a shadow row (`renderbase.cpp:104-112`),
  so CT's 8192-row sizing (`S16_PALETTE_ENTRIES*2`) is fixed and correct;
  no write ever touches only one of the two.
- *No layer emits colours outside the palette-RAM-derived index space*: a
  check of every producer of `pixels[]` values (tiles, text, road,
  sprites) shows each composes its final value from
  `COLOR_BASE`/`color_offset*`/`nTilePalette` constants plus a 4-bit tile
  nibble, always `< 0x1000`, with only `hwsprites.cpp` setting the
  `0x1000` shadow bit. Nothing writes a `pixels[]` value independent of
  palette RAM — but this is convention, not enforcement: CT would
  silently read an out-of-intended-range slot rather than fault if some
  future producer (or RAM corruption) broke the pattern, the same latent
  assumption the current code already has.
- *Dead code nearby hints at unfinished refactors*: `nPaletteOffset`/
  `TILEMAP_COLOUR_OFFSET` (`hwtiles.cpp:292-301`,
  `hwtiles.hpp:60,70,80,90,100,110`) is computed and passed into every
  `render8x8_tile_mask_*` variant but never read inside any of them.
  Likewise `shadow_multi` (§2) and `RenderBase::rgb_lookup[LOOKUP_SIZE]`
  (`renderbase.hpp:36`, never read or written anywhere in `src/main`) are
  dead. None block a CT implementation, but they're a reminder not every
  table/parameter here is load-bearing — worth re-checking before
  assuming any given array is "the" source of truth.

**(d) Frame-dump hook point and buffers.** The filter's literal input is
`bpix` (today; would be `pixels` directly under CT) as passed to
`snes_ntsc_blit_hires_fast/_hires`/`snes_ntsc_blit` at
`rendersurface.cpp:1000-1017`, together with the `phase` used for that
call and the geometry (`src_width`, `block_height`, `output_pitch`,
`Ashifted`). Two viable capture points:
  1. **Inside `blargg_filter()`**, right after the conversion loop
     (`rendersurface.cpp:993`, once `bpix` is populated for `this_section`)
     and before the `snes_ntsc_blit*` call — runs on whichever render
     thread owns that half (no extra cross-thread sync needed for its own
     half), but fires twice per frame (per `fastpass`) and must be
     labelled/merged by `section`.
  2. **A whole-frame, already-synchronised alternative**: in
     `main.cpp:592-595`, right after `renderDone0.acquire();` and (if
     `render_threads==2`) `renderDone1.acquire();` — both halves of
     `rgb_pixels` are guaranteed complete, and `phase` (constant for the
     frame, set at `rendersurface.cpp:1287-1288`) can be read without
     racing either render thread. Cleanest whole-frame point, but needs a
     public accessor for `RenderSurface`'s private `rgb_pixels`/`phase`
     (`rendersurface.hpp:85-102`), which `main.cpp` cannot see today.
  Either way, the buffer to dump for an offline replay is `rgb_pixels`
  (size `src_width*src_height*sizeof(uint16_t)`, contiguous, already the
  packed-RGB+shadow format the filter consumes) plus the single `int
  phase` — sufficient to reproduce a `snes_ntsc_blit_hires_fast()` call
  byte-for-byte with no palette/engine state, matching the call contract
  the original brief already assumed. Capturing
  the earlier `pixels` (raw S16 slot buffer, pre-`rgb_blargg`) instead
  would additionally validate a CT implementation end to end, since under
  CT that buffer *is* the filter's input.

## Files referenced

- `src/main/video.cpp`, `src/main/video.hpp`
- `src/main/sdl2/renderbase.hpp`, `src/main/sdl2/renderbase.cpp`
- `src/main/sdl2/rendersurface.hpp`, `src/main/sdl2/rendersurface.cpp`
- `src/main/sdl2/snes_ntsc.h`, `src/main/sdl2/snes_ntsc.cpp`,
  `src/main/sdl2/snes_ntsc_config.h`
- `src/main/main.cpp`
- `src/main/globals.hpp`, `src/main/frontend/config.hpp`,
  `src/main/frontend/config.cpp`
- `src/main/hwvideo/hwtiles.cpp/.hpp`, `src/main/hwvideo/hwsprites.cpp/.hpp`,
  `src/main/hwvideo/hwroad.cpp/.hpp`
- `src/main/engine/opalette.cpp/.hpp`, `src/main/engine/outrun.cpp`,
  `src/main/engine/osprites.cpp`, `src/main/engine/otiles.cpp`
