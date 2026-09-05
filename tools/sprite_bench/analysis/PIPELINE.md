# CannonBall-SE Sprite Rendering Data Flow — Read-Only Code Audit

Scope: static-code audit of the working tree on branch `ntsc-rearch` (HEAD
`583b689`, plus the uncommitted `rendersurface.cpp`/`snes_ntsc.*` edits noted
in git status — none of those touch the sprite path). Builds on
`tools/ntsc_bench/analysis/GRAPHICS_SETUP.md` (palette mechanics, S16 pixel
buffers, the prepare/render/present pipeline, Blargg table lifetime); that
material is not repeated here except where sprite-specific detail requires
it, and every figure taken from it was independently re-checked against the
current tree.

## 1. Threading model per frame

`hwsprites::render()` is called once, synchronously, from
`Video::prepare_frame()` (`video.cpp:201`, inside the `else` branch at
189-203). `prepare_frame()` runs wherever the *game engine* tick runs — it
is **not** one of the `render_thread` instances despite the name overlap.
Which thread that is depends on `-t`:

| `-t` | `using_threading` | `render_threads` | `prepare_threads` | Where `hwsprites::render()` runs | Where NTSC/RGB conversion runs |
|---|---|---|---|---|---|
| 1 | false | — | — | main thread, sequential (`main.cpp:614-627`) | main thread, `video.render_frame(-1)` right after |
| 2 | true | 1 | false | main thread (`main.cpp:598-605`) | 1 dedicated render thread (whole frame) |
| 3 | true | 2 | false | main thread | 2 render threads (top/bottom half, `fastpass` 0/1) |
| 4 (Linux only) | true | 2 | true | dedicated `prepare_thread` (`main.cpp:431-440,512`) | 2 render threads |

`main_loop()` (`main.cpp:488-501`) computes `render_threads = threads>2 ? 2
: 1` and `prepare_threads = threads>=4`. **On Windows, `-t` is capped to 3**
(`main.cpp:490-493`, comment: input must stay on the main thread), so a
dedicated prepare thread — and therefore any thread other than main running
sprite/tile/road drawing — never exists on Windows; only Linux can reach
`-t 4`. `-t x` itself parses a single digit 1-4 (`main.cpp:782-789`);
anything else prints an error and is ignored. Default thread count is
`omp_get_max_threads()` (`main.cpp:183`), further forced to 1 if
`singleCorePi()` detects a single-core Raspberry Pi or `-1`/`-x` is passed
(`main.cpp:791-798,880-886`).

Per iteration (`main_loop`, `main.cpp:548-737`): the previous iteration's
`renderDone0/1`/`prepareDone` have already been acquired, so it is safe to
call `video.flush_ntsc_palette()` (main.cpp:591) before releasing
`renderReady0/1` and (if `prepare_threads`) `prepareReady`; the main thread
then does `video.present_frame()` (GPU texture upload, `main.cpp:608`)
concurrently with whichever thread(s) are doing prepare/render work, and
finally acquires all the done-semaphores before `video.swap_buffers()`
(`main.cpp:639`) flips both `current_pixel_buffer` and `current_game_surface`.
This gives the `prepare(N+1) || render(N) || present(N-1)` overlap described
in `GRAPHICS_SETUP.md` §3, with sprite drawing occurring during the
"prepare" stage only.

**`-perftest`** (`main.cpp:799-801,652-654`) sets `cannonball::perftest =
true`; the only effect is that `nextFrameTime` is reset to `now` every
iteration instead of being paced by `frameDuration`, defeating the
"sleep until due" branch (`main.cpp:655-664`) so the loop runs flat-out.
It prints nothing extra — the existing every-2-second console line
`"%i FPS (dropped: %i%%)"` (`main.cpp:669-681`, driven by `renderedFrames`
and `droppedFrames`) is the only output, now reflecting the unthrottled
rate. It does not isolate the sprite path from tiles/road/NTSC/audio.

**Frame pacing** otherwise: `frameDuration = 1/targetFPS` (30 or 60,
`-30`/`-60` or auto-detected via the 10-second `fps_eval_period` check,
`main.cpp:684-736`), vsync-aware sleep when not using `-perftest`
(`main.cpp:655-664`), and a "drop if behind schedule, but always force
every 4th frame" policy (`main.cpp:567-576`) that skips `prepare_frame()`
(and hence `hwsprites::render()`) entirely for a dropped frame, running only
`tick()`/`audio.tick()`.

## 2. Sprite RAM

Lives inside `hwsprites` (owned by `Video::sprite_layer`, `video.hpp:34`,
constructed `video.cpp:40`) as two parallel `uint16_t` arrays,
`ram[SPRITE_RAM_SIZE]` (front, engine-writable) and `ramBuff[SPRITE_RAM_SIZE]`
(back, render-readable), `SPRITE_RAM_SIZE = 128*16 = 2048`
(`hwsprites.hpp:29,43-44`) → **4 KiB each, 8 KiB total**. The header comment
"128 sprites, 16 bytes each (0x400)" is stale: the `was *8` note next to it
shows the per-sprite stride was doubled from 8 to 16 words (16→32 bytes) to
add scratch fields, but the comment wasn't updated.

**Write path**: `OSprites::do_sprite()` (`osprites.cpp:569-905`) converts
one engine-side `oentry` into an `osprite` (16×`uint16_t` `data[]` +
`uint32_t scratch`, `osprite.hpp:19-20`) via bit-packing setters in
`osprite.cpp` (`set_pitch`, `set_vzoom`, `set_pal`, `set_height`,
`set_clip`, `set_width`, `set_offset(int16_t)`, etc.). `OSprites::sprite_copy()`
(`osprites.cpp:432-494`) drives `do_sprite()` for every sprite in priority
order, then `finalise_sprites()` (503-519) writes an end marker
(`0xFFFF,0xFFFF`) at `sprite_entries[sprite_count]` and calls
`blit_sprites()` (528-543), which walks `sprite_entries[0..sprite_count]`
and calls `video.write_sprite16(&dst_addr, data[i])` for all 16 words of
each entry — **one contiguous pass, all entries written in one go**, not
incrementally across frames. `Video::write_sprite16` (`video.cpp:423-427`)
masks the address to `0xfff` and calls `hwsprites::write()`
(`hwsprites.cpp:162-165`, `ram[adr>>1] = data`). This whole sequence runs
once per `Outrun::jump_table()` (called from `Outrun::tick()`,
`outrun.cpp:275` `osprites.sprite_copy();`), i.e. once per game-logic tick,
the same tick that runs inside `prepare_frame()`'s thread context.

`finalise_sprites()` sets `do_sprite_swap = true`; the actual `ram`↔`ramBuff`
swap happens in `OSprites::update_sprites()` (`osprites.cpp:139-147`),
called from `Outrun::vint()` (`outrun.cpp:182`) — which runs *after*
`jump_table()` within the same `tick()` (`outrun.cpp:160-162`). So each
game tick both fills `ram[]` with the new frame's sprites and immediately
flips it into `ramBuff[]` for `hwsprites::render()` to read that same tick
— `hwsprites::swap()` (`hwsprites.cpp:168-180`) does a plain word-by-word
exchange, not a pointer swap.

**Max entries**: software-side cap is `JUMP_ENTRIES_TOTAL = SPRITE_ENTRIES
(0x62=98) + 24 = 122` (`osprites.hpp:43,49`), enforced by
`do_spr_order_shadows()`'s `if (spr_cnt_main+spr_cnt_shadow >=
JUMP_ENTRIES_TOTAL) return;` guards (`osprites.cpp:371,392`); the separate
`>0x7F` check in `sprite_copy()` (line 440) is consequently dead — the
121-entry ceiling is always hit first. Hardware RAM itself holds up to 128
slots (`SPRITE_RAM_SIZE/16`), so RAM capacity is never the limiting factor.
`no_sprites` (`osprites.cpp:40`) additionally throttles how many of the 98
scenery slots are active based on `config.engine.level_objects`, but does
not change the hardware write path.

**Per-entry field layout** (as decoded by `hwsprites::render()`,
`hwsprites.cpp:421-457`, matching the `osprite::set_*` writers):

| Word | Bits used | Meaning |
|---|---|---|
| `data[0]` | `e-------` end marker; `-h-h----` hide; `--c-----` clip flag; `----bbb-` bank; `-------t tttttttt` top scanline+256 |
| `data[1]` | offset within sprite bank |
| `data[2]` | `ppppppp-` pitch (signed 7-bit, combined with a bit from `data[4]`) |
| `data[3]` | `--pp----` priority tier (2 bits); `------vv vvvvvvvv` vertical zoom |
| `data[4]` | `y-------` top-to-bottom flag; `-f------` h-flip; `--x-----` left-to-right flag; width |
| `data[5]` | `hhhhhhhh` height-1 (high byte); `-ccccccc` palette (low byte) |
| `data[6]` | X position |
| `data[7]` | `set_rawh` — underlying (pre-scale) sprite height |
| `data[15]` | JJP addition — hi-res sub-pixel X offset (`osprite::set_offset(int16_t)`, applied at `hwsprites.cpp:601-602`) |

## 3. Sprite ROM

Loaded by `roms.cpp:65-73`: `sprites.init(0x100000)` (1 MiB buffer),
8×128 KiB files loaded `INTERLEAVE4` (4-way byte interleave, standard for
the 4 sprite ROM pairs). `hwsprites::init()` (`hwsprites.cpp:94-118`)
re-packs every 4 source bytes into one little-endian `uint32_t` in
`sprites[SPRITES_LENGTH]`, `SPRITES_LENGTH = 0x100000>>2 = 0x40000`
(262,144) — a straight endian-swap covering the *entire* 1 MiB, no
subsetting. `numbanks = SPRITES_LENGTH/0x10000 = 4`; a sprite's bank field
(`data[0]` bits 9-11) is reduced `% numbanks` and offsets into
`sprites + 0x10000*bank` (`hwsprites.cpp:419,468-471`).

**No table is precomputed at startup** from the ROM. Two same-sized
companion arrays are lazily memoised *during play*, one 32-bit word/row at
a time, the first time each row of each orientation is drawn:
`sprites_flipped[SPRITES_LENGTH]` (h-flipped pixel order, sentinel
`0xffffffff` set by `reset()`, filled at `hwsprites.cpp:535-577` on first
flipped read) and `sprites_shadowinfo[SPRITES_LENGTH]` (`uint8_t`, sentinel
`0xff`, set to `0x11` if any nibble in the row's four 32-bit words matches
the shadow-pixel code `0xA`, else `0x00`; filled at `hwsprites.cpp:578-598`
for the non-flipped path). This is a runtime cache keyed by ROM offset, not
a startup pass — a row that is never drawn (e.g. an unused sprite bank on a
given track) is never computed. The "clip" bit referenced in the task brief
is **not** part of any ROM-derived table; it is the per-sprite-instance
`data[0]` bit 0x2000 computed each tick in `OSprites::do_sprite()`
(`osprites.cpp:796-800`, `fully_inside`/`clip` from screen-bounds math) and
consumed by `hwsprites::render()`'s jump-table dispatch (`clip ? 0u : 4u`,
`hwsprites.cpp:650`).

## 4. Pixel buffer

`Video::pixel_buffers[0/1]` are `uint16_t*` S16 palette-index buffers,
allocated in `Video::init()` (`video.cpp:66-69`) at
`config.s16_width × (config.s16_height+2) + 64` elements, 64-byte aligned.
Dimensions (`video.cpp:118-138`, `globals.hpp:32-37`):

| Mode | Width | Height |
|---|---|---|
| Lo-res, standard | 320 | 224 |
| Lo-res, widescreen | 404 | 224 |
| Hi-res, standard | 640 | 448 |
| Hi-res, widescreen | 808 | 448 |

`config.video.hires` defaults to **1** (`config.cpp:215`), so hi-res
(640×448 or 808×448) is the out-of-the-box mode; `config.video.hiresprites`
defaults to **0** (`config.cpp:216`, per `GRAPHICS_SETUP.md`'s citation,
re-confirmed here) — hi-res mode still runs sprites at continuous
(non-doubled) vertical resolution regardless of that toggle
(`hwsprites.cpp:482-487`, `top/ytarget <<= 1; zoom >>= 1;`); the toggle only
adds the sub-pixel `data[15]` X nudge and doubles the ROM-derived sprite
w/h lookup (`osprites.cpp:632-638,712-718`).

**Encoding**: each `uint16_t` element is a direct index into
`RenderBase::rgb_blargg[]`/`s16_rgb555[]` (`S16_PALETTE_ENTRIES*2 = 8192`
entries each, `renderbase.hpp:50,52`), not a colour value. Sprite pixels use
`COLOR_BASE(0x800) + ((palette&0x7f)<<4) + nibble` (`hwsprites.hpp:31`,
`hwsprites.cpp:448-449`), always `< 0x1000`; the shadow flag is folded in
by OR-ing `S16_PALETTE_ENTRIES (0x1000)` directly onto the stored pixel
(`hwsprites.cpp:224,240-241,258-260,...`, the `*pPix |= S16_PALETTE_ENTRIES`
lines in the `draw_pixel_*row()` macros) rather than being a separate bit
field. Tile/road/text layers occupy `< 0x1000` from disjoint sub-ranges
(`nPalette` for tiles, `color_offset3=0x780` for road, `hwtiles.cpp:365`,
`hwroad.cpp:130,285,569,583` — re-verified against the current tree).

**Composition** (`Video::prepare_frame()`, `video.cpp:189-203`): all
layers write into the *same* `pixels` buffer (the currently-writable half
of `pixel_buffers[]`), in this fixed order — road background, tile
background pass, tile foreground pass, road foreground, **sprites**, text.
Sprites therefore write **directly** into the final S16 index buffer, not
into a separate intermediate that is merged later; that S16 buffer *is*
itself the intermediate consumed by `RenderSurface::draw_frame()`
(NTSC/RGB conversion, a different thread — see §1) to produce the
GPU-uploadable `GameSurface`.

`sprite_layer->render(pixels, 8)` is called with a **fixed, single**
priority value of 8 (`video.cpp:201`) — there is exactly one call site
(verified repo-wide). Internally, `sprpri = 1 << ((ramBuff[data+3]>>12)&3)`
(`hwsprites.cpp:427`) yields one of {1,2,4,8}, and any entry whose 2-bit
priority field isn't 3 is skipped by `if (sprpri != priority) continue;`
(line 428) for *this and every other call*, since no other call exists.
Every `.shadow =` assignment found in the engine (which feeds this field
via `set_priority`, `osprite.cpp:102-105`) uses 3 or 7 (both end `...11` in
binary) — except two ROM-sourced assignments (`olevelobjs.cpp:67`,
`omap.cpp:237`, `roms.rom0p->read8(...)`) whose value cannot be bounded
statically. See open questions.

## 5. Per-frame memory working set

| Buffer | Type/size | Bytes |
|---|---|---|
| `hwsprites::ram`+`ramBuff` | 2×2048×`uint16_t` | 8,192 |
| `hwsprites::sprites` | 262,144×`uint32_t` | 1,048,576 |
| `hwsprites::sprites_flipped` | 262,144×`uint32_t` | 1,048,576 |
| `hwsprites::sprites_shadowinfo` | 262,144×`uint8_t` | 262,144 |
| `OSprites::sprite_entries` | 122×(16×`uint16_t`+`uint32_t`) | ≈4,392 |
| `OSprites::sprite_order`+`sprite_order2` | 2×8192×`uint8_t` | 16,384 |
| `ZOOM_LOOKUP`+`ZOOM_LOOKUP_HIRES` | ≈514×4×`uint16_t` | ≈4,112 |
| `rgb_blargg`/`s16_rgb555` (shared, not sprite-only) | 2×8192×`uint16_t` | 32,768 |
| Destination `pixel_buffers[0]` (hi-res, widescreen) | 808×450+64 ×`uint16_t` | ≈727,328 |
| Destination `pixel_buffers[0]` (hi-res, standard) | 640×450+64 ×`uint16_t` | ≈576,128 |
| Destination `pixel_buffers[0]` (lo-res, standard) | 320×226+64 ×`uint16_t` | ≈144,768 |

`sprites`+`sprites_flipped`+`sprites_shadowinfo` total **≈2.25 MiB**
resident for the lifetime of `hwsprites` regardless of how much is
actually touched per frame; the per-frame *touched* subset is
data-dependent (sum of `pitch×height` words over up to 121 active sprites)
and cannot be bounded from static code — see open questions. `WH_TABLE`
(`oaddresses.hpp:604`, `0x20000` into `rom0p`, the main-CPU ROM, not the
sprite graphics ROM) is read per-sprite in `do_sprite()`
(`osprites.cpp:626-736`) but its extent isn't declared in code; a
commented-out debug dump (`osprites.cpp:594-600`) iterates `i<0x8000`,
suggesting ~32 KiB, unconfirmed.

## 6. Build flags reaching hwsprites.cpp

From `CMakeLists.txt`: C++23, `-Wall -Wextra -Wpedantic` (GCC/Clang) plus a
`QUIET_BUILD`-gated set of `-Wno-*` suppressions (lines 444-465);
**`-Ofast`** in Release/RelWithDebInfo only (`CMakeLists.txt:413-416`);
**`-march=native`** for GCC/Clang Release/RelWithDebInfo when
`WITH_MARCH_NATIVE` is ON (default ON, `CMakeLists.txt:58-59,468-475`) —
there is no separate `-mcpu=...` handling for ARM in the main build (unlike
`tools/ntsc_bench/run_pi.sh`, which hardcodes `-mcpu=cortex-a53` for its own
standalone build); on Raspberry Pi, codegen relies entirely on GCC's
`-march=native` host auto-detection. No unity build and no LTO
(`INTERPROCEDURAL_OPTIMIZATION` is only ever explicitly set to `FALSE`,
under `ENABLE_PROFILING`, `CMakeLists.txt:484-490`; otherwise left at
CMake's default OFF).

**`PIXEL_ACCURACY` is effectively dead for hwsprites.cpp.** The CMake
option (`CMakeLists.txt:68,279-283`, default **ON**) passes
`-DPIXEL_ACCURACY=1` (or 0) on the compiler command line, but
`hwsprites.cpp:58` unconditionally does `#define PIXEL_ACCURACY 0` with no
prior `#undef` — a plain macro redefinition that wins for the rest of the
translation unit regardless of the build-system value, so the `#if
PIXEL_ACCURACY` branch (the "glowy edge" hardware-accurate shadow path,
lines 182-207) is **never** compiled; the `#else` branch (lines 209-410,
the jump-table macros used throughout §2/§4) is always what builds,
irrespective of `-DPIXEL_ACCURACY`.

## 7. Existing instrumentation

- **`-dump <dir> [interval] [count]`** (`main.cpp:803-826`,
  `rendersurface.cpp:1335-1383,1420-1443`): writes the *post-composition*
  S16 `pixels` buffer (which already includes sprite output — this is the
  buffer `hwsprites::render()` writes into) plus a snapshot of
  `rgb_blargg[]` and the pre-computed colour-mapped frame, to
  `frame_NNNNNN.bin` every `interval` rendered frames (default 30) until
  `count` frames (default 60) have been written, then triggers a clean
  quit via `cannonball::dump_finished`. It is not sprite-specific — no
  separate hook captures sprite RAM, sprite ROM, or the intermediate
  jump-table state.
- **`hwsprites` per-path timing**: public members `setup`
  (`std::chrono::nanoseconds`, `hwsprites.hpp:21`) and `draw[16]`
  (`hwsprites.hpp:22`) are declared for per-jump_key timing. `setup`'s
  accumulation is commented out (`hwsprites.cpp:616-617`) so it is always
  zero; `draw[jump_key] += ...` (`hwsprites.cpp:981`) *is* live and
  accumulates every frame, but nothing outside `hwsprites.cpp`/`.hpp` ever
  reads either member (repo-wide grep) — write-only instrumentation. A
  function-local `static uint32_t freq[32]` (line 418, incremented 656) and
  `static uint32_t reps[6]` (line 416, incremented inside the
  `draw_pixel_1row_nc()` macro, line 289) are likewise accumulated forever
  but only ever printed by a commented-out debug block at the end of
  `render()` (`hwsprites.cpp:984-1002`).
- **`-perftest`**: see §1 — reuses the existing FPS/dropped-% console line,
  not sprite-specific.
- No sprite-specific "palette usage" debug hook was found; the "few hundred
  distinct input values" figure referenced in `tools/ntsc_bench` docs comes
  from external tooling (`frame_stats.py` over `-dump` output), not
  in-engine instrumentation.

## 8. Open questions

1. Does the single fixed-priority (`8`) call to `sprite_layer->render()`
   (`video.cpp:201`) silently drop sprites whose ROM-sourced `shadow` field
   (`olevelobjs.cpp:67`, `omap.cpp:237`) has priority bits (12-13) other
   than `11`? Every hardcoded `.shadow =` assignment in the engine is 3 or
   7 (both satisfy the mask), but the two ROM-driven assignments cannot be
   bounded statically. If the original hardware/MAME design used multiple
   priority-tier render passes interleaved with road drawing, and this SE
   fork collapsed them to one pass, is that collapse verified safe for all
   ROM-authored level-object data, or just for the tracks tested?
2. Exact byte extent of `WH_TABLE` (`oaddresses.hpp:604`) in `rom0p` is not
   declared anywhere in code; inferred ~32 KiB from a commented-out debug
   loop only.
3. Per-frame sprite-ROM *touched* working set (as opposed to the resident
   1 MiB+1 MiB+256 KiB) is inherently data-dependent (varies with sprite
   pitch/height per active entry) and not derivable from static code.
4. Whether `-march=native` alone gives adequate ARM codegen for the sprite
   inner loops on the Pi Zero 2 W bench box, versus an explicit
   `-mcpu=cortex-a53` as used by `tools/ntsc_bench`, was not tested here —
   worth a build comparison before assuming parity with the bench tool's
   numbers.
5. Confirm whether the CMake `PIXEL_ACCURACY` option is intended to be
   user-facing (README doesn't mention it) — if not, the option and its
   `target_compile_definitions` could be removed without effect, since
   `hwsprites.cpp` always overrides it to 0.

## Files referenced

- `src/main/video.cpp`, `src/main/video.hpp`, `src/main/globals.hpp`
- `src/main/hwvideo/hwsprites.cpp/.hpp`, `hwtiles.cpp/.hpp`, `hwroad.cpp/.hpp`
- `src/main/sdl2/rendersurface.cpp/.hpp`, `renderbase.hpp`
- `src/main/main.cpp`
- `src/main/roms.cpp/.hpp`, `src/main/romloader.hpp`
- `src/main/engine/osprites.cpp/.hpp`, `osprite.cpp/.hpp`, `outrun.cpp`
- `src/main/engine/data/ozoom_lookup.hpp`, `oaddresses.hpp`
- `src/main/frontend/config.cpp/.hpp`
- `CMakeLists.txt`, `res/config.xml`, `README.md`, `install.sh`,
  `tools/ntsc_bench/run_pi.sh`
