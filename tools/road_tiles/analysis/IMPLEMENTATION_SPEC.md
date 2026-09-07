# Implementation specification: road and tile renderer optimisations (S0-S7)

Branch `sprite-a53`. Companion to `ROAD_TILE_RENDERER.md` (which absorbed the ranked plan and the data study)
(data study). Line numbers below refer to the files as they stand on this branch at the time of
writing (`hwroad.cpp` 790 lines, `hwtiles.cpp` 548 lines, `video.cpp` 646 lines); an implementer
whose file has drifted must re-anchor on the quoted code, not the number.

Every section is self-contained. An implementer takes one section, reads S0 (shared harness and
macro scheme) plus their section, and needs nothing else. Sections are listed in dependency
order; the dependency graph is in S0.7.

Facts that are fixed and must not be re-derived:

- Target Cortex-A53 AArch64 (Pi Zero 2 W). The tree must also build on x86-64 MSVC and on ARM
  without NEON. Any NEON or `DC ZVA` code is compile-time guarded with a scalar fallback.
- The frame buffer is 16-bit palette slots. Hi-res non-wide (shipping): 640 x 448, pitch 1280
  bytes = exactly 20 cache lines per row. Hi-res wide: 808 x 448, pitch 1616 bytes (not
  line-aligned). Lo-res: 320 x 224 (pitch 640 = 10 lines) or 404 x 224 (pitch 808, not aligned).
  `pixels` is 64-byte aligned (`video.cpp:67-72`: `align_val_t(64)` base plus 64 halfwords =
  128 bytes) and the allocation has two spare rows after row `s16_height - 1` (`video.cpp:67`).
- `render_background` paints sky rows only; `render_foreground` paints road rows only and skips
  rows where both roads carry the low-priority bit 0x800 (`hwroad.cpp:631`). Every row the
  foreground paints, it paints in full, opaquely, including the off-road margin. The whole
  foreground pass is skipped when `config.engine.fix_bugs && oroad.horizon_base ==
  ORoad::HORIZON_OFF` (`video.cpp:208`).
- Output must remain bit-identical for every frame in hi-res non-wide, hi-res wide, and lo-res.

Two structural facts an implementer must know that are easy to get wrong:

1. **Which rows the foreground writes depends on the control mode.** From `hwroad.cpp:631-635`
   plus the `continue` at `715-716` (mode 0) and `776-777` (mode 3): mode 0 writes a row iff
   road 0 is not solid; mode 3 iff road 1 is not solid; modes 1 and 2 iff not both solid. The
   background (`534-559`) fills a row in mode 0 iff road 0 is solid, mode 3 iff road 1 is solid,
   modes 1 and 2 iff either is solid. So in modes 0 and 3 the two passes are exactly
   complementary by construction; in modes 1 and 2 they cover every row (union is all rows) but a
   row with exactly one solid road is written by both, foreground last. In OutRun's data both
   bits are always set together, which is why the measurement saw exactly 448 rows; the mask in
   S3 must nevertheless follow the foreground's own predicate, never the background's.
2. **Hi-res rows come in pairs that share a fate.** For odd `y`, `yy = y >> 1` is the same as for
   `y - 1`, `data0`/`data1` are re-read from the same road RAM words, and the interpolation block
   (`648-674`) only rewrites `data0`/`data1` when their 0x800 bit is already clear. So the skip
   decisions for rows `2yy` and `2yy + 1` are always identical, and the odd row's `color_table`
   is the one built on the even row of the same pair (`678-698`). Any rewrite must keep the
   per-row setup code (`627-707`) verbatim so this state carry-over is preserved.

---

## S0 - Build and test harness

### S0.1 Files

| Action | Path |
|---|---|
| New | `src/main/hwvideo/hwarch.hpp` - ISA macros, `DC ZVA` helper, NEON include |
| Edit | `src/main/hwvideo/hwroad.hpp`, `hwroad.cpp` - `_ref` twins, accessors, test friend |
| Edit | `src/main/hwvideo/hwtiles.hpp`, `hwtiles.cpp` - `_ref` twins, accessors, test friend |
| Edit | `src/main/video.cpp`, `video.hpp` - RAM dump hook, mask storage, debug checks |
| Edit | `src/main/main.cpp` - `-dumpram` and `-nozva` flags |
| Edit | `CMakeLists.txt` - options `RENDER_CHECKS`, `RAM_DUMP`; add `hwarch.hpp` to `src_hwvideo` (line 116-124) |
| New | `tools/road_tiles/test/road_tiles_test.cpp` - the driver |
| New | `tools/road_tiles/test/stubs/frontend/config.hpp` - minimal `Config` |
| New | `tools/road_tiles/test/CMakeLists.txt` - standalone project |
| New | `tools/road_tiles/test/golden/` - checked-in hashes (see S0.6) |

### S0.2 Macro scheme (`src/main/hwvideo/hwarch.hpp`)

One header, included by `hwroad.cpp`, `hwtiles.cpp` and the driver. It follows the pattern of
`tools/ntsc_bench/stage/WT/snes_ntsc_simd.h` (compile-time ISA detection, a force-scalar
override, every primitive present on every target).

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

// CB_FORCE_SCALAR: build every optimised path with its portable fallback (any host).
#if !defined(CB_FORCE_SCALAR)
  #if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    #define CB_HAVE_DCZVA 1              // inline asm needed: GCC/Clang AArch64 only
  #endif
  #if defined(__aarch64__) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
    #include <arm_neon.h>
    #define CB_HAVE_NEON 1               // AArch64 NEON dialect only (vzip1q etc.)
  #endif
#endif
#if defined(CB_HAVE_NEON)
  #define CB_ARCH_NAME "NEON"
#else
  #define CB_ARCH_NAME "scalar"
#endif
```

Rules: 32-bit ARM (ARMv6/ARMv7, `__ARM_NEON` under `-mfpu=neon`) is deliberately *not* given
NEON, because the tile kernel uses AArch64-only intrinsics; it takes the scalar path. MSVC ARM64
(`_M_ARM64`) is not a supported target and takes the scalar path. x86-64 takes the scalar path
for everything (no SSE2 port is specified; the scalar code is what MSVC ships).

The `DC ZVA` helper also lives here (full definition in S2.2):

```cpp
int  cb_dczva_probe(void);                       // 1 iff DC ZVA usable with 64-byte blocks
void cb_zva_lines(void* p, size_t bytes);        // zero every whole 64-byte line inside [p, p+bytes)
```

Both are `static inline`; on targets without `CB_HAVE_DCZVA` the probe returns 0 and the helper
is an empty function.

Test-only macros:

- `CB_RENDER_TEST` - compiles the `_ref` twins and declares `friend struct RoadTilesTestAccess;`
  in `HWRoad` and `hwtiles`. Defined only by the driver's CMake; never by the game.
- `CB_RENDER_CHECKS` - enables the S3 debug assertions in `Video::prepare_frame`. CMake option
  `RENDER_CHECKS` (default OFF) adds `-DCB_RENDER_CHECKS=1` to the game, alongside
  `PIXEL_ACCURACY` at `CMakeLists.txt:278-282`. The driver always defines it.
- `CB_RAM_DUMP` - compiles the `-dumpram` capture (S0.5). CMake option `RAM_DUMP`, default OFF.
- `CB_FRAME_CKSUM` - already exists (`video.cpp:227-237`, `main.cpp:636-649`); used as the
  whole-frame smoke test (S0.6).

### S0.3 `_ref` twins

Under `#ifdef CB_RENDER_TEST`, each function below is duplicated *verbatim* (copy the body as it
is on this branch before any section is implemented, rename, add nothing) as a private member:

| Original | Twin |
|---|---|
| `HWRoad::render_background_lores` (`hwroad.cpp:244`) | `render_background_lores_ref` |
| `HWRoad::render_foreground_lores` (`297`) | `render_foreground_lores_ref` |
| `HWRoad::render_background_hires` (`521`) | `render_background_hires_ref` |
| `HWRoad::render_foreground_hires` (`595`) | `render_foreground_hires_ref` |
| `hwtiles::render_tile_layer` (`hwtiles.cpp:212`) | `render_tile_layer_ref` |
| `hwtiles::render_text_layer` (`311`) | `render_text_layer_ref` |
| `hwtiles::render8x8_tile_mask_hires` (`458`) | `render8x8_tile_mask_hires_ref` |
| `hwtiles::render8x8_tile_mask_clip_hires` (`502`) | `render8x8_tile_mask_clip_hires_ref` |

The twins must call only twins (e.g. `render_tile_layer_ref` dispatches through a pair of
function-pointer members `render8x8_tile_mask_ref` / `render8x8_tile_mask_clip_ref`, set in
`hwtiles::init` next to `hwtiles.cpp:128-129`, so that the ref scan never calls a new kernel).
The function-local `static const` tables `priority_map` and `priority_lookup` (`hwroad.cpp:305-322`
and `608-625`) are moved to file scope in `hwroad.cpp` (identical contents, one copy) so the twins,
the new code and S6's kernel share them. This is the one edit to the copied bodies.

The twins take no `DC ZVA`, no window scan, no mask: they are the reference for every section.
They are not compiled into the game.

### S0.4 Test access and accessors

Add to both classes, under `CB_RENDER_TEST`: `friend struct RoadTilesTestAccess;`. The driver
defines that struct and uses it to reach `ramBuff`, `road_control`, `roads`, `x_clamp`,
`tile_banks`, the twins and the private kernels. This avoids `#define private public`.

Add unconditionally (needed by the game's `-dumpram`):

```cpp
// hwroad.hpp
const uint16_t* road_ram_buffer() const { return ramBuff; }   // 0x800 halfwords
uint8_t  get_road_control() const { return road_control; }
// hwtiles.hpp
int16_t  get_x_clamp() const { return x_clamp; }
uint8_t  get_tile_bank(int i) const { return tile_banks[i]; }
```

### S0.5 RAM capture (`-dumpram <dir> <interval> <count>`)

Under `CB_RAM_DUMP`, `main.cpp` parses `-dumpram` (next to `-perftest`, `main.cpp:813`) into
three globals; `Video::prepare_frame` (`video.cpp:177`), immediately after
`tile_layer->update_tile_values()` (line 196), writes `<dir>/ram_%06u.bin` every `interval`-th
frame until `count` files exist. Use an interval coprime with common loop lengths (e.g. 37) and
`count` of at least 300 across the attract cycle plus a recorded gameplay session. Format v1,
little-endian, fixed layout:

```
offset  size    field
0       8       magic "CBRAMD01"
8       2       s16_width   (config.s16_width, hi-res doubled, e.g. 640)
10      2       s16_height  (448 or 224)
12      2       s16_x_off   (0 or 42 in lo-res units; hi-res wide also 42 - see video.cpp:124)
14      1       hires (0/1)
15      1       widescreen (0/1)
16      1       road_control (hwroad.get_road_control(), low 3 bits meaningful)
17      1       fg_enabled  = (!config.engine.fix_bugs || oroad.horizon_base != ORoad::HORIZON_OFF)
18      2       x_clamp     (int16, tile_layer->get_x_clamp())
20      2       tile_banks[0..1]
22      2       reserved (0)
24      4       frame_no (video.prof_frames)
28      4096    ramBuff  (0x800 x uint16, hwroad.road_ram_buffer())
4124    65536   tile_ram (tile_layer->tile_ram, first 0x10000 bytes)
69660   4096    text_ram (tile_layer->text_ram, first 0x1000 bytes)
73756   end
```

`page[]`, `scroll_x[]`, `scroll_y[]` are not stored: they derive from `text_ram` via
`update_tile_values()` (`hwtiles.cpp:187-196`), which the driver calls after loading.

### S0.6 The driver (`tools/road_tiles/test/road_tiles_test.cpp`)

Standalone CMake project that compiles, unmodified, `src/main/hwvideo/hwroad.cpp` and
`src/main/hwvideo/hwtiles.cpp` plus the driver, with include directories in this order:
`tools/road_tiles/test/stubs` then `src/main`. The stub `frontend/config.hpp` shadows the real one
(`hwroad.cpp:4`, `hwtiles.cpp:5`) and contains exactly:

```cpp
#pragma once
#include "stdint.hpp"
struct Config { uint16_t s16_width, s16_height, s16_x_off; };
extern Config config;
```

(`hwroad.cpp` and `hwtiles.cpp` reference only `config.s16_width`, `config.s16_height`,
`config.s16_x_off`; grep before building - if a section adds another member, add it to the stub.)
The driver defines `Config config;`. The real `romloader.hpp` is used as-is (`read16`/`read32` are
inline; nothing links). Compile definitions: `CB_RENDER_TEST=1 CB_RENDER_CHECKS=1`. Two targets:
`road_tiles_test` (native ISA: `-O2`, plus `-mcpu=cortex-a53` on the Pi) and
`road_tiles_test_scalar` (adds `-DCB_FORCE_SCALAR=1`). Both must pass everywhere.

Inputs: `--roms <dir>` holding the raw files `opr-10185.11` (road, 32 KB; `decode_road` uses only
the first 0x8000 bytes for both roads, `hwroad.cpp:175`) and the six tile ROMs concatenated in
the order of `roms.cpp:52-57` into a 0x30000-byte image (`opr-10268.99 @0, opr-10232.102 @0x8000,
opr-10267.100 @0x10000, opr-10231.103 @0x18000, opr-10266.101 @0x20000, opr-10230.104 @0x28000`).
`--frames <dir>` holding `ram_*.bin` captures. `--seed N` for the random tests. Sub-commands
`unit` (all per-section unit tests), `replay` (captured frames), `bench` (S0.8), `golden`
(print per-frame hashes of the *ref* render for `golden/`).

Replay procedure, per captured frame, for each of `mode_set = {as captured}` and, when the
capture is hi-res non-wide, also the synthetic variants `{hi-res wide, lo-res, lo-res wide}` (the
RAM is the same; only `config`, `x_clamp` recomputed as `192 - s16_x_off`, and the `init(rom,
hires)` selection change):

1. Set `config`, call `hwtiles::init(tiles_rom, hires)` and `hwroad.init(road_rom, hires)` once
   per mode; load `ramBuff`, `road_control`, `tile_ram`, `text_ram`, `x_clamp`, `tile_banks` via
   `RoadTilesTestAccess`; `update_tile_values()`.
2. Allocate two buffers exactly as `video.cpp:67-72` (size `(s16_width * (s16_height + 2) + 64) *
   2` bytes, 64-aligned, `pixels = base + 64`), fill both with the same pseudo-random content
   (seeded; this catches any write outside the intended rows or any missed write).
3. Buffer A: `render_background_*_ref`, `render_tile_layer_ref(1)`, `render_tile_layer_ref(0)`,
   `if (fg_enabled) render_foreground_*_ref`, `render_text_layer_ref(1)`.
   Buffer B: the new sequence exactly as the modified `Video::prepare_frame` does it (mask
   derivation, `render_background`, tile layers with mask, foreground, text without mask).
   Sprites are omitted from both (unchanged code, drawn between foreground and text; omitting
   them in both pipelines does not affect the comparison).
4. `memcmp` the *entire allocation* (including the 64-halfword front slack and the two spare
   rows). Any difference: print frame number, mode, first differing (row, x), both values, and
   fail.

Golden hashes: `golden` prints FNV-1a 64 (`video.cpp:231-235` variant, over `s16_width *
s16_height` halfwords of buffer A) per frame per mode; check the output in as
`golden/<capture-set>.txt`. `replay` also recomputes the ref hash and fails if it differs from
the golden file, so an accidental edit to a `_ref` twin is caught.

Whole-game smoke (every section, final gate): build the game twice (before/after) with
`-DCB_FRAME_CKSUM=1`, run `CB_MAX_FRAMES=3000 ./cannonball-se -t 1 -perftest` from a cold start,
compare the printed `cksum`. Deterministic only with `-t 1 -perftest` (`main.cpp:637-640`).

### S0.7 Order and dependencies

```
S0 -> S1 (independent)
S0 -> S3 -> S2 -> S4        (S4 uses S2's helper and S3's predicate)
      S3 -> S5
S0 -> S6 (independent of S2-S5; S4's ZVA placement must survive it, see S6.3)
S0 -> S7 (independent)
```

Before any section: implement S0, build the driver on WSL x86-64, capture at least 300 frames
on the Pi or WSL with the *unmodified* game plus `-dumpram`, run `golden`, commit the golden
file. Every section ends with `unit`, `replay` (x86-64 scalar and Pi NEON, both driver targets)
and the checksum smoke.

### S0.8 Measurement

In-game numbers come from the existing 2-second report (`main.cpp:657-683`), which prints
ms/frame for `tilevals | road_bg | tile_bg | tile_fg | road_fg | SPRITES | text | TOTAL` from the
`prof_ns_*` accumulators in `video.hpp:48-56`. Procedure for every section: Pi Zero 2 W, shipping
config (hi-res, non-wide, Blargg filter with its threads running), attract loop, take the median
of the reports between 60 s and 180 s, before and after. The driver's `bench` sub-command times
the isolated functions (10,000 iterations over a captured frame, `steady_clock`, reports ns per
call and per row) for S6 and S7, and is run on the Pi with `-mcpu=cortex-a53`.

---

## S1 - Visible-window tilemap scan

### S1.1 Files and functions

`src/main/hwvideo/hwtiles.cpp`: `render_tile_layer` (212-309), `render_text_layer` (311-354).
No header change beyond S0.

### S1.2 Behaviour

**Tile layer.** Replace the 64 x 128 nest (`231-235`) with a two-stage scan. First, per call,
evaluate the *existing* coordinate expressions once per index and keep the accepted ones:

```cpp
int   xs[128]; uint8_t mxl[128]; int nmx = 0;
for (int mx = 0; mx < 128; mx++) {
    int x = mx * 8 - x_decrement;                // hwtiles.cpp:274,279
    if (x < -x_clamp) x += 1024;                 // 282-283
    if (x > -8 && x < s16_width_noscale) { xs[nmx] = x; mxl[nmx++] = (uint8_t)mx; }
}
int   ys[64];  uint8_t myl[64];  int nmy = 0;
for (int my = 0; my < 64; my++) {
    int y = my * 8 - y_decrement;                // 275,285
    if (y < -288) y += 512;                      // 289-290
    if (y > -8 && y < S16_HEIGHT) { ys[nmy] = y; myl[nmy++] = (uint8_t)my; }
}
```

The acceptance predicates are the union of the fast test (`302`) and the clip test (`304`),
which reduces to the clip test's x-part and y-part because the fast region is a subset of the
clip region in each axis (x: 8..W-9 inside -7..W-1; y: 8..216 inside -7..223), and because the
region is a product of per-axis intervals. Do not simplify further and do not hard-code ranges.

Second, iterate `for j in 0..nmy` (outer) and `for i in 0..nmx` (inner), with `my = myl[j]`,
`mx = mxl[i]`, `x = xs[i]`, `y = ys[j]`, and inside the body keep lines `238-272` and `292-305`
character for character (quad, `ActPage`, `TileIndex`, `Data`, priority test, `Code`, the
`Code == 0` continue, `Colour`, `ColourOff`, and the two-branch fast/clip dispatch, including the
now-redundant `else if` condition). Delete only the per-cell recomputation of `x`/`y`
(`274-290`) and the `my8`/`mx8` counters (`230, 233-234, 237`). `int16_t x, y` at line 214 may
become `int`; every value fits either type.

**Text layer.** Same two-stage form. Per-index predicates are the union of `345` and `347`:
`my` accepted iff `8*my >= 0 && 8*my < S16_HEIGHT` (so `my <= 27`); `mx` accepted iff
`8*mx - 192 > -8 && 8*mx - 192 < s16_width_noscale` (so `mx` in 24..63 for both W = 320 and
W = 404; derive it with the loop, do not hard-code). Replace the running `TileIndex += 2` (`351`)
with `TileIndex = (my * 64 + mx) * 2` computed per visited cell. Keep lines `323-348` otherwise
verbatim, including `x + config.s16_x_off` in the kernel calls.

Iteration order of visited cells is unchanged (ascending `my`, then ascending `mx`), so even the
write order is identical.

### S1.3 Must not change

- The cell body (page select, `TileIndex` formula, priority bit `Data >> 15`, bank remap
  `264`, `Code &= NUM_TILES - 1`, `Colour = (Data >> 6) & 0x7f`, `ColourOff`).
- The fast/clip dispatch conditions and which kernel is called.
- Widescreen: `x_clamp` may be 192, 150 or 108 (`set_x_clamp`, `171-185`) and
  `s16_width_noscale` 320 or 404; the predicate loop handles all because it evaluates the
  original expressions.
- No state crosses cells in the original (`ActPage`, `Priority`, `TileIndex` are per-cell), so
  none may be introduced.

### S1.4 Test

`unit s1`: for every `x_decrement` in 0..1023, `y_decrement` in 0..511, `x_clamp` in
{192, 150, 108}, `W` in {320, 404} (that is 1024 x 512 x 3 x 2 = 3.1M cases), compute the set of
`(mx, my)` accepted by the original per-cell test (`x > -8 && x < W && y > -8 && y < 224` after
the same wrap adjustments) and the set produced by the two-stage precompute; they must be equal
as ordered lists (same order of visitation). Same for the text layer over `W` in {320, 404}.
Then `replay`: `render_tile_layer` vs `render_tile_layer_ref` and `render_text_layer` vs
`_ref`, all captured frames, all four modes; pass = whole-buffer `memcmp` equal.

### S1.5 Measure

`prof_ns_tile_bg`, `prof_ns_tile_fg`, `prof_ns_text`. Expected on the Pi under load: tile_bg
0.43 to about 0.30 ms, tile_fg 0.20 to about 0.09 ms.

### S1.6 Dependencies

S0 only. Land first; nothing depends on it, but S5 edits the same loop and is easier on top of it.

---

## S2 - `DC ZVA` line pre-allocation, road background only

### S2.1 Files and functions

`src/main/hwvideo/hwarch.hpp` (helper), `hwroad.hpp` (state), `hwroad.cpp`: `init` (125-146),
`render_background_lores` (283-291), `render_background_hires` (580-587). `main.cpp`: `-nozva`.

### S2.2 Behaviour

Helper, in `hwarch.hpp`:

```cpp
#if defined(CB_HAVE_DCZVA)
static inline int cb_dczva_probe(void) {
    uint64_t dczid;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));      // EL0-readable
    const int    prohibited = (int)((dczid >> 4) & 1u);         // DZP bit
    const size_t block      = (size_t)4 << (dczid & 0xFu);      // bytes
    return (!prohibited && block == 64) ? 1 : 0;
}
static inline void cb_zva_lines(void* p, size_t bytes) {
    uintptr_t a   = ((uintptr_t)p + 63u) & ~(uintptr_t)63u;    // first whole line at or after p
    uintptr_t end = ((uintptr_t)p + bytes) & ~(uintptr_t)63u;  // end of last whole line inside
    for (; a < end; a += 64)
        __asm__ volatile("dc zva, %0" : : "r"(a) : "memory");
}
#else
static inline int  cb_dczva_probe(void)            { return 0; }
static inline void cb_zva_lines(void*, size_t)     { }
#endif
```

Contract of `cb_zva_lines`: zero exactly the 64-byte lines wholly contained in `[p, p+bytes)`,
touch nothing else; the caller must overwrite every byte of `[p, p+bytes)` before anything reads
the buffer. No `DSB`/`DMB` is needed: `DC ZVA` orders as a store to the block, and the caller's
subsequent stores to the same addresses are ordered after it on the same thread. The `"memory"`
clobber stops the compiler reordering the caller's stores above it. Block size other than 64 is
treated as unavailable (the A53 reports 64; a different size would break the "whole lines inside
the row" reasoning for widescreen pitches).

State, in `HWRoad`: `bool dczva_on = false;` and `bool dczva_disabled = false;`, public
`void set_dczva(bool enable)` which sets `dczva_disabled = !enable` and recomputes
`dczva_on = !dczva_disabled && cb_dczva_probe()`. `init` (`hwroad.cpp:125`) does the same
recomputation. `main.cpp`: flag `-nozva` calls `hwroad.set_dczva(false)` before `video.init`.

Placement, hi-res (`hwroad.cpp:580-587`): inside `if (color != -1)`, after `pPixel` is computed
and before the `std::fill_n`:

```cpp
if (dczva_on) cb_zva_lines(pPixel, (size_t)config.s16_width * 4u);   // rows y and y+1, contiguous
```

Lo-res (`283-291`): `if (dczva_on) cb_zva_lines(pPixel, (size_t)w * 2u);` before the fill.

The fill statements themselves are unchanged. The zeroed lines are written immediately by the
same fill, so the buffer's observable content is unchanged.

### S2.3 Must not change

- The sky-colour switch (`534-559` / `256-281`) and the fill values.
- Rows not filled by the background must not be touched: the call sits inside `color != -1`.
- Widescreen: pitch 1616 (hi-res) / 808 (lo-res) is not line-aligned; the helper's rounding
  handles it (24 or 25 whole lines per hi-res wide row are zeroed, the partial end lines are
  written normally). Never round outward.
- The `+2` spare rows (`video.cpp:67`) must never be zeroed: the byte range passed is always
  exactly the rows the fill writes.
- Without `CB_HAVE_DCZVA` (x86-64, ARMv7, `CB_FORCE_SCALAR`) the code compiles to the original.

### S2.4 Test

`unit s2a` (helper, all targets): a 4096-byte buffer, 64-aligned; for `off` in 0..127 and `len`
in 0..1024: fill with 0xFF, call `cb_zva_lines(buf + off, len)`, verify byte `k` is zero iff its
line `[k & ~63, +64)` lies wholly inside `[off, off + len)`, else 0xFF. On non-ZVA targets verify
nothing changes. `unit s2b`: two identical random-filled frame buffers; `render_background_*_ref`
on A, `render_background_*` with `dczva_on` forced 1 (via the friend struct) on B, for every
captured frame, all four modes, and both `road_control & 3` sets seen; whole-allocation
`memcmp`. Run on the Pi (real ZVA) and on x86-64 (no-op path).

### S2.5 Measure

`prof_ns_road_bg`, A/B with and without `-nozva`, plus the standalone microbenchmark from
`ROAD_TILE_RENDERER.md` section 1 (this result is supplied separately). Kill rule: `DCZID_EL0` DZP set, loaded fill improves by less than 1.5x, or in-game
`road_bg` stays above 0.40 ms. If killed, S2 ships as dead code with `dczva_on` forced false
(keep the helper: S4 is then also off), or is reverted.

### S2.6 Dependencies

S0. S3 should be in place first only because its checks run alongside; no code dependency.

---

## S3 - Road-foreground full-row invariant and overwrite-mask derivation

### S3.1 Files and functions

`hwroad.hpp` (new inline predicates and two methods), `hwroad.cpp` (their bodies), `video.hpp`
(mask storage), `video.cpp`: `prepare_frame` (177-238).

### S3.2 Behaviour

Predicates, in `hwroad.hpp`, used by S3, S4 and the checks so they cannot drift:

```cpp
// True iff the foreground pass writes lo-res line yy (hi-res rows 2yy, 2yy+1) given the two
// road RAM words and the control mode.  Mirrors hwroad.cpp:631-635, 715-716, 776-777.
static inline bool road_row_written(unsigned mode, unsigned d0, unsigned d1) {
    const bool s0 = (d0 & 0x800u) != 0, s1 = (d1 & 0x800u) != 0;
    switch (mode & 3u) { case 0: return !s0; case 3: return !s1; default: return !(s0 && s1); }
}
// True iff the background pass fills the line.  Mirrors hwroad.cpp:534-559.
static inline bool road_row_sky(unsigned mode, unsigned d0, unsigned d1) {
    const bool s0 = (d0 & 0x800u) != 0, s1 = (d1 & 0x800u) != 0;
    switch (mode & 3u) { case 0: return s0; case 3: return s1; default: return s0 || s1; }
}
```

Mask method:

```cpp
// out[yy] = 1 iff the foreground will write lo-res line yy this frame; all 0 if !fg_enabled.
void HWRoad::foreground_row_mask(uint8_t out[S16_HEIGHT], bool fg_enabled) const {
    for (int yy = 0; yy < S16_HEIGHT; yy++)
        out[yy] = fg_enabled && road_row_written(road_control & 3, ramBuff[yy], ramBuff[0x100 + yy]);
}
```

Caller, `video.cpp:196-209` becomes:

```cpp
tile_layer->update_tile_values();
const bool fg_enabled = !config.engine.fix_bugs || oroad.horizon_base != ORoad::HORIZON_OFF;
hwroad.foreground_row_mask(road_fg_rows, fg_enabled);          // Video member uint8_t road_fg_rows[S16_HEIGHT]
...
if (fg_enabled) (hwroad.*hwroad.render_foreground)(pixels);    // same variable, replaces the inline test at 208
```

Road RAM cannot change between the mask and the foreground call: `ORoad::do_road`
(`oroad.cpp:193-207`) runs in `tick()` before `prepare_frame` on the same thread.

Check method (compiled always, called only under `CB_RENDER_CHECKS` and by the driver):

```cpp
// Renders the foreground into `scratch` (same size/alignment as `pixels`, pre-filled with
// 0xFFFF, a value no road colour can take: color_offset1/2 are 0x400/0x420) and verifies, per
// lo-res line, that every hi-res row of the line is written in full iff mask[yy] == 1 and not
// touched at all iff mask[yy] == 0; also verifies road_row_sky || road_row_written for every
// line (coverage).  Returns the number of violating lines and prints the first violation
// (mode, yy, pixels written) to stderr.  `rows_both` receives the count of lines where the
// background also fills (statistic, not a failure).
int HWRoad::check_foreground_rows(const uint8_t mask[S16_HEIGHT], bool fg_enabled,
                                  uint16_t* scratch, int* rows_both);
```

`rows_per_line` is 2 in hi-res (`config.s16_height == 2 * S16_HEIGHT`) and 1 in lo-res. In
`prepare_frame` under `CB_RENDER_CHECKS`, after the foreground call: allocate the scratch once
(static, `video.cpp:67-72` formula), call the check, `abort()` on non-zero.

### S3.3 Must not change

- The foreground and background functions are not edited by S3 (only their predicates are
  restated). The mask is derived from `ramBuff` (the buffer read by the renderers, not `ram`).
- `fg_enabled` must be computed once and used for both the mask and the call.
- The text layer and sprites never see the mask (see S5.3).

### S3.4 Test

`unit s3`: for every `mode` in 0..3, every `(s0, s1)` in {0,1}^2 and both `road_control & 4`
values, build a `ramBuff` with 224 lines cycling through those combinations (lines also carry
random line-select bits and random h-scroll/colour tables), and for both hi-res and lo-res
`init`: `check_foreground_rows` must return 0 with `fg_enabled` true and with it false (mask all
zero, foreground not rendered). Also assert the closed-form expectations: mode 0 written iff
`!s0`, mode 3 iff `!s1`, modes 1/2 iff `!(s0 && s1)`. `replay`: run the check on every captured
frame in all four modes; must return 0 everywhere; print the `rows_both` total (expected 0 for
OutRun data; non-zero is information, not failure).

### S3.5 Measure

No performance effect intended; `prof_ns_tilevals` should be unchanged within noise (224
iterations). The check costs a full foreground render and is debug-only.

### S3.6 Dependencies

S0. Gate for S4 and S5: neither may be merged until `replay` reports 0 violations over the full
capture set on both hosts.

---

## S4 - `DC ZVA` for the road foreground rows

### S4.1 Files and functions

`hwroad.cpp`: `render_foreground_hires` (595-789), `render_foreground_lores` (297-516).

### S4.2 Behaviour

Hi-res: immediately after the both-solid skip (`631-635`), before any modification of
`data0`/`data1`, capture the predicate from the raw words:

```cpp
const bool row_written = road_row_written(road_control & 3, data0, data1);
```

Then immediately before `switch (road_control & 3)` at line 711 (after `pPixel` is computed at
707):

```cpp
if (dczva_on && row_written) cb_zva_lines(pPixel, (size_t)config.s16_width * 2u);   // one row
```

Lo-res: the same two statements, the predicate after line 329 and the call before the switch at
368, with `(size_t)config.s16_width * 2u` (one row).

The `continue` statements inside cases 0 and 3 (`715-716`, `776-777`, and `371-372`, `490-491`)
stay; `row_written` is false in exactly those situations, so no zeroed row is left unwritten.
Nothing else in the functions changes.

### S4.3 Must not change

- The zero must never precede a `continue`: that is why the guard is the predicate, not the case
  bodies. If an implementer prefers to place the call inside each case body after its guard,
  that is equivalent, but the single guarded call is the specified form.
- `row_written` must be evaluated on the raw `data0`/`data1` (before `648-674`), even though the
  interpolation block cannot change the 0x800 bit; this keeps the predicate obviously identical
  to the mask's.
- The odd hi-res row is zeroed on its own iteration (one row per call); the pair invariant is not
  relied upon.
- All modes, both `road_control & 4` values, widescreen and lo-res behave as in S2.3.

### S4.4 Test

`unit s4`: identical to `unit s2b` but for the foreground functions, plus synthetic `ramBuff`
sets that exercise every branch: mode 0 with `s0` set and `s1` clear (row skipped by `continue`
after the ZVA guard must be false), mode 3 mirror, modes 1/2 with exactly one solid road, the
`yy == 223` odd row (no interpolation branch), and `fg_enabled` false (function not called).
Two detectors cover the failure mode "row zeroed but then not written": the `_ref` comparison
above (the ref leaves the row's random pre-fill intact, the new code would leave zeros), and the
S3 check on the Pi (its 0xFFFF sentinel turns to 0x0000 in such a row, which the check counts as
written pixels in a row the mask says is untouched). Both must pass on the ZVA-enabled build.

### S4.5 Measure

`prof_ns_road_fg`, A/B via `-nozva`. Expected 0.88 to about 0.70 ms before S6; the main value of
S4 shows after S6 (see S6.5).

### S4.6 Dependencies

S2 (helper and `dczva_on`), S3 (predicate and passing checks). Must be re-verified after S6.

---

## S5 - Clip the tile layers to rows the road foreground will overwrite

### S5.1 Files and functions

`hwtiles.hpp`/`.cpp`: `render_tile_layer` signature and the S1 `my` precompute. `video.cpp:202,
205` (call sites). `render_text_layer` is **not** changed.

### S5.2 Behaviour

Signature: `void render_tile_layer(uint16_t* buf, uint8_t page_index, uint8_t priority_draw,
const uint8_t* dead_rows);` where `dead_rows` is `Video::road_fg_rows` (224 entries, lo-res
lines) or `nullptr` (no clipping; the `_ref` twin keeps the old three-argument form).

In the S1 `my` precompute, after computing `y` for an accepted `my`, compute whether every
on-screen row of the tile is dead and, if so, do not add the `my` to the list:

```cpp
if (dead_rows) {
    const int r0 = y < 0 ? 0 : y;
    const int r1 = (y + 8 > S16_HEIGHT) ? S16_HEIGHT : y + 8;   // r0 < r1 guaranteed by acceptance
    bool dead = true;
    for (int r = r0; r < r1; r++) if (!dead_rows[r]) { dead = false; break; }
    if (dead) continue;
}
```

Partially dead tiles are rendered exactly as before (no per-row skipping inside the kernels;
that refinement is explicitly out of scope for this pass). Call sites in `video.cpp`:
`render_tile_layer(pixels, 1, 0, road_fg_rows)` and `render_tile_layer(pixels, 0, 0,
road_fg_rows)`; `render_text_layer(pixels, 1)` unchanged.

### S5.3 Must not change

- The text layer is drawn *after* the road foreground (`video.cpp:214`); applying the mask to it
  would delete the HUD. It must never receive `dead_rows`.
- With `fg_enabled` false the mask is all zero and nothing is skipped (horizon-off state).
- The mask is in lo-res lines; tile `y` is in lo-res lines in both resolutions (`hwtiles.cpp:470`
  doubles it for hi-res). Do not double or halve either.
- Off-screen rows of a clip-path tile do not count toward "dead" (they are never drawn), so the
  loop is clamped to `[0, S16_HEIGHT)`.
- Whether a skipped tile would have used the fast or clip kernel is irrelevant.

### S5.4 Test

`replay` compares the *post-foreground* buffer, so a wrongly skipped tile shows as a difference
only if the road does not overwrite it, which is exactly the bug class. Additionally `unit s5`:
for 2,000 random synthetic states (random mask with runs, random scrolls, random tile RAM with
30% non-zero codes, all four `x_clamp`/`W` combinations, hi-res and lo-res) render tile layer
with mask into B and `_ref` into A, then apply a synthetic "foreground" that overwrites every
masked row with a constant in both A and B, then `memcmp`. Must be equal. Also a negative
control: pass the mask to the text layer in the driver and confirm the test *fails* (proves the
test can see the bug).

### S5.5 Measure

`prof_ns_tile_bg`, `prof_ns_tile_fg`. Expected small (about 0.05 ms) plus the removal of 372
first-touch line fills that the road later zeroes anyway once S4 is in.

### S5.6 Dependencies

S1 (edits the same loop), S3 (mask and passing checks).

---

## S6 - Run-length road foreground (hi-res)

### S6.1 Files and functions

`hwroad.hpp` (run table, kernel declaration), `hwroad.cpp`: `decode_road` (171-194, table build
appended), `render_foreground_hires` (the four case bodies `713-786` only). The lo-res foreground
is **not** converted (out of scope; it keeps the scalar loops and S4's ZVA).

### S6.2 Behaviour

**Run table**, built at the end of `decode_road` (the only writer of `roads[]`; if any later code
ever writes `roads[]`, the table must be rebuilt). 513 lines (`0..255` road 0, `256..511` road 1,
`512` dummy all-3 line; `roads` is `0x40200 = 513 * 512` bytes, `hwroad.hpp:42`):

```cpp
static const int ROAD_LINES = 513;
std::vector<uint16_t> run_tab;        // entry = start (bits 0-9, 0..512) | value << 12 (bits 12-14)
uint32_t run_first[ROAD_LINES + 1];   // run_tab index of line L's first entry; [513] = total
```

For each line, emit one entry per maximal run of equal byte value (values are 0,1,2,3,7; store
the 3-bit value as found), then a sentinel entry with `start = 512, value = 0`. Sizing is exact
(two passes or `push_back`); the OutRun ROM yields about 8.2K entries (16 KB), an adversarial
ROM at most 513 x 513 entries, which is why offsets are 32-bit. `static_assert(S16_WIDTH_WIDE <=
512)` because the visible-window argument below needs `pairs <= 0x200`.

**Row kernel**, private member, called from the four cases:

```cpp
// out32: row start; pairs = config.s16_width >> 1 (320 or 404); mode = road_control & 3;
// src0/src1: the line base pointers computed by the unchanged setup (hwroad.cpp:662,671,701,703);
// t0/t1: (hposN - (s16_x + x_offset)) & 0xfff exactly as at 717/732-733/756-757/778.
void HWRoad::render_row_runs(uint32_t* out32, int pairs, unsigned mode,
                             const uint8_t* src0, const uint8_t* src1,
                             uint32_t t0, uint32_t t1, const uint16_t* color_table);
```

Definition of the reference semantics it must reproduce (`736-751`): for pair index `i` in
`[0, pairs)`, `tN(i) = (tN + i) & 0xfff`, `vN(i) = tN(i) < 0x200 ? srcN[tN(i)] : 3`, colour
`c(i) = select(mode, v0(i), v1(i))`, output word `c | c << 16` at `out32[i]`, where

```
select(0, v0, v1) = color_table[v0]
select(1, v0, v1) = priority_lookup[v0][v1] ? color_table[0x10 + v1] : color_table[v0]
select(2, v0, v1) = ((priority_map[1][v0] >> v1) & 1) ? color_table[0x10 + v1] : color_table[v0]
select(3, v0, v1) = color_table[0x10 + v1]
```

(Modes 2 and 3 in the reference loop per pixel with `hpos` advancing on odd `x`, `754-786`; that
is the same per-pair function.)

Visible window per road. Because `pairs <= 404 < 0x200`, the set of `i` with `tN(i) < 0x200` is
one contiguous interval `[a, b)` with texel index `tex + (i - a)`:

```
if (t < 0x200)        { a = 0;          b = min(pairs, 0x200 - t);  tex = t; }
else if (0x1000 - t < pairs) { a = 0x1000 - t; b = pairs;            tex = 0; }   // wraps to texel 0
else                  { a = b = 0; }                                             // never visible
```

In mode 0 force road 1 to `a = b = 0` (its value is ignored by `select`, so its breakpoints would
only fragment the fill); in mode 3 force road 0 likewise.

Merge. Each road contributes breakpoints at `a`, `b`, and `a + (s - tex)` for every run start
`s` in `(tex, tex + (b - a))`. Walk both roads with a cursor:

```
i = 0; for each road r: state_r = (v_r = 3, next_r = pairs), then if a_r == 0 enter(r)
while i < pairs:
    n = min(next_0, next_1, pairs)                 // n > i always
    c = select(mode, v_0, v_1)
    fill_pairs(out32 + i, n - i, (uint32_t)c | ((uint32_t)c << 16))
    i = n
    for each road r with next_r == i: advance(r)

enter(r):   locate run k of line L_r containing texel tex_r (linear scan from run_first[L_r];
            L_r = (src_r - roads) / 512);  v_r = value(k);
            next_r = min(b_r, a_r + (start(k+1) - tex_r))
advance(r): if i == b_r: v_r = 3; next_r = pairs
            else if i == a_r: enter(r)
            else: k++; v_r = value(k); next_r = min(b_r, a_r + (start(k+1) - tex_r))
```

`start(k+1)` of the last run is the sentinel 512, so `next_r` never exceeds `b_r`. Adjacent
segments with equal colour may be coalesced or not; the output is identical either way.

`fill_pairs(uint32_t* p, int n, uint32_t w)`: under `CB_HAVE_NEON`, `uint32x4_t v =
vdupq_n_u32(w); while (n >= 4) { vst1q_u32(p, v); p += 4; n -= 4; } while (n-- > 0) *p++ = w;`
(unaligned `vst1q` is fine on AArch64; `p` is only 4-byte aligned). Otherwise `std::fill_n(p, n,
w)`. Unrolling to two `vst1q` per iteration for `n >= 8` is permitted and tunable.

**Call sites.** Replace each case body with the guard (cases 0 and 3 keep their `continue`),
then one call. For example case 1 (`729-753`) becomes:

```cpp
case 1:
    render_row_runs(reinterpret_cast<uint32_t*>(pPixel), config.s16_width >> 1, 1, src0, src1,
                    (hpos0 - (s16_x + x_offset)) & 0xfff, (hpos1 - (s16_x + x_offset)) & 0xfff,
                    color_table);
    break;
```

Keep the operand expressions and types exactly (`hpos0` is `int32_t`, `s16_x` is `uint16_t`,
`x_offset` is `int32_t`, the `& 0xfff` yields a non-negative `int`).

### S6.3 Must not change

- Lines `597-707` (setup, skip, interpolation, `color_table`, `src` fallback, `s16_x`, `pPixel`)
  are untouched; the odd-row `color_table` carry-over (facts, item 2) depends on it.
- S4's `row_written` capture and the guarded `cb_zva_lines` before the switch stay where S4 put
  them.
- Values 4, 5, 6 do not occur in `roads[]` but the kernel must not assume it: any 3-bit value
  must index the tables exactly as the reference does.
- Widescreen: `pairs = 404`, `s16_x = 0x5f8 + 42`; the window argument holds. Lo-res is untouched.
- `x_offset` is always 0 (`hwroad.cpp:131`, never written elsewhere) but must remain in the
  expression.

### S6.4 Test

All comparisons are `render_foreground_hires` vs `render_foreground_hires_ref` on two identical
random-filled full buffers, whole-allocation `memcmp`, for `s16_width` in {640, 808}.

- `unit s6a` (exhaustive single-road, direct mode): `road_control = 4 | mode` makes each line
  `yy` index its own h-scroll word (`roadram[0x200 + yy]`, `hwroad.cpp:640`), so one call tests
  224 independent `(line, hscroll)` pairs. Sweep line select `L` in 0..255 plus solid (dummy line)
  and `hscroll` in 0..4095 for road 0 in modes 0, 1, 2 (road 1 held at a random `(L, h)` per
  call) and for road 1 in modes 1, 2, 3 (road 0 random): 257 x 4096 / 224 = about 4,700 calls per
  mode. The odd rows exercise the interpolation between consecutive lines for free.
- `unit s6b` (indirect mode, random): `road_control = mode`, 20,000 calls per mode with fully
  random road RAM (`data` words random 16-bit so line select, 0x800 and 0x200 bits and the
  0x1ff index vary; h-scroll and colour tables random).
- `unit s6c` (relative offset sweep): mode 1 and 2, road 1 h-scroll = road 0 h-scroll + `d` for
  every `d` in -600..600, random lines, 3 calls per `d`. This walks every merge-boundary
  configuration.
- `unit s6d` (synthetic ROMs): re-run s6b with 500 calls per mode after `init` on (i) a random
  32 KB ROM image, (ii) alternating-pixel bytes 0x55/0xAA in both planes (512 runs per line,
  maximum table size), (iii) all-zero, (iv) all-0xFF. `decode_road` applies the stripe-area
  `|= 4` (`hwroad.cpp:184-185`) so value 7 appears in all of them.
- `replay` on all captured frames.

Pass criterion: zero differing bytes in every case. Run on WSL x86-64 (scalar `fill_pairs`) and
on the Pi (NEON `fill_pairs`), both driver targets.

### S6.5 Measure

`prof_ns_road_fg` in game (expected 0.88 to about 0.20 ms with S4 in place, about 0.3 ms without
it) and `bench fg` in the driver: ns/row for `_ref` vs new over a captured driving frame.

### S6.6 Dependencies

S0. Independent of S2-S5 in code, but S4's ZVA guard must be preserved through the edit and S3's
check re-run afterwards. Recommended after S4 so the in-game number reflects both.

---

## S7 - NEON tile-row kernel with scalar fallback

### S7.1 Files and functions

`hwtiles.cpp`: `render8x8_tile_mask_hires` (458-500) and `set_pixel_x4` (452-455). The clip
variant (`502-548`) and both lo-res kernels are unchanged. The kernel is shared by the tile and
text layers automatically because both dispatch through `render8x8_tile_mask` (`hwtiles.hpp:52`).

### S7.2 Behaviour

Introduce a file-scope `static inline void tile_row_hires(uint16_t* buf, uint32_t p0, uint32_t
nPalette, uint16_t width)` that writes one tile row (16 hi-res pixels on rows `buf` and `buf +
width`), and make `render8x8_tile_mask_hires` call it inside the existing `if (p0 !=
nMaskColour)` test (`477`) with everything else in the function unchanged (`nPalette` at 468,
`buf` at 470, the `buf += width << 1; pTileData++` advance at 497-498).

Reference semantics (`479-495`): nibble `c_k` for `k` in 0..7 is bits `28 - 4k .. 31 - 4k` of
`p0` (`c0` is the leftmost pixel); if `c_k != 0`, the halfword `(uint16_t)(nPalette + c_k)` is
written to `buf[2k]`, `buf[2k+1]`, `buf[2k+width]`, `buf[2k+1+width]`; otherwise those four
positions are left as they are.

NEON path (`CB_HAVE_NEON`):

```cpp
const uint32_t sw = __builtin_bswap32(p0);                     // bytes: c0c1, c2c3, c4c5, c6c7
uint8x8_t  b   = vreinterpret_u8_u32(vdup_n_u32(sw));           // lanes 0..3 hold those bytes
uint8x8_t  hi  = vshr_n_u8(b, 4);                               // c0 c2 c4 c6 ..
uint8x8_t  lo  = vand_u8(b, vdup_n_u8(0x0F));                   // c1 c3 c5 c7 ..
uint8x8_t  nib = vzip1_u8(hi, lo);                              // c0 c1 c2 c3 c4 c5 c6 c7
uint16x8_t n16 = vmovl_u8(nib);
uint16x8_t col = vaddq_u16(n16, vdupq_n_u16((uint16_t)nPalette));
uint16x8_t L   = vzip1q_u16(col, col);                          // c0 c0 c1 c1 c2 c2 c3 c3
uint16x8_t R   = vzip2q_u16(col, col);                          // c4 c4 .. c7 c7
if (((((p0 & 0x77777777u) + 0x77777777u) | p0) & 0x88888888u) == 0x88888888u) {   // all 8 nibbles non-zero
    vst1q_u16(buf, L); vst1q_u16(buf + 8, R);
    vst1q_u16(buf + width, L); vst1q_u16(buf + width + 8, R);
} else {
    uint16x8_t m  = vtstq_u16(n16, n16);                        // 0xFFFF where nibble != 0
    uint16x8_t mL = vzip1q_u16(m, m), mR = vzip2q_u16(m, m);
    uint16_t* r1 = buf + width;
    vst1q_u16(buf,     vbslq_u16(mL, L, vld1q_u16(buf)));
    vst1q_u16(buf + 8, vbslq_u16(mR, R, vld1q_u16(buf + 8)));
    vst1q_u16(r1,      vbslq_u16(mL, L, vld1q_u16(r1)));
    vst1q_u16(r1 + 8,  vbslq_u16(mR, R, vld1q_u16(r1 + 8)));
}
```

The opacity test: per nibble `n`, `(n & 7) + 7` sets bit 3 iff `n & 7 != 0` without carrying
into the next nibble; OR-ing `p0` back in covers `n == 8`; so bit 3 of every nibble is set iff
every nibble is non-zero. The additions are 16-bit, so `nPalette + c_k` cannot differ from the
scalar `uint16_t` store (`nPalette <= 0x3F8` for tiles, `<= 0x38` for text). The mixed-row path
reads the destination and blends; it writes back the unchanged halfwords with their existing
values, which is observably identical to not writing them (single thread, nothing else reads the
buffer during `prepare_frame`).

Scalar path (`!CB_HAVE_NEON`): the current body of the `if` at `477-496` verbatim, using
`set_pixel_x4`. This is what MSVC ships.

### S7.3 Must not change

- Nibble order: `c0 = p0 >> 28` is the leftmost pixel. The `bswap` + `vzip1_u8(hi, lo)` sequence
  above yields lane order `c0..c7`; any other arrangement (for example `vzip1_u8(lo, hi)`, or
  omitting the swap) is wrong and the exhaustive test will catch it.
- Transparent test is nibble `== 0` regardless of `nMaskColour`; the whole-word test at `477`
  compares against `nMaskColour` (always 0 from the callers). Keep both as they are.
- `buf` is only 4-byte aligned (`2 * StartX` halfwords); use unaligned `vld1q`/`vst1q` only.
- The clip kernel (`502-548`) stays scalar and untouched; it handles screen-edge tiles, including
  the y-clip at `520` and per-pixel x-clip at `535-542`.
- Lo-res kernels untouched.

### S7.4 Test

`unit s7`: a scratch of 4 rows x 32 halfwords, `width = 32`, kernel target at `scratch + 32 +
8` (guard row above and below, guard columns left and right). For every `p0` in the set
{all 65,536 words of `tiles[]` from the real ROM} ∪ {all 65,536 words with each nibble in
{0, 1, 8, 15}} ∪ {100,000 seeded random words} and every `nTilePalette` in 0..127: fill the
scratch with seeded random halfwords, copy it, call `render8x8_tile_mask_hires_ref` on one and
the new function on the other with a one-row tile (drive the kernel through a temporary 8-row
tile whose other seven rows are zero, or call `tile_row_hires` directly through the friend struct
and the scalar reference row through `_ref` on a synthetic tile), `memcmp` all 128 halfwords.
About 30M kernel invocations; seconds. Run on the Pi (NEON) and x86-64 (scalar, trivially
equal). Then `replay`.

### S7.5 Measure

`prof_ns_tile_bg`, `prof_ns_tile_fg`, `prof_ns_text` (text is where opaque rows dominate on the
high-score screens); driver `bench tiles`: ns per opaque row and per mixed row, ref vs new.
Expected 0.05-0.1 ms per frame.

### S7.6 Dependencies

S0 only. Independent of S1-S6; the `_ref` twin of the scan must dispatch to the `_ref` kernel
(S0.3) so that the reference is unaffected.

---

## Appendix A - Summary of new and changed symbols

| Symbol | Where | Section |
|---|---|---|
| `CB_HAVE_NEON`, `CB_HAVE_DCZVA`, `CB_FORCE_SCALAR`, `cb_dczva_probe`, `cb_zva_lines` | `hwarch.hpp` | S0, S2 |
| `CB_RENDER_TEST`, `CB_RENDER_CHECKS`, `CB_RAM_DUMP` | CMake / driver | S0 |
| `RoadTilesTestAccess` (friend) | `hwroad.hpp`, `hwtiles.hpp` | S0 |
| `HWRoad::road_ram_buffer`, `get_road_control`; `hwtiles::get_x_clamp`, `get_tile_bank` | headers | S0 |
| `HWRoad::set_dczva`, `dczva_on`, `dczva_disabled`; `-nozva` | `hwroad.*`, `main.cpp` | S2 |
| `road_row_written`, `road_row_sky`, `HWRoad::foreground_row_mask`, `check_foreground_rows`, `Video::road_fg_rows` | `hwroad.*`, `video.*` | S3 |
| `hwtiles::render_tile_layer(buf, page, prio, dead_rows)` | `hwtiles.*`, `video.cpp` | S5 |
| `HWRoad::run_tab`, `run_first`, `render_row_runs`, `fill_pairs` | `hwroad.*` | S6 |
| `tile_row_hires` | `hwtiles.cpp` | S7 |

## Appendix B - Edge-case checklist (tick every box per section before merging)

- [ ] hi-res non-wide (640 x 448), hi-res wide (808 x 448), lo-res (320 x 224), lo-res wide (404 x 224)
- [ ] road control modes 0, 1, 2, 3, each with and without bit 2 (direct scanline mode)
- [ ] rows with exactly one solid road in modes 1 and 2 (background and foreground both write)
- [ ] `fg_enabled` false (horizon off with `fix_bugs`): no mask, no foreground, no tile clipping
- [ ] hi-res odd rows: interpolated `src`/`hpos`, `color_table` carried from the even row; `yy == 223`
- [ ] h-scroll windows that wrap 0xfff to 0 and windows that straddle 0x200
- [ ] tiles at all four screen edges (clip kernel) and tiles fully off-screen after the 1024/512 wrap
- [ ] `x_clamp` 192 / 150 / 108
- [ ] text layer never clipped by the road mask
- [ ] non-line-aligned pitch (widescreen): ZVA touches only whole lines inside the row
- [ ] `CB_FORCE_SCALAR` build of the driver passes on the Pi; MSVC x86-64 game build compiles
