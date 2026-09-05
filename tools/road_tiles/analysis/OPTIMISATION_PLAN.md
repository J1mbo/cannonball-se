# Road and tilemap renderers on Cortex-A53: cost model, data study and optimisation plan

Target: Pi Zero 2 W / Pi 3, AArch64, one A53 core at 1.0 GHz (1 ms = 1.0M cycles), shipping
config (hi-res on, widescreen off, 640x448 x 16-bit = 573 KB destination), three other cores plus
the GPU hammering the same LPDDR2. Microarchitecture facts are taken from
`tools/sprite_bench/analysis/MICROARCH_A7_A53.md` (A53 column): in-order dual-issue, one memory
op per cycle, no load+store co-issue, 32 KB L1D with 64-byte lines, 512 KB shared L2 at 15-17
cycles, ~170 cycles to DRAM, 7-cycle mispredict, 128-bit NEON ops occupy two issue slots, no
documented hardware prefetcher, no documented no-write-allocate mode.

Everything labelled **measured** below is either the in-game timing table supplied with the brief
or something I computed from the ROMs / from 100 instrumented frames. Everything labelled
**estimated** is my reasoning from the microarchitecture facts.

Method notes. (1) I decoded the tile and road ROMs from `C:/Users/pearc/OneDrive/Documents/roms/`
with a throwaway Python script that mirrors `hwtiles::init` (`src/main/hwvideo/hwtiles.cpp:110-137`)
and `HWRoad::decode_road` (`src/main/hwvideo/hwroad.cpp:171-194`). (2) I built a private copy of
the tree under WSL (x86-64, `-Ofast -march=native`, unmodified renderers) with ~60 lines of
instrumentation in `Video::prepare_frame` that writes road RAM, tile RAM, text RAM, the scroll
registers and a snapshot of the 573 KB buffer after every layer. 100 frames were sampled across
~5.5 minutes of the attract cycle (53 attract-driving, 4 logo, 3 best-outrunners frames), and a
Python re-implementation of the two renderers was run over the captured RAM to count exactly what
each call does. Nothing under `src/` or the ROMs on this machine was modified. (3) The hot loops
were copied into a freestanding file and cross-compiled with clang 18 (`--target=aarch64-linux-gnu
-mcpu=cortex-a53 -Ofast`) to count instructions and memory ops; the Pi build uses GCC, so counts
are representative, not exact.

---

## 1. How each renderer works

### 1.1 Road background, `HWRoad::render_background_hires` (`hwroad.cpp:521-589`)

Loop over `y = 0..446 step 2` (224 iterations, one per lo-res scanline). Per iteration: two
16-bit loads from `ramBuff` (the road RAM half swapped in by `read_road_control`,
`hwroad.cpp:218-232`, called from `ORoad::do_road`, `src/main/engine/oroad.cpp:206`), a 4-way
switch on `road_control & 3`, and if either road entry for that scanline has the solid-fill bit
(0x800) a `std::fill_n` of `s16_width` 32-bit words = 1280 pixels covering hi-res rows `y` and
`y+1` (`hwroad.cpp:580-587`). clang turns the fill into `stp q,q` (32 bytes per instruction).

Trip count and output (measured over 100 frames): 229 hi-res rows filled on average (72-376,
depends on the horizon), i.e. 146,600 pixels = 293 KB per frame, 51% of the buffer. The sky
colour comes from `color | color_offset3` per lo-res line, so consecutive rows usually differ
(sky gradient).

### 1.2 Road foreground, `HWRoad::render_foreground_hires` (`hwroad.cpp:595-789`)

Loop over hi-res `y = 0..447`. Rows where both road entries are solid are skipped two at a time
(`hwroad.cpp:631-635`). For the rest, the per-row setup fetches the road-RAM line index and
h-scroll for both roads (`hwroad.cpp:644-647`); odd rows interpolate the source line and h-scroll
between lo-res lines `yy` and `yy+1` (`hwroad.cpp:648-683`); even rows rebuild the 10-entry
`color_table` (`hwroad.cpp:687-697`). Then the pixel loop. In OutRun `road_control & 3` is 1 in
99 of 100 sampled frames (`case 1`, `hwroad.cpp:729-752`):

```
for 320 iterations (one per pixel pair):
    pix0 = thpos0 < 0x200 ? src0[thpos0] : 3        load (byte) + compare + branch
    pix1 = thpos1 < 0x200 ? src1[thpos1] : 3        load (byte) + compare + branch
    c = priority_lookup[pix0][pix1] ? color_table[0x10+pix1] : color_table[pix0]   2 loads
    *out32++ = c | c<<16                            1 store (2 pixels)
    thpos0 = (thpos0+1) & 0xfff; thpos1 likewise
```

Cross-compiled: 18 instructions per iteration, 4 loads + 1 store, priority select as `csel`
(no data-dependent branch in clang's output; GCC may differ). Each source line is 512 bytes of
`roads[]` (`hwroad.hpp:42`, 257 KB decoded).

Trip count and output: 219 rows rendered on average (72-376) x 320 iterations = 70k iterations,
140,000 pixels = 280 KB per frame. Rows filled by the background plus rows rendered by the
foreground sum to 448 in every sampled frame: the two calls together write every pixel of the
buffer exactly once.

### 1.3 Tilemap layers, `hwtiles::render_tile_layer` (`hwtiles.cpp:212-309`)

Called twice per frame (`src/main/video.cpp:202,205`): page 1 (background) then page 0
(foreground), both with `priority_draw = 0`. Structure:

```
for my in 0..63:                 (64)
  for mx in 0..127:              (128)  -> 8192 cells per call
    quad -> ActPage (4 bits of page[])   hwtiles.cpp:238-252
    Data = tile_ram[idx]<<8 | tile_ram[idx+1]   two byte loads
    if priority bit != priority_draw: continue
    Code = ...; if Code == 0: continue          hwtiles.cpp:263-270
    x = mx*8 - x_decrement (+1024 wrap), y = my*8 - y_decrement (+512 wrap)
    if fully inside: render8x8_tile_mask_hires   hwtiles.cpp:302-303
    else if partly inside: ..._clip_hires        hwtiles.cpp:304-305
```

The whole 64 KB of tile RAM is read every call. Only cells whose screen position lands in
(-8,320) x (-8,224) can render: at most 41 x 29 = 1,189 of the 8,192 cells. Cross-compiled, the
common path (cell rejected) is ~16 instructions including 2 loads and 2-3 conditional branches.

`render8x8_tile_mask_hires` (`hwtiles.cpp:458-500`): 8 rows; per row one 32-bit load of the
packed 8 x 4-bit pixels; if the word is non-zero, 8 x (extract nibble, `cbz`, add palette,
`set_pixel_x4`). `set_pixel_x4` (`hwtiles.cpp:452-455`) is four 16-bit stores (2x2 block).
Cross-compiled: a fully opaque row is ~64 instructions of which 32 are `strh`. The clip variant
(`hwtiles.cpp:502-548`) adds per-pixel x-range tests.

Measured per frame (100 frames): background layer 319 tiles rendered (120-738; 30 via the clip
path), 8 rows each: 53% all-transparent (word == 0, skipped), 43% fully opaque, 5% mixed; 36,200
destination pixels written, 122 distinct tile codes on screen. Foreground layer 95 tiles (80-123),
rows 29% zero / 61% opaque / 10% mixed; 16,000 pixels, 40 distinct codes. Only 13% (bg) and 4%
(fg) of the 8,192 scanned cells even carry a non-zero code.

### 1.4 Text layer, `hwtiles::render_text_layer` (`hwtiles.cpp:311-354`)

Same kernels, 32 x 64 = 2,048 cells, `priority_draw = 1`, fixed position `x = mx*8 - 192`
(columns 0-23 are never visible). Measured: 33 tiles in attract driving (score/time/HUD), 581-648
on the best-outrunners screens; mean over the cycle 71 tiles, 5,900 pixels.

### 1.5 What feeds them

Road RAM is written by `ORoad::do_road` (`oroad.cpp:193-207`): `blit_road` writes the 2 x 0x1A0
line-select words (`oroad.cpp:1526-1551`), `output_hscroll` the 2 x 512 h-scroll words
(`oroad.cpp:1553-1596`), `copy_bg_color` the 1 KB colour table (`oroad.cpp:1598-1616`), then
`read_road_control` swaps halves. All of this happens inside `tick()`, which the main loop always
runs before `video.prepare_frame()` on the same thread (`src/main/main.cpp:427-428, 585-586,
599-600`), so road RAM and `road_control` are frozen for the whole of `prepare_frame`. Tile RAM
is only rewritten at stage boundaries (`OTiles::copy_fg_tiles/copy_bg_tiles`,
`src/main/engine/otiles.cpp:285-398`); per frame only the six scroll/page registers change
(`OTiles::write_tilemap_hw`, `otiles.cpp:47-55`). `tile_banks` is `{0,1}` forever
(`hwtiles.cpp:85`), so the bank remap in `hwtiles.cpp:264` is the identity.

---

## 2. Cost model per call

Destination line accounting (measured from the snapshots, 64-byte lines, 8,960 lines in the
buffer; "first" = not touched earlier in the same frame):

| layer | lines touched | of which first touch |
|---|---|---|
| road background | 4,581 | 4,581 |
| tile background | 1,240 | 372 (all in rows the road foreground later overwrites) |
| tile foreground | 552 | 44 |
| road foreground | 4,379 | 3,962 |
| sprites | 2,147 | 0 |
| text | 284 | 0 |

Every one of the 8,960 lines is first-touched exactly once per frame. Because 573 KB does not fit
L2 (512 KB, shared, and being streamed through by the NTSC threads' 15.8 MB table) each first
touch is a write-allocate line fill from DRAM; later touches within the same few hundred
microseconds hit L2.

**Road background** (measured 0.265 ms solo, 0.682 ms loaded). 265k cycles / 4,581 lines = **58
cycles per line solo, 149 loaded**. The store-issue floor is ~2 x `stp q` per line ≈ 4 cycles, i.e.
~0.02 ms. This call is >90% write-allocate fetch: there is no computation to hide it behind. The
+0.42 ms under load is contention on the fetches, not on our stores.

**Road foreground** (0.70 solo, 0.88 loaded). 70k iterations x 18 instructions = 1.26M
instructions, 350k memory ops. At one memory op per cycle and two instructions per cycle the
floor is ~9-10 cycles per iteration; measured 700k / 70k = **10 cycles per iteration**. So solo
this loop is issue-bound on instruction count and the single memory pipe, and its 3,962 x 58 =
230k cycles of line fills are hidden under the compute (the fills proceed while the next 20
iterations execute). Under load the fills stretch to ~149 cycles per line = 590k cycles, only
partly hidden, hence +0.18 ms. Consequence for the plan: making this loop 6x faster on its own
exposes ~0.3-0.6 ms of fetch latency; the fetches must be dealt with at the same time.

**Tile background** (0.34 solo, 0.43 loaded). Estimated budget: scan 8,192 cells x ~16
instructions ≈ 130k instructions, with two data-dependent branches per cell (13% taken rate) ≈
90-110k cycles; the 64 KB tile-RAM sweep is 1,024 L1 misses (16k cycles from L2, 60k if the NTSC
threads have pushed it to DRAM); kernel 1,081 opaque rows x ~40 cycles + 120 mixed x ~25 + 1,333
zero x ~4 + 319 calls x ~30 ≈ 60k; destination 868 L2 re-touches x 16 + 372 first touches x 58 ≈
35k. Sum ≈ 0.21-0.26 ms against 0.34 measured. The residual 0.08-0.13 ms is not explained by my
model; the two candidates are the tile-RAM sweep coming from DRAM and mispredicts in the scan.
Either way **the 8,192-cell scan is the largest component (~40-50%) and the pixel kernel is only
~20%**. Under load the extra +0.09 ms matches the 372 first-touch lines at +91 cycles each plus a
colder tile-RAM sweep.

**Tile foreground** (0.18 solo, 0.20 loaded). Same scan cost (~0.1 ms) but only 95 tiles: kernel
465 x 40 + 78 x 25 + 220 x 4 + 95 x 30 ≈ 25k, destination 552 lines mostly L2 ≈ 11k. Model 0.14
vs 0.18 measured, same-sized residual as the background layer. **Here the scan is ~60% of the
call.**

**Text** (0.31 loaded). For a driving frame (33 tiles): scan 2,048 cells ≈ 20k cycles, kernel
~5k, 284 lines from L2 ≈ 5k (or ≈ 40k if evicted to DRAM by the sprite pass): 0.03-0.07 ms. For a
best-outrunners frame (600+ tiles): ~1,200 opaque rows x 40 + 1,500 lines x 16-149 ≈ 0.1-0.3 ms.
**The measured 0.31 ms does not fit a driving frame at all**; it fits only if the 2-second
measurement window sat on the text-heavy screens, or if text's destination lines were DRAM-cold.
This should be re-measured with the per-frame text-tile count logged beside the time; until then
I treat the text layer as a smaller version of the tile-layer problem.

**Where the milliseconds go (loaded, road+tiles+text = 2.51 ms, estimated split):** write-allocate
fetches on first touch ≈ 1.0-1.2 ms (road bg 0.6, road fg 0.3-0.4, tiles ~0.1); the road
foreground pixel loop ≈ 0.6; the two 8,192-cell tilemap scans ≈ 0.2-0.25; tile pixel kernels
≈ 0.1; text ≈ 0.1-0.3. The memory system, not instruction count, is the first-order problem,
and it is paid at exactly one place per line: whichever layer touches the line first.

---

## 3. What the data lets you do

**Road ROM (`opr-10185.11`, 32 KB, decoded to 256 lines x 512 px, values 0/1/2/3/7).**
Measured: every line is distinct; value histogram over all pixels: exterior (3) 51.2%, road (0)
39.3%, stripes (1, 2) 4.3% each, centre stripe (7) 1.0%. **A line is at most 15 runs of constant
value** (220 of 256 lines have exactly 15, none more), and the visible window is 320 source
pixels of the 512 with everything outside `[0,0x200)` forced to 3. Per rendered output row the
two roads' run boundaries merge into a mean of **16.9 constant-colour segments, never more than
29** (histogram over 21,900 rendered rows). The renderer currently does 320 iterations with four
loads each to produce what is a list of 17 fills. The whole run-length table is 257 lines x
≤15 x (2-byte start + 1-byte value) ≈ 13 KB, L1-resident, versus 512 bytes of `roads[]` streamed
per road per row today.

**Tile ROM (3 planes x 64 KB → 8,192 tiles).** Measured: 6,313 distinct tiles, 737 all-zero;
31.2% of all pixels transparent; of the 65,536 tile rows 24% are all-transparent, **59% fully
opaque**, 17% mixed; 31% of rows are a single colour. In the rendered set the opaque share is
higher: 43% of background rows and 61% of foreground rows are fully opaque, and only 5%/10% are
mixed (transparency is mostly whole rows, i.e. tile edges). Because `nPalette` is
`palette << 3` (`hwtiles.cpp:468`), adding a nibble never carries out of the low byte, so the
colour of a pixel is `nPalette + nibble` with a constant high byte: a NEON widen-and-add, no
table lookup needed. Any 8-pixel row therefore maps to two 128-bit vectors (16 hi-res pixels,
2 rows) with ~12 vector ops and four 128-bit stores instead of 32 halfword stores.

**Tile RAM contents per frame.** Only 13%/4% of scanned cells carry a code; only 1,189 cells can
be on screen; 122/40 distinct codes are visible per frame (a per-frame decoded-tile cache is not
worth it: 512 bytes per tile to copy costs about what expanding it costs).

**Layer overlap (measured from the snapshots).** 33% of tile-background pixels (11.8k of 36.2k)
and **80% of tile-foreground pixels (12.8k of 16.0k) land in rows that the road foreground then
overwrites in full**: that work, and the 372 first-touch line fills it triggers, is invisible.
A further 39% of tile-background pixels are covered by the foreground tilemap. Of the sky
region (147k px, 51% of the buffer) 83% remains visible after both tilemaps. The road
foreground writes every pixel of its rows in all four control modes (`hwroad.cpp:720-786`), so a
per-row "will be overwritten by the road" mask is exact and cheap (224 entries derived from
road RAM, which is frozen during `prepare_frame`).

**Alignment.** `pixels` is 64-byte aligned (`video.cpp:69-72`: `align_val_t(64)` plus 64
halfwords) and the hi-res pitch is 1,280 = 20 x 64 bytes, so every row is exactly 20 whole cache
lines. (Widescreen: 1,616-byte pitch, rows not line-aligned; any line-granular trick must clip to
lines wholly inside the row.)

---

## 4. Optimisation plan (ranked)

Savings are per frame under the 4-thread + GPU load unless stated. All items keep the 573 KB
buffer bit-identical; none adds a thread; resident memory grows by 13 KB (P3) at most.

### P1. Scan only the visible tilemap window (tiles and text)

*Mechanism.* In `render_tile_layer`, precompute the ≤2 contiguous ranges of `mx` whose screen
`x` (with the `+1024` wrap, `hwtiles.cpp:279-283`) lands in (-8, 320) and the ≤2 ranges of `my`
landing in (-8, 224); iterate only those cells (≤1,189 instead of 8,192). Same in
`render_text_layer` (columns 24-63, rows 0-27: 1,120 cells instead of 2,048). Cells skipped are
exactly those that fail the existing range tests at `hwtiles.cpp:302-305`, and tiles never
overlap within a layer (map 1024 x 512 px, screen 320 x 224), so order is irrelevant.
*Why it helps on this core.* Removes ~7,000 iterations x 16 instructions with data-dependent
branches per call, and 62 KB of tile-RAM reads per call (1,000 L1 misses that under load come
from a thrashed L2 or DRAM).
*Saving (estimated).* Tile bg 0.43 → ~0.30, tile fg 0.20 → ~0.09, text -0.01: **~0.25 ms**.
*Risk.* Negligible. *Effort.* 2 hours. *Measure.* `prof_ns_tile_bg/tile_fg` (`video.cpp:219-220`).

### P2. Pre-allocate destination lines with `DC ZVA` before the road fills (AArch64 only)

*Mechanism.* `DC ZVA, Xt` zeroes a whole 64-byte block and allocates it in the cache without a
line fill; it is architecturally defined, enabled for EL0 by Linux (glibc's aarch64 `memset` and
the kernel's `clear_page` are built on it) and the block size is read from `DCZID_EL0` at run
time (bit 4 = disabled → fall back). In `render_background_hires` issue 20 `dc zva` for the row
pair immediately before the `fill_n`; in `render_foreground_hires` issue 20 per rendered row
before its pixel loop. Guard: only lines wholly inside the row (trivially all of them at 1,280
pitch; the widescreen 1,616 pitch clips two partial lines per row). Both calls rewrite every
byte of the rows they zero within the same call, and nothing reads the buffer between, so output
is unchanged by construction. x86-64 / ARMv7: the macro is empty.
*Why it helps on this core.* The doc records no no-write-allocate or streaming mode; ZVA is the
one software lever that removes the read half of the write-allocate traffic. It converts
8,543 first-touch line fills per frame (573 KB of DRAM reads competing with the NTSC threads and
the GPU) into cache-local zero-fills, and removes the ~149-cycle exposed latency per line in the
background fill.
*Saving (estimated).* Road bg 0.68 → ~0.10-0.15; road fg 0.88 → ~0.70 (its hidden fills stop
consuming bandwidth); tile layers lose their 416 first touches after P4: **~0.6-0.7 ms**, plus
less bus pressure on the other three cores.
*Risk.* Correctness: low (whole-row rewrite; alignment guard). Benefit: medium; the doc has no A53
figure for ZVA, so it is verified by a microbenchmark first (section 5). *Effort.* Half a day
including the Pi measurement. *Measure.* `prof_ns_road_bg` loaded; expect ≤0.2 ms.
*Fallback if killed.* `PRFM PSTL1STRM` 2-3 rows ahead in the road foreground loop hides latency
but cannot remove the traffic; expected only ~0.1-0.15 ms.

### P3. Run-length road foreground

*Mechanism.* At `init`, convert each of the 257 decoded lines to its run list (≤15 runs; 13 KB).
Per rendered row keep the existing setup (`hwroad.cpp:637-711`, including interpolation) and
replace the 320-iteration loop with: (a) for each road, the visible window `[t, t+320)` over the
512-px line intersects `[0,0x200)` in one contiguous interval (t is monotonic in the pair index
with at most one wrap at 0xfff→0, and the wrapped part is always ≥0x200), so the road's contribution
is "3, then a slice of its run list, then 3"; (b) merge the two run lists by boundary
(≤29 segments, mean 17), evaluate `priority_lookup`/`priority_map` and `color_table` once per
segment; (c) fill each segment with a 16-bit constant: NEON `vst1q` (2 issue slots per 16 B), or
`std::fill_n` on 32-bit words in the scalar fallback. Modes 0/2/3 use the same merge with their
own select table; the old loops stay in the source as the reference for the tests.
*Why it helps on this core.* Per row ~17 segments x ~12 instructions + 80 x 128-bit stores ≈
250-350 cycles instead of 320 x 10 = 3,200; memory ops per output pixel drop from 2.5 to ~0.13;
no per-pixel loads of `roads[]` and no data-dependent branches at all; the 13 KB run table lives
in L1 where 512-byte source lines did not.
*Saving (estimated).* Compute 0.70 → ~0.10 ms. With P2 in place the fills are already cheap, so
loaded 0.88 → ~0.20: **~0.6 ms**. Without P2 the newly exposed line fills cap the gain at roughly
0.3 ms, which is why P2 ranks above it.
*Risk.* Moderate (four modes, wrap arithmetic, interpolated rows): mitigated by an exhaustive
per-road test (257 lines x 4,096 h-scroll values x 320 pairs against the scalar loop, ~340M
compares, seconds in C) and 10^5 random two-road merges per mode. *Effort.* 1-2 days.
*Measure.* `prof_ns_road_fg`; kernel microbench on the Pi.

### P4. Clip the tilemap layers to rows the road foreground will not overwrite

*Mechanism.* Before the tile calls, derive a 224-entry per-line mask from `ramBuff` and
`road_control` using precisely the skip logic of `render_foreground_hires` (`hwroad.cpp:631-635`
and the `continue` cases at 714-715, 775-776) and the caller's enable condition
(`video.cpp:208`: `!fix_bugs || horizon_base != HORIZON_OFF`). Pass it to the tile kernels;
skip a tile whose 8 rows are all masked, else skip masked rows (the clip variant already handles
partial rows). Road RAM cannot change between the mask and the road call (section 1.5).
*Why it helps.* 80% of foreground-tile pixels and 33% of background-tile pixels are dead work,
and they cause 416 first-touch line fills per frame in rows the road will zero-fill (P2) anyway.
*Saving (estimated).* ~0.05 ms of kernel work plus ~0.05 ms of fills without P2, ~0.03 with it.
*Risk.* Low-moderate: the mask must match the road renderer's row set exactly; a unit test that
renders road-fg into a scratch row set and compares the mask catches any drift. *Effort.* Half a
day. *Measure.* tile fg time; snapshot diff after the road foreground.

### P5. NEON tile-row kernel (shared by tile bg, tile fg and text)

*Mechanism.* For an opaque row: `dup` the 32-bit word, `and`/`ushr` to split nibbles, `zip1` +
`rev64` to order them c0..c7, `uaddw` with `nPalette` to widen to 8 x u16 colours, `zip1q/zip2q`
to double horizontally, 2 x `stp q` to rows y and y+1 (cross-compiled: 18 instructions, no
branches). For mixed rows either the existing scalar path (5-10% of rows) or a `cmeq`+`bsl`
read-modify-write; zero rows unchanged. Scalar fallback: the current code, or 32-bit pair stores
(`buf[0..1]` as one word) which halves the store count with no vectors at all. Clip variant keeps
scalar.
*Why it helps.* An opaque row is 32 `strh` + 24 ALU today, saturating the single memory pipe for
≥32 cycles; the vector row is 4 x 128-bit stores (8 slots) plus ~10 vector ops ≈ 15-18 cycles.
*Saving (estimated).* (1,081 + 465 + 231) opaque rows x ~25 cycles ≈ 0.05 ms in driving frames,
~0.1 ms on text-heavy screens. *Risk.* Low: pure function, exhaustively testable over all 8,192
codes x palettes x both row parities. *Effort.* 1 day.

### P6 (optional, later). Composition change: fused "sky composer"

Render sky rows in one pass: for each 8-pixel column block, take the background and foreground
tile rows (`ext` to handle the two layers' different x scroll), select `fg ? fg : bg ? bg : sky`
in registers and store once, with nibble 0 mapping to the per-line sky colour (`vbsl` with the
fill constant). Removes the separate fill pass, the tile RMW question, and the ~1,400 L2
re-touches of sky lines. Estimated gain only ~0.1-0.15 ms once P1-P5 are done (the re-touches
are L2 hits, 16 cycles each), for 2-3 days of work and the highest correctness risk (two scroll
offsets, page quadrants, clip edges, priority bit). A cheaper variant with most of the cache
benefit is band interleaving: fill 16 hi-res rows, then draw the bg and fg tiles intersecting
that band while its 20 KB is still in L1. Rank after everything above.

**Not worth doing.** `color_table` rebuilds, the `priority_map`/`priority_lookup` duplication,
`update_tile_values` (1 us), the lo-res paths, the road-RAM half swap (1 KB per frame), the
`tile_banks` remap: all below 0.01 ms.

**Expected end state (estimated, loaded):** road bg ~0.12, tile bg ~0.22, tile fg ~0.06, road fg
~0.20, text ~0.1-0.2 → **~0.7-0.8 ms** for the five calls versus 2.51 ms today, i.e. ~1.7 ms of
the 16.7 ms budget returned, and ~570 KB per frame less DRAM read traffic for the other cores.

---

## 5. What I would do first, and the kill rule

**First: P2 on the road background fill**, preceded by a 30-line microbenchmark on the Pi Zero 2 W:
fill a 573 KB 64-byte-aligned buffer with a constant, 600 times, (a) with `stp q` only, (b) with
`dc zva` per line then `stp q`, (c) with `dc zva` only, each run solo and with the shipping game
running its NTSC threads and shader on the other cores. Read `DCZID_EL0` first.

Why first: it is the smallest change (~40 lines), the largest single saving, and it is the
experiment that decides the rest of the plan. If ZVA halves the fill under load, the memory
model in section 2 is right and P3/P4 can be sized for compute alone. If it does not, the
line-fill cost is a floor and the plan collapses to P1 + P3 (capped) + P5, worth ~0.7 ms instead
of ~1.7.

**Kill rule.** Abandon P2 if any of: `DCZID_EL0` bit 4 set (ZVA prohibited) on the target kernel;
the loaded fill microbenchmark improves by less than 1.5x; or, in-game, `prof_ns_road_bg` under
the 4-thread load stays above 0.40 ms (from 0.68). Then use the `PRFM` fallback in the road
foreground only and drop the fill-side change.

Second, P1, the same afternoon: certain, tiny, and independent of the memory question.

---

## 6. What must stay identical, and how to know

The contract is the 573 KB palette-slot buffer after `prepare_frame`, every frame, bit for bit.
I propose no deliberate output change. Specific invariants the plan relies on and must test:

1. **Road rows:** every rendered foreground row writes all 640 pixels; every filled background row
   pair writes all 1,280 (both are what makes `dc zva` and P4 safe). Test: assert in a debug
   build that `bg_rows + fg_rows == 448`, and compare the row set produced by the P4 mask with
   the rows the road foreground actually writes (scratch buffer, per frame).
2. **Run-length road = scalar road.** Keep `render_foreground_hires` as `render_foreground_hires_ref`
   in a test-only build. Exhaustive single-road test: 257 lines x 4,096 h-scroll x 320 pairs; random
   two-road merges (10^5 per control mode, all four modes, both row parities including the
   interpolated case at `hwroad.cpp:648-683`); `memcmp` of the row.
3. **NEON tile row = scalar tile row.** All 8,192 codes x 8 rows x 8 palette values x
   (fast, clip at each of the 15 partial x offsets, both y-clip edges), `memcmp` of the 2 x 16
   pixels, on the Pi and on x86 (scalar fallback). Nibble order (c0 = bits 28-31 is the leftmost
   pixel, `hwtiles.cpp:479-495`) is the one thing easy to get backwards.
4. **Visible-window scan = full scan.** Property test over all 1,024 x-scroll and 512 y-scroll
   values: the set of (mx,my) accepted by the range tests equals the set produced by the window
   computation.
5. **Whole-frame regression.** The instrumentation I used (per-layer snapshots of the buffer plus
   road/tile/text RAM, ~4 MB per sampled frame) is the right harness: capture 200 frames across the
   attract cycle and a recorded gameplay session with the reference build, then run the new
   renderers over the captured RAM in-process and `memcmp` after every layer. That is independent
   of game determinism and of timing. Separately, hash the buffer per frame in both builds over
   the attract loop as a smoke test.
6. **Builds.** x86-64 MSVC (no NEON, no ZVA, scalar run-fill), ARMv7/ARMv6 (scalar), AArch64
   (NEON + ZVA); the scalar and vector paths must both pass tests 2-4, since the scalar path is
   what the MSVC build ships.

What is *not* required to be identical: timing, the order in which pixels within a layer are
written, and the contents of the lines zeroed by ZVA between the zero and the rewrite (nothing
observes them; the other buffer is what the NTSC threads read, `video.cpp:232`).
