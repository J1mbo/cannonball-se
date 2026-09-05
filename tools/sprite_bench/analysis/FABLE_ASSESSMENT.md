# Sprite renderer assessment for Cortex-A53 (Pi 3 / Pi Zero 2 W)

Scope: `hwsprites::render()` and its immediate producers/consumers, judged purely from the code as it stands. All line references are to the current tree. Target facts (in-order dual-issue A53, one load/store pipe, 64-bit NEON, 32 KB L1D, 512 KB shared L2 under contention from the NTSC thread and GPU upload, LPDDR2 with ~100 ns latency) are taken as given.

---

## 1. The pipeline

### 1.1 Inputs per frame

**Sprite list (sprite RAM).** The engine builds up to 128 entries of 16 x 16-bit words (`osprite::data[16]`, `src/main/engine/osprite.hpp:19`) in `OSprites::do_sprite()` (`osprites.cpp:569-905`), copies them into hardware sprite RAM via `blit_sprites()` -> `Video::write_sprite16()` -> `hwsprites::write()` (`osprites.cpp:528-541`, `video.cpp:423-427`, `hwsprites.cpp:162-165`), and on the next `update_sprites()` swaps `ram`/`ramBuff` word-by-word (`hwsprites.cpp:168-180`). `render()` reads `ramBuff`. Per-entry layout as consumed by `render()` (`hwsprites.cpp:425-456`):

| word | bits | meaning |
|---|---|---|
| 0 | 15 end-of-list; 14/12 hide; 13 clip-needed (JJP); 11-9 bank; 8-0 top+256 | |
| 1 | ROM word offset within bank | |
| 2 | 15-9 pitch (words per ROM row, 0..127) | sign path from MAME is lost (see 6) |
| 3 | 14 shadow-enable; 13-12 priority; 11-0 zoom (0x200 = 1:1, smaller = larger) | |
| 4 | 15 top-to-bottom (always set, `osprites.cpp:1020`); 14 h-flip; 13 left-to-right; 10-0 rendered width | |
| 5 | 15-8 height-1; 6-0 palette | |
| 6 | X in game coordinates (screen 0 = 0xBE) | JJP: moved out for widescreen |
| 7 | raw ROM height | read, unused |
| 15 | signed x offset for hi-res sprites | only if `hiresprites==1` |

Priority: `render()` is called exactly once with `priority=8` (`video.cpp:201`), i.e. only entries whose priority field is 3 are drawn. `set_priority(input->shadow << 4)` (`osprites.cpp:904`) with `shadow` in {3,7} makes every sprite priority 3, so nothing is skipped by that test in practice; the 30/60 fps tick decides which frames get a new list. Typical in-game count is a few dozen; the hard cap is 0x7F (`osprites.cpp:445`).

**Sprite ROM.** 1 MB, converted once in `init()` (`hwsprites.cpp:94-118`) into `sprites[0x40000]` 32-bit words: 8 x 4-bit pixels per word, p0 in bits 31-28. Four banks of 0x10000 words. Nibble 0 is transparent; nibble 0xF is transparent and, in the second-to-last position of a word, terminates the row (`hwsprites.cpp:678`); nibble 0xA is "shadow" when word 3 bit 14 is set, else an ordinary colour. Two derived tables are built lazily at run time: `sprites_flipped` (1 MB, nibble-reversed rows for h-flipped sprites, `hwsprites.cpp:523-577`) and `sprites_shadowinfo` (256 KB, one byte per ROM row start: 0xFF unseen / 0x11 row contains an 0xA / 0x00 not, `hwsprites.cpp:578-598`). The object is ~2.3 MB.

**Colour.** `color = 0x800 + (pal & 0x7F) << 4` (`hwsprites.cpp:448-449`); an opaque pixel writes `pix | color` (12-bit palette slot). A shadow pixel ORs `S16_PALETTE_ENTRIES` (0x1000) into whatever is already there (`hwsprites.cpp:224`), selecting the darkened copy of the slot.

### 1.2 Output

A `uint16_t` slot buffer, one of two (`Video::pixel_buffers[2]`, `video.cpp:66-71`): `s16_width x (s16_height+2)` plus a 64-element (128 B) front pad, so unclipped sprites may overrun by a few pixels. Dimensions: lo-res 320x224 (widescreen 404x224); hi-res doubles both (640x448 / 808x448) -> 143 KB / 181 KB / 573 KB / 724 KB. Values are palette slots 0..0xFFF plus bit 12 for shadow; the road pass fills every row first (`hwroad.cpp:244-292`), tiles then write masked 8x8 blocks (`hwtiles.cpp:370-398`), so the sprite pass never clears anything and is a pure read-modify-write on top.

The two buffers are frame n (being written by the prepare thread) and frame n-1 (being read by the render thread(s) through `Video::render_frame()` -> `RenderSurface::draw_frame()` -> `snes_ntsc_blit[_hires_vec]()` or the `s16_rgb555[]` lookup, `rendersurface.cpp:1398-1470`). `Video::swap_buffers()` (`video.cpp:94-100`, called at `main.cpp:639` after all semaphores are acquired) flips them, so no buffer is shared between threads at the same instant, but both live in the same 512 KB L2 as the NTSC 5 MB table and the GPU upload source.

### 1.3 Per-frame control flow

`main_loop()` (`main.cpp:548-737`) releases `renderReady0/1` then `prepareReady`; `prepare_thread()` (`main.cpp:431-440`) runs `tick()` (engine, incl. `osprites.sprite_copy()` / `update_sprites()`) then `Video::prepare_frame()` (`video.cpp:176-204`): road background, tile layer x2, road foreground, **`sprite_layer->render(pixels, 8)`**, text layer.

Inside `render()` (`hwsprites.cpp:414-1003`), per entry:

1. 421-456 decode; 425 end-of-list; 428/433 priority/hide.
2. 460-487 X fix-up, bank select, widescreen offset, hi-res doubling (`xpos<<=1, top<<=1, ytarget<<=1, zoom>>=1`).
3. 492-494 ROM rows needed (`steps*zoom>>9`).
4. 509-519 right-to-left is converted into left-to-right with the flip bit toggled, so `xdelta` is always +1 afterwards.
5. 523-598 **pre-pass**: for every ROM row of the sprite, either check/build the flipped copy or check/build the shadow byte.
6. 620-980 **row loop** over output scanlines: 623 vertical clip; 630-637 `count` = how many output rows (1..3) share this ROM row; 642 `shadowfound` from the shadow byte; 649-651 `jump_key = count-1 | (clip?0:4) | (shadow?8:0)`; 660 `switch` into one of 12 live 8-pixel-unrolled word loops built from the `draw_pixel_*` macros (`hwsprites.cpp:219-408`); 977-979 zoom accumulator / ROM row advance.

Each word loop: load one ROM word, for each of 8 nibbles run a DDA `while (xacc < 0x200)` that emits 0..N output pixels, then test the 0xF terminator.

---

## 2. Cost model on the A53

Assumptions: GCC 14 `-O3`, nibble extraction folds to `ubfx`; sprite content is roughly half transparent at silhouette edges; a typical in-game frame draws 40-80 sprites with 30-60 output rows each. I have no measured pixel counts (see section 5) so the frame totals below are order-of-magnitude.

### 2.1 Per output pixel (inner DDA iteration)

Best path (`draw_pixel_1row_nc_ns`, cases 4-6): `cmp xacc; b.ge` + `sub/cmp/b.hs` (transparent test) + `orr` + `strh` + `add pPix` + `add xacc` + `b` = ~10 instructions, one store, no loads. Clipped paths add `sub/cmp/b` (3), shadow paths add `cmp/b` + an RMW `ldrh/orr/strh` on 0xA pixels, multi-row paths add one `strh` + one `add` per extra row. With dual issue and one branch per cycle this is ~5-7 issue cycles/pixel on an ideal predictor.

Branches per output pixel: two data-dependent ones (DDA exit, transparency), three on shadow paths. The transparency branch mispredicts at every silhouette edge (~2 per row, cheap). The DDA-exit branch is the problem: zoom is fractional for almost every sprite (`ZOOM_LOOKUP` first column spans 0x100..0x3F0 with 0x200 appearing on a single row; hi-res halves 0x100..0x7E0 to 0x80..0x3F0), so the emit-count sequence per ROM pixel is a periodic pattern of period up to 512 pixels. The A53's modest global-history predictor will track short periods (2:1, 3:2) and miss on the rest; at ~8 cycles per miss and one miss per ~3-8 pixels that adds 1-3 cycles/pixel. **Estimate: 8-12 cycles per output pixel, ~6-12 ns at 1.0-1.4 GHz**, before memory stalls.

### 2.2 Per ROM word / row / sprite

- Per word: 1 load (`spritedata[spriteaddr++]`), 8 `ubfx`, terminator `and/cmp/b` -> ~12 instructions. Sequential within a row, stride 4 B, prefetcher-friendly.
- Per row group: row pointer arithmetic (mul), the `count` while loop (<=2 iterations, `std::min` twice), one dependent byte load `spriterom_shadowinfo[addr]`, `freq[jump_key]++` (static RMW), and an **indirect branch** for the `switch`. `jump_key` is stable across a sprite unless `count` toggles (it does, every few rows, for fractional zoom), so the indirect predictor is hit-or-miss; assume 1 miss (8 cycles) per 2-4 row groups. ~40-60 instructions per group.
- Per sprite: ~120 instructions of decode, `bank %= 4`, a 64-bit multiply, **two `std::chrono::high_resolution_clock::now()` calls** (`hwsprites.cpp:423, 981`; vDSO `clock_gettime`, ~50-100 ns each on this core), and the pre-pass: `sprite_height` iterations of load+compare+branch on `sprites_shadowinfo` or `sprites_flipped` (`hwsprites.cpp:535-597`), i.e. 150-400 cycles for a 40-row sprite after warm-up. `reps[i]++` in `draw_pixel_1row_nc` (`hwsprites.cpp:275-289`) adds a static RMW per row on path 12.

Frame-level: 60 sprites x 40 rows = 2.4k row groups (~120k instructions, ~5-20k mispredict cycles) and ~0.1-0.2 M cycles of per-sprite overhead. Small next to the pixel loop unless sprite counts are high and sprites are tiny (far traffic / roadside objects), where per-row overhead dominates.

### 2.3 Memory footprint and traffic

**Destination.** Each output pixel is one 2-byte store; multi-row groups store to 2-3 rows 1280-1616 B apart (hi-res). Working set per frame = the sprite-covered region of a 143-724 KB buffer that the road/tile passes have just streamed through. Lo-res the buffer fits L2 comfortably; hi-res it does not even alone, and the NTSC thread is concurrently streaming the other 573-724 KB buffer plus a 5 MB table through the same L2. So in hi-res most sprite stores are **write-allocate misses to DRAM**: a line fetch (~100 ns) per 64 B touched. Rough count: 300k output pixels x 2 B / 64 B ~ 10k lines/frame; with in-order execution and a small store buffer only a fraction overlaps. Expected stall component: 0.3-0.7 ms/frame in hi-res, near zero in lo-res. Bandwidth (~1.3 MB/frame incl. writeback, ~80 MB/s) is not the issue; latency is.

**ROM.** Bytes read = ROM rows x pitch x 4. A 40-row, 8-word sprite is 1.3 KB; 60 sprites ~80-150 KB/frame scattered over 1 MB (`sprites`) + 1 MB (`sprites_flipped`, about half of sprites face the other way) + 256 KB (`sprites_shadowinfo`). Runs are sequential inside a sprite so the stride prefetcher hides much of the latency, but each sprite start is a cold miss to a new region, and the flipped copy doubles the L2-resident footprint for no arithmetic saving. ~2-3k lines/frame, mostly L2 misses under the NTSC load.

**TLB.** 4 KB pages: dest rows are 1.3-1.6 KB in hi-res so a sprite crosses a page every ~3 rows; ROM crosses one every ~1k words. Per frame ~200-300 distinct pages: fits the 512-entry main TLB, thrashes the 10-entry micro-TLB constantly (~5-10k refills/frame, a few cycles each -> ~30-50 us). Minor but measurable.

**I-cache.** `render()` is 12 unrolled word loops of ~0.5-1 KB each; a frame typically touches 4-6 of them (nc/ns 1-2 rows, nc shadow 1-2 rows, and the clip variants). ~4-6 KB hot, fine against 32 KB L1I, though the `switch` bouncing between kernels within one sprite costs I-fetch locality.

### 2.4 Frame totals (to be measured)

If a lo-res frame runs ~80k DDA iterations: ~0.8 M cycles ~ 0.6-0.8 ms. Hi-res is ~4x the iterations (2x each way; more if `hiresprites=1` because larger source frames are chosen, `osprites.cpp:695-700`): ~3 M cycles + 0.3-0.7 ms memory ~ **2.5-4 ms of the 16.7 ms prepare-thread budget**. Both figures are estimates that the first measurement in section 5 must replace.

---

## 3. Bottlenecks, ranked

1. **Data-dependent branches in the per-pixel DDA loop** (`hwsprites.cpp:221-230` and siblings). Two to three unpredictable branches per output pixel on a core that flushes ~8 cycles per miss and cannot overlap anything across a miss. This is the largest term in hi-res where fractional zooms (0x80..0x3F0 after halving) are universal.

2. **Write-allocate misses on the destination in hi-res**, aggravated by L2 sharing with the NTSC thread and GPU upload. Stores are cheap when they hit; here they mostly do not. The in-order pipeline has no way to hide a store-buffer stall once the buffer fills.

3. **Redundant row work in the 3-row groups.** Cases 2/6/10/14 write rows y..y+2 but advance `y` by only 2 (`for (int i=1; i<2; i++)` runs once, `hwsprites.cpp:732-737`, plus the outer `y += ydelta`). The next group starts at y+2 with `addr` unchanged (no carry can have occurred since `yacc+2*zoom < 0x200`) and redraws that row with identical values. Output is bit-identical today, but every 3-row group costs 4 rows of stores. 3-row groups occur whenever the halved zoom is < 0x100, i.e. the largest, closest sprites in hi-res, which are exactly the sprites with the most pixels.

4. **Doubled ROM working set** from `sprites_flipped` plus the `sprites_shadowinfo` side table and its per-frame pre-pass (`hwsprites.cpp:523-598`). Extra L2 footprint on a machine where L2 is the contested resource, plus a dependent load+branch per ROM row per sprite per frame after warm-up.

5. **Per-row-group indirect branch and bookkeeping** (`switch`, `freq[]`, `std::min`, the `count` loop) and per-sprite instrumentation left in (`chrono` x2, `draw[]`, `reps[]`). Individually small; together perhaps 5-10% of sprite time for frames dominated by small sprites.

6. **Off-screen iteration for clipped sprites** (`draw_pixel_*` without `_nc`): every off-screen output pixel still runs the DDA and the clip compare. Matters in widescreen where objects constantly cross the edges.

---

## 4. Optimisation ideas, ranked by gain per effort

All must keep the slot buffer bit-identical to today's output unless stated.

### 4.1 Fix the 3-row group advance (trivial)
Change the inner loop bound at `hwsprites.cpp:732, 809, 888, 965` so the group advances by `count-1` rows (2 for count 3). Mechanism: removes one full redundant row of stores per 3-row group. Why it helps here: a full row of write-allocate misses vanishes. Effect: up to 25% of row work on close-up hi-res sprites; frame-level maybe 5-15% of sprite time in hi-res. Risk: none by the argument in 3.3 (same ROM row, same colour, same shadow flag, no carry) — verify by buffer diff. Measure: `freq[jump_key]` histogram (already present) for how often count==3 fires, then buffer diff and cycle delta on replayed frames.

### 4.2 Strip instrumentation (trivial)
Remove the two `now()` calls, `draw[]`, `freq[]`, `reps[]` and the dead `sprite_scanlines`/`rawh` reads. Effect: ~10-30 us/frame plus a few static RMWs per row; small but free. Risk: none.

### 4.3 Closed-form skip of all-transparent ROM words (low)
When `pixels == 0`, the 8-pixel DDA emits `n = (0x1000 - xacc + zoom - 1) / zoom` outputs and leaves `xacc' = xacc + n*zoom - 0x1000` (with `xacc < zoom` on word entry). Replace the 8 DDA loops by one `udiv`, one `msub`, pointer/x advance. Why: trades ~40-100 cycles of branchy loop for ~15 cycles straight-line. Effect: proportional to the transparent-word fraction (guess 10-25% of words -> 10-20% of pixel-loop time). Risk: low; prove the formula by exhaustive check over `xacc` in [0,0x3F0), zoom in both tables, against the loop. Measure: count transparent words per frame first.

### 4.4 Software prefetch of the next row group's destination and next ROM row (low)
At the top of each row group issue `__builtin_prefetch(pixels + (y+count)*scrn_width + xpos, 1)` for one or two lines, and for `spritedata[addr + pitch]`. Why: gives the in-order core ~a row's worth of cycles to cover the ~100 ns miss the stride prefetcher only catches after several rows. Effect: bites only where 3.2 bites (hi-res): 5-15% of sprite time there, ~0 in lo-res. Risk: none. Measure: L1D/L2 refill counters (A53 PMU 0x03/0x17) and cycles, with and without a synthetic L2 thrasher on another core.

### 4.5 Hoist per-row work; cache the pre-pass (low)
`count` has a closed form (rows until carry: `min(countmax, (0x200 - yacc + zoom - 1)/zoom)`); `shadowfound` only changes when `addr` changes; the pre-pass over `sprite_height` rows can be skipped once a (bank, addr, height) key has been validated (small direct-mapped cache, invalidated in `reset()`). Effect: 100-400 cycles/sprite, ~1-3% of sprite time, more for frames of many small sprites. Risk: none to output.

### 4.6 Clip fast-forward (low-medium)
For clipped rows compute, with the same closed form as 4.3, how many ROM pixels lie wholly left of x1 and jump the DDA state past them; stop the row at the first word whose first output is >= x2 (the loop already checks per word). Effect: only for sprites straddling edges; in widescreen maybe 5-15% of clipped-path time. Risk: low; same exhaustive proof.

### 4.7 Mirror on the fly and drop `sprites_flipped` (medium)
A flipped row is the forward row read in reverse word order with nibbles reversed per word: `rev32` + nibble swap (`(x>>4)&0x0F0F0F0F | (x<<4)&0xF0F0F0F0`) = ~5 scalar instructions per word (0.6 per output pixel). Terminator check stays `(pixels & 0xF0)`, which on the mirrored word is the original p1 — exactly what the lazily built table gives today. Effect: halves the ROM working set in L2 and removes the 1 MB lazy-write pass (re-triggered after every menu visit by `reset()`, `menu.cpp:331`). Per-pixel cost is a wash; the gain is fewer L2 misses on a contested L2 and less code. Risk: low, easily diffed. Measure: L2 refill count for the render call with the NTSC thread running.

### 4.8 NEON per-word DDA kernel (high effort, largest per-pixel gain)
For one ROM word compute for lanes j=0..15: `a_j = xacc + j*zoom` (u16, max 16x0x3F0 fits), `idx_j = a_j >> 9`, gather nibble via `tbl` from the word's 8 nibbles (split to bytes with `ushr/and`), `val = nib | color`, `opaque = (nib-1) <u 14`, `shadow = nib == 0xA`, `valid = j < n` (n from 4.3). Load 32 B of destination, blend `dst' = ((dst | (shadow&valid ? 0x1000 : 0)) & ~(opaque&~shadow&valid)) | (val & opaque&~shadow&valid)`, store 32 B. Repeat the blend per output row of the group (each row's dest may differ). Why on A53: replaces ~10 scalar instructions plus 2-3 unpredictable branches per pixel with ~35 NEON ops (~70 issue slots on the 64-bit datapath) per 8-16 pixels and **no data-dependent branches**; 2.5-5 cycles/pixel vs 8-12. Handles shadow, flip (with 4.7), fractional zoom and multi-row in one kernel, collapsing the 12-way switch to clip/no-clip. Hi-res zoom in [0x80,0x100) emits up to 32 outputs/word -> two 16-lane passes. Caveats: the RMW load makes dest misses stall harder, so pair with 4.4; masked lanes write back unchanged values, so every lane address must stay inside the allocation (front pad 128 B, tail 2 rows) — clipped rows need explicit x-range masks or the scalar path. Expected: 2-3x on the pixel loop, i.e. 1-2 ms/frame in hi-res. Risk: medium; the DDA index math must be provably identical (`(xacc + j*zoom) >> 9` vs the incremental loop — same integers) and shadow OR semantics preserved. Measure: exhaustive lane/loop equivalence test for all (xacc, zoom), then full-frame buffer diffs over a recorded frame set, then cycles and branch-miss counters.

### 4.9 Band-ordered composition across layers (high effort, best for the loaded bus)
Render road/tiles/road-fg/sprites/text per horizontal band (e.g. 32 rows x 1.3-1.6 KB = 40-50 KB, L2-resident, mostly L1) instead of full-frame passes. Sprite entries are decoded once per frame into a compact array with y-ranges so each band only walks the overlapping ones. Why: turns the destination RMW from DRAM misses into cache hits for every layer, and cuts the prepare thread's L2 pollution against the NTSC thread. Output is identical: per-pixel layer order is preserved inside each band and rows are independent (row groups just clip at band edges). Effect: removes the memory-stall term for the whole prepare pass (0.5-1.5 ms/frame hi-res, guess). Risk: low to output, high to code structure (touches `hwroad`, `hwtiles`, `video.cpp`). Do only after 4.4/4.8 measurements show dest misses still dominate.

### 4.10 Integer-zoom special kernels (conditional)
Straight-line 1:1 and 2:1 kernels (2:1 with 32-bit paired stores) cost little to write but the tables show exact 0x200/0x100 on one row each, so coverage is probably tiny. Only worth it if the per-frame zoom histogram (section 5) says otherwise; 4.8 subsumes it.

---

## 5. Measurements before committing to anything

Per real frame, from recorded sprite RAM + config (lo/hi, wide, `hiresprites`), replayed through `hwsprites::render()` on a private buffer prefilled with a recorded background:

- Counts: sprites drawn; row groups by `jump_key` (the existing `freq[]`); output-pixel DDA iterations split opaque / transparent / shadow; ROM words read and the all-zero fraction; zoom histogram (halved value in hi-res); count==3 fraction; clipped and flipped fractions; off-screen iterations for clipped sprites; distinct 64 B destination lines and 4 KB pages touched.
- Time: `render()` per frame (one clock pair per frame, not per sprite), min/median/p95 over the set, on the Pi at a fixed governor, pinned with `taskset`, in three conditions: idle machine; a synthetic L2/DRAM thrasher on another core; and the real game with the NTSC thread live (semaphore-to-semaphore prepare-thread time from `main.cpp:598-613`, plus the dropped-frame % already printed at `main.cpp:675`).
- PMU (`perf stat -e`): cycles, instructions, `r10` branch mispredicts, `r03` L1D refills, `r05` L1D TLB refills, `r17` L2 refills, `r18` L2 writebacks, normalised per output pixel. These decide between 4.4/4.9 (memory) and 4.3/4.8 (issue/branch).
- Exactness gate for every change: byte compare of the slot buffer against the reference build for the whole recorded set, both resolutions, widescreen on/off, `hiresprites` on/off.

---

## 6. Hazards noticed in passing

- **3-row groups advance 2 rows** (`hwsprites.cpp:732-737` and 809, 888, 965): output-identical today by luck of the accumulator; any change to what the third row draws would become a visible bug.
- **`ydelta = -1` is unsupported**: `countmax` casts `ytarget - y` to `uint16_t` (`hwsprites.cpp:632`), and the `y++` inside cases 1/2 would cancel `y += ydelta`, looping forever. Safe only because `set_hrender()` always ORs 0x80 (`osprites.cpp:1020`). The `xdelta < 0` loop conditions (`hwsprites.cpp:665` etc.) are dead after 509-519.
- **`x1`, `x2` are never initialised** in the constructor (`hwsprites.cpp:85-87`); `render()` uses `x2 - x1` (`hwsprites.cpp:614`). Today `menu->init()` / `outrun.init()` call `set_x_clip(false)` before the first `prepare_frame()`; nothing enforces that ordering.
- `sprite_scanlines` shifts a 16-bit value right by 16 (`hwsprites.cpp:450-451`), always 1; `rawh`, `hzoom` dead. MAME's signed pitch became unsigned via `uint16_t` promotion (`hwsprites.cpp:439`); harmless for OutRun (pitch is always positive) but a behavioural drift from the hardware model.
- **No bounds on `spriteaddr`** within the 64K-word bank: a row runs until a 0xF terminator, so corrupt ROM offsets read into the next bank or, for bank 3, off the end of `sprites[]` into `sprites_flipped`. Same as MAME, but worth a debug assert.
- Unclipped paths rely on `do_sprite()`'s bound (`osprites.cpp:783-803`) leaving at most 2 px (lo-res) / 4 px (hi-res) of overrun into the row padding; the +2 rows / 128 B pad covers it, but the relationship is implicit and the hi-res doubling of both `xpos` and `width` at `hwsprites.cpp:483, 511` is what makes it hold.
- `reset()` (also called from `menu.cpp:331`) refills 1.25 MB of side tables and forces the lazy re-conversion, so the first seconds after each menu visit carry an extra write burst.
- `write_bytes_*/read_bytes_*` (`hwsprites.cpp:64-81`) and the commented dump code are dead weight; `static` counters inside a member function would not be thread-safe if a second renderer ever existed.
