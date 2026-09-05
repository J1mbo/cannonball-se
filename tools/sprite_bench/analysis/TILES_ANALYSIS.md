# Tilemap renderer cost model (Cortex-A53 target)

Branch `sprite-a53`. Primary source: `src/main/hwvideo/hwtiles.cpp` (549 lines)
and `src/main/hwvideo/hwtiles.hpp` (113 lines), read in full. Caller:
`src/main/video.cpp` `Video::prepare_frame()` (`src/main/video.cpp:177-225`).
Target config: hi-res on, widescreen off — `config.s16_width` = 640,
`config.s16_height` = 448 (`src/main/globals.hpp:32-37`, doubled at
`src/main/video.cpp:135-138`), 286,720 destination pixels.

## 1. The four entry points

- `update_tile_values()` (`hwtiles.cpp:187-196`) reads four 16-bit page
  selectors and four each of x/y scroll registers out of `text_ram` (fixed
  offsets `0xe80`/`0xe90`/`0xe98`) into the small `page[4]`/`scroll_x[4]`/
  `scroll_y[4]` member arrays. Pure register-file refresh, no rendering.
- `render_tile_layer(buf, page_index, priority_draw)` (`hwtiles.cpp:212-309`)
  draws one of the two scrolling tilemap layers. `page_index` selects which
  of the four scroll/page register sets to use (1 = background, 0 =
  foreground — `video.cpp:199-200`); it does **not** select different tile
  data, only a different scroll/page context into the same `tile_ram`. Both
  calls pass `priority_draw = 0`, so only tiles whose priority bit is clear
  are drawn by either call (see §7).
- `render_text_layer(buf, priority_draw)` (`hwtiles.cpp:311-354`) draws the
  fixed 64x32 text plane straight out of `text_ram`, called once with
  `priority_draw = 1` (`video.cpp:208`).
- `render_all_tiles()` (`hwtiles.cpp:199-210`) is a debug dump (raw tile
  index order, no scroll/priority logic) — not part of the per-frame path.

## 2. Per-frame control flow, trip counts, coverage

`render_tile_layer` walks a fixed **64 x 128** grid of 8x8 cells
(`my`/`mx` loops, `hwtiles.cpp:231-235`) = 8192 grid cells per call, twice
per frame (bg + fg) = 16384 cells. This grid is the full 1024x512-pixel
tilemap addressed by four page slots per quadrant
(`quad`/`ActPage`, `hwtiles.cpp:238-239`), i.e. much bigger than the visible
window. Only the sub-window actually on screen produces a draw: with
`s16_width_noscale` = 320 and `S16_HEIGHT` = 224, that's 40x28 = **1120 of
the 8192 cells** (13.7%) per layer, before accounting for transparent tile
index 0 (skipped whole via `if (Code == 0) continue;`, `hwtiles.cpp:270`).
Scroll/wrap: `x_decrement`/`y_decrement` (`hwtiles.cpp:227-228`) are
subtracted from each cell's screen position with wraparound at `+1024`
(x, `hwtiles.cpp:282-283`) and `+512` (y, `hwtiles.cpp:289-290`); per-column
and per-row fine scroll is read from `text_ram` when the sign bit of the
16-bit scroll register is set (`hwtiles.cpp:222-225`).

`render_text_layer` walks a fixed **32 x 64** grid (`hwtiles.cpp:316-320`) =
2048 cells, `TileIndex` incrementing by 2 every iteration — a fully linear
scan of `text_ram`, no page indirection, fixed `x -= 192` offset
(`hwtiles.cpp:341`), no scroll registers at all.

Both loops end each surviving cell with the same bounds dispatch: an
unclipped fast path when the whole 8x8 tile is inside the visible rectangle
(`hwtiles.cpp:302-303`, `345-346`), else a per-row/per-pixel clipped path
(`hwtiles.cpp:304-305`, `347-348`). At most 1 tile in ~40 per row needs the
clip path (screen edges), so the fast path dominates for the bulk of the
frame. A fully opaque tile in hi-res mode writes 8x8x4 = 256 destination
pixels (2x2 replication, §3); 1120 such tiles from one fully-opaque layer
write exactly 1120x256 = 286,720 pixels — the whole frame. With two layers
(bg+fg) both largely opaque, the two `render_tile_layer` calls alone can
issue on the order of 2 stores per destination pixel before road/sprites/text
are drawn.

## 3. Inner loop detail

`render8x8_tile_mask_lores` (`hwtiles.cpp:356-397`) is the hot pixel loop for
the lo-res build; `..._hires` (`hwtiles.cpp:458-500`) is used in the shipping
config. Both are structured the same way, 8 rows per call:

- Per row: one 32-bit load `p0 = *pTileData` (`hwtiles.cpp:372`, `475`) from
  the decoded tile table (`tiles`, see §4) — this is the only source read.
  One branch on `p0 != nMaskColour` (`374`, `477`) skips the whole row if
  all 8 nibbles are transparent (`nMaskColour` is always literal `0` at
  every call site).
- 8 shift+mask ALU ops unpack the 4-bit nibbles `c0..c7` (`376-383`,
  `479-486`) — no lookup table, just bit twiddling on the loaded word.
- 8 data-dependent branches, one per nibble (`if (c0) buf[0] = ...` etc.,
  `385-392`, `488-495`) — content-dependent, effectively unpredictable
  across arbitrary tile art, ~7-cycle misprediction cost each on A53.
- Lo-res: up to 8 scalar 16-bit stores per row, one per surviving pixel,
  direct index into `buf` (`hwtiles.cpp:385-392`).
- Hi-res: up to 8 calls to `set_pixel_x4` (`hwtiles.cpp:452-455`), each
  issuing **4 separate scalar 16-bit stores** (`buf[0]`, `buf[1]`,
  `buf[width]`, `buf[1+width]`) to replicate one source nibble into a 2x2
  physical block. Two of the four addresses in each call are adjacent
  (`buf[0]`/`buf[1]` and `buf[width]`/`buf[1+width]`) and carry the same
  16-bit value — a direct 32-bit-store merge opportunity that the source
  does not do explicitly (whether the compiler does it is not determinable
  without disassembly).
- The `_clip` variants (`hwtiles.cpp:399-443`, `502-548`) add a per-row Y
  bound check and AND a per-pixel X bound check into every one of the 8
  per-pixel branches — more branches, same store shape.
- The destination is **never read** in any of these four functions — only
  conditionally written. Compositing is overdraw-by-omission (transparent
  nibble = no store), not read-modify-write blend.

Outside the pixel loop, `render_tile_layer`'s cell loop does one 16-bit
`tile_ram` load per cell (`hwtiles.cpp:257`, reconstructed from two byte
loads), a priority-bit branch (`259-261`), and — only on a hit — bank
remap/mask (`263-268`), colour extract (`272`), wrap arithmetic (`279-290`),
and an unused `ColourOff` computation (`292`, see §7). The `quad`/`ActPage`
computation at `hwtiles.cpp:238-239` (a variable-shift + mask) is redone on
*every* of the 8192 `mx` iterations even though `my >= 32` only changes 64
times per call — a hoistable per-row quantity currently recomputed 128x more
often than necessary.

## 4. Caching and precomputation

There is no per-frame or on-demand tile decode cache: `init()`
(`hwtiles.cpp:110-137`) decodes **every** tile in the ROM once, up front,
into a flat array `tiles[TILES_LENGTH]` (`hwtiles.hpp:40`,
`TILES_LENGTH = 0x10000` = 65536 `uint32_t` = 8192 tiles x 8 rows =
**256 KiB**, `hwtiles.hpp:39`). Each `uint32_t` packs one 8-pixel row as 8
4-bit palette-slot nibbles via the `PL0`/`PL1`/`PL2` bit-interleave LUTs
built once in the constructor (`hwtiles.cpp:90-101`, 256 entries x 4 bytes x
3 = 3 KiB, init-time only, not touched in the render path). A second,
identical 256 KiB copy, `tiles_backup` (`hwtiles.hpp:41`), exists purely for
`patch_tiles()`/`restore_tiles()` (`hwtiles.cpp:140-161`, whole-array
`memcpy`) and is not read during rendering.

**Flag: `tiles` (256 KiB) alone is 8x the 32 KB L1D.** It is addressed by
`nTileNumber << 3` (`hwtiles.cpp:367`, `410`, `469`, `513`) with
`nTileNumber` coming from arbitrary ROM-authored map data — effectively
random access across the full 256 KiB table, not sequential. Because the
offset is always a multiple of 32 bytes, every 8-row (32-byte) tile fetch
does land inside a single 64-byte cache line regardless of tile parity, so
each *distinct* tile touched costs exactly one line fill, not two. Whether
the real working set per frame (only some of the 8192 tiles are used by any
given map) is small enough to stay resident in L1D/L2 depends on game
content and is **not determinable from the code**. `tiles` + `tiles_backup`
together are 512 KiB — the whole shared L2 budget — though the backup copy
is cold outside patch/restore. `tile_ram` (`hwtiles.hpp:18`, 64 KiB) and
`text_ram` (`hwtiles.hpp:17`, 4 KiB) are both well under L1D individually.

Tile RAM/text RAM are not written by a single per-frame function; they are
mutated incrementally by the 68000 CPU emulation through
`Video::write_tile8/16/32` and `Video::write_text8/16/32`
(`video.cpp:250-329` for text, mirrored for tiles starting `video.cpp:335+`),
scattered across however many memory-mapped writes the game code issues that
frame — volume not visible from `hwtiles.cpp` alone.

## 5. Cost model (estimates, not measurements)

Assumptions: one ALU/branch op ~1 instruction; A53 issues at most one
load-or-store per cycle. Per **surviving** (opaque) pixel in the lo-res
kernel: ~1 branch (nonzero test) + 1 add (`nPalette + cN`) + 1x16-bit store,
plus an 8-pixel-amortized share (~0.125/pixel) of the row's 32-bit tile load
and 8 nibble-extract ops — roughly **4 instructions and 1 store per opaque
pixel**, 0 reads of the destination. In hi-res, each *source* nibble still
costs about the same decode, but fans out to 4 store instructions covering 4
destination pixels — so store count per *destination* pixel is still ~1, but
delivered as narrow, scattered 16-bit `STRH`-class ops instead of fewer wider
ones (§6). Per fully-opaque 8x8 tile: 8 loads, ~64 branches, up to 64
(lo-res) or 256 (hi-res) stores.

Per frame, at up to 1120 visible tiles x 2 layers (bg+fg), the two
`render_tile_layer` calls can issue on the order of `2 x 1120 x 256` ≈
573,440 store instructions in the worst (fully opaque) case — roughly 2
stores per destination pixel before road/sprites/text are drawn.
`render_text_layer`'s grid is 4x smaller (2048 vs 8192 cells) and its
content is typically sparse (HUD digits/signs, mostly `Code == 0`), so it is
very unlikely to approach that volume. `update_tile_values()` is O(1) (~24
memory ops total) and negligible.

**Which call dominates:** the two `render_tile_layer` calls are almost
certainly the primary cost among the four tile-layer entry points, being
comparable to each other and structurally capable of the largest pixel-store
volume of the four. **Caveat:** `video.cpp`'s own instrumentation
(`prof_ns_road_tiles`, `video.cpp:211`) times `t0`→`t1`
(`video.cpp:194`/`205`), which wraps `update_tile_values()`, the road
background *and* foreground draws, and both tile-layer calls together
(`video.cpp:196-203`) — the given 2.20 ms figure is a road+tiles bundle, not
a tiles-only number, and the split between hwroad and hwtiles is **not
determinable from the code**.

## 6. Ranked list of expensive-looking things

1. **Hi-res 2x2 replication via 4 scalar 16-bit stores instead of fewer,
   wider ones** — `set_pixel_x4` (`hwtiles.cpp:452-455`), used 8x per row in
   `render8x8_tile_mask_hires`/`_clip_hires` (`hwtiles.cpp:488-495`,
   `535-542`). Two of the four stores per call are adjacent, same-value
   16-bit writes that are natural 32-bit-store merges; combined with the
   next source nibble, a whole 8-wide tile row (16 hi-res destination
   pixels) is a candidate for one or two wide/vector stores instead of up to
   32 scalar ones. This is exactly the pattern the sprite work already found
   costly on this core.
2. **8 data-dependent per-pixel branches per tile row, doubled again by the
   clip variants** (`hwtiles.cpp:385-392`, `430-437`, `488-495`,
   `535-542`) — content-dependent nonzero tests on ~7-cycle misprediction
   hardware, present in all four `render8x8_tile_mask*` variants.
3. **`quad`/`ActPage` recomputed every `mx` iteration** (`hwtiles.cpp:238-
   239`) though `my >= 32` is invariant across the 128-wide inner loop —
   128x more often than needed; the 4 possible `ActPage` values could be
   precomputed once per `render_tile_layer` call.
4. **8192-cell grid walked per layer to find ~1120 visible tiles**
   (`hwtiles.cpp:231-235`) — 86% of cells do a `tile_ram` load and priority
   branch (`hwtiles.cpp:257-261`) purely to be discarded; scroll math means
   the visible sub-rectangle isn't trivially known in advance, but bounding
   the `my`/`mx` ranges to the visible window (with wrap handled at the
   edges) would cut iteration count roughly 7x.
5. **`ColourOff`/`TILEMAP_COLOUR_OFFSET` computed but discarded**
   (`hwtiles.cpp:292`, see §7) — cheap per-tile, but pure waste and worth
   removing once its correctness status is settled.

## 7. Correctness hazards / dead code noticed in passing

> **Resolved 2026-09-04 (maintainer):** there are no known colour or palette
> problems in the shipped game, so the dead `nPaletteOffset` parameter below is
> benign in practice. Treat it as vestigial code to delete for clarity, not as a
> defect to investigate, and do not spend optimisation effort there.

- **`nPaletteOffset` is a dead parameter.** All four
  `render8x8_tile_mask*` functions declare it (`hwtiles.cpp:364`, `407`,
  `466`, `510`) but never reference it in the body — `nPalette` is built
  solely from `nTilePalette`/`nColourDepth`/`nMaskColour`
  (`hwtiles.cpp:366`, `409`, `468`, `512`). Every call site computes a
  non-trivial `ColourOff` value for it, including the `(Colour >> 5) << 8`
  high-palette-bank term (`hwtiles.cpp:292`) and the `TILEMAP_COLOUR_OFFSET`
  constant (`0x1c00`, `hwtiles.hpp:50`) — none of it reaches the framebuffer.
  If tiles with `Colour >= 0x20` are meant to select a different palette
  bank, that selection currently has no effect; this looks worth verifying
  against the palette/renderer stage rather than assuming it's benign.
- **Only `priority_draw = 0` tiles are ever drawn by `render_tile_layer`**,
  and only `priority_draw = 1` text is ever drawn by `render_text_layer`
  (`video.cpp:199-200`, `208` — confirmed these are the only call sites in
  the tree). Whatever content sets the tile-priority bit for the
  complementary case (priority-1 scrolling tiles, priority-0 text) is never
  rendered through these functions; this may be correct S16 semantics but
  wasn't verified against a reference implementation here.
- **Redundant mask** `Code &= (NUM_TILES - 1)` at `hwtiles.cpp:268`: for
  `render_tile_layer`, `Code` was already formed as `Data & 0x1fff`
  (`hwtiles.cpp:263`) then remapped through the 2-entry `tile_banks` table
  shifted by 12 (`264`), so the final mask is doing real work only when
  `tile_banks[i] > 1`; harmless but worth a comment if that's never expected.
