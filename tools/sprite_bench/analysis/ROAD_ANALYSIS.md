# Road Renderer Cost Model (Cortex-A53)

Scope: `src/main/hwvideo/hwroad.cpp` / `hwroad.hpp`, called from
`src/main/video.cpp` (`Video::prepare_frame`), fed once per frame by
`src/main/engine/oroad.cpp` (`ORoad::do_road`). Documentation only, no source
changed. Shipping default: `video.hires=1`, `video.widescreen=0`
(`src/main/frontend/config.cpp:213-216`) &rarr; `config.s16_width=640`,
`config.s16_height=448` (`src/main/video.cpp:119-139`, `S16_WIDTH=320` at
`src/main/globals.hpp:32`). The road hardware's own scanline count is fixed at
`S16_HEIGHT=224` (`globals.hpp:33`) regardless of display resolution &mdash;
hi-res doubles the *output*, not the number of hardware scanlines the road
chip computes.

## 1. Entry points

`render_background` and `render_foreground` are member function pointers
(`hwroad.hpp:28-29`) selected exactly once, in `HWRoad::init`
(`hwroad.cpp:125-146`), by the `hires` bool passed in from
`Video::init` (`video.cpp:84`, itself driven by `config.video.hires`):

```
hires=1 -> render_background_hires / render_foreground_hires   (hwroad.cpp:521, 595)
hires=0 -> render_background_lores / render_foreground_lores   (hwroad.cpp:244, 297)
```

There is no per-frame or resolution-independent re-selection; the pointers
are frozen for the process lifetime. In the shipping default the **hires**
pair runs; the lores pair is present but dead code for that configuration.
Both pairs are further specialised at runtime by a 2-bit `road_control & 3`
value (`hwroad.cpp:256, 368, 534, 711`), set once per frame by
`ORoad::blit_roads` (`oroad.cpp:1493-1523`) from the `road_ctrl` enum. The
default game state is `ROAD_BOTH_P0` &rarr; `road_control=1`
(`oroad.cpp:1510-1514`), which selects `case 1` in every switch &mdash;
explicitly labelled "hot path" in the source (`hwroad.cpp:396, 730`). Cases 0
and 3 are marked "infrequently called" / "never taken in Outrun"
(`hwroad.cpp:714, 754-755, 774-775`); case 2 is unlabelled but structurally
symmetric to case 1 and likewise not the default. `Video::prepare_frame`
calls `render_foreground` conditionally &mdash; it is skipped when
`config.engine.fix_bugs` is set and `oroad.horizon_base==HORIZON_OFF`
(`video.cpp:202-203`), i.e. this is a data-dependent per-frame gate, not a
per-scanline one.

## 2. Per-frame control flow

**Background, hires** (`hwroad.cpp:521-589`): one outer loop, `y` stepping by
2 from 0 to 447, i.e. **224 trips**, each reading one pair of road-RAM
entries (`roadram[0x000+y>>1]`, `roadram[0x100+y>>1]`). A 4-way switch on
`road_control&3` (re-evaluated every trip although invariant for the whole
frame) picks a "sky" colour or leaves `color=-1`. **The background layer does
not fill every pixel of every row** &mdash; a row pair is written only when
the selected road's top bit (`0x800`, "solid fill") is set for that hardware
scanline; otherwise the trip does nothing and whatever was already in the
buffer (nothing yet at this point in the frame) is left for the tile layer
and the foreground pass to fill. When taken, one `std::fill_n` writes
`config.s16_width` (640) 32-bit words = 1280 `uint16_t` pixels = exactly rows
`y` and `y+1` (`hwroad.cpp:586`).

**Foreground, hires** (`hwroad.cpp:595-789`): one outer loop, `y` from 0 to
447 **stepping by 1** &mdash; **448 trips**, i.e. every output row is visited
individually (unlike the background loop's row-pair stride). `yy=y>>1`
indexes the 224 hardware scanlines. Per row: 2-4 road-RAM loads for
`data0/data1/hpos0/hpos1`; a row-skip `continue` only if *both* roads are
solid-fill this scanline (`hwroad.cpp:631-635`, rare in normal play); a
parity branch (`y&1`, perfectly predictable, alternates every row) choosing
between recomputing `color_table` on even rows (`hwroad.cpp:678-698`) or
interpolating the previous scanline's source row/position on odd rows
(`hwroad.cpp:648-674`) for the doubled-resolution "half" lines. Then the same
frame-invariant `switch(road_control&3)` as the background pass
(`hwroad.cpp:711`), re-decided on every one of the 448 rows. In the `case 1`
hot path the inner pixel loop runs `xmax = config.s16_width>>1 = 320`
iterations (`hwroad.cpp:735-751`) &rarr; **320 x 448 = 143,360 iterations per
frame**, each iteration producing one 32-bit store that covers **2** output
pixels (the sample is deliberately doubled horizontally: `(c<<16)|c`,
`hwroad.cpp:747`). Total pixels written by this one pass: 143,360 x 2 =
286,720 = the *entire* 640x448 frame, exactly once, whenever `case 1` runs
unconditionally on a row (the normal case). So for any row where the
foreground pass is not skipped, it overwrites the whole row regardless of
what the background/tile layers already put there &mdash; background work on
those rows is wasted.

The **lores** pair is the same shape at half scale (background:
`hwroad.cpp:244-293`, 224 trips/rows, 160 32-bit stores/row; foreground:
`hwroad.cpp:297-516`, 224 trips, `case 1` at `hwroad.cpp:396-442`, 160
iterations/row = 35,840/frame) but is dead code in the shipping (hires)
configuration.

## 3. The inner loop, in detail

Dominant loop: `render_foreground_hires`, `case 1`, `hwroad.cpp:736-751`.
Per iteration (produces 2 destination pixels):

- **Loads**: `src0[thpos0]` and `src1[thpos1]`, each gated by a ternary
  bounds check `thpos < 0x200 ? roads[...] : 3` (`hwroad.cpp:737-738`).
  Whether this compiles to a real branch or a predicated/`csel` load is not
  determinable from the source; either way it is a data-dependent selection
  every iteration. `priority_lookup[pix0][pix1]` is a further table load
  (8x8 bytes, `hwroad.cpp:615-625`), and `color_table[0x00+pix]` /
  `color_table[0x10+pix]` is a third table read from the 32-entry stack array
  built once per row.
- **Branch**: `if (priority_lookup[pix0][pix1])` (`hwroad.cpp:740`) is a real
  `if`/`else`, not a select, and the code's own comment states it is
  data-dependent, "hot path 85% or so" one way (`hwroad.cpp:744`) &mdash; this
  is exactly the class of data-dependent branch called out as costly on this
  core (~7-cycle mispredict penalty), firing up to 143,360 times/frame.
- **Store**: exactly one 32-bit store per iteration (`hwroad.cpp:747`), never
  wider. No read-modify-write of the destination anywhere in this loop or
  anywhere else in `hwroad.cpp` &mdash; every write is a plain store of a
  freshly computed value; the destination pixel buffer is never read back by
  the road renderer.
- Position update: two `(x+1)&0xfff` operations (cheap ALU), no
  multiplication or division.

Per-row setup redundancy: `priority_lookup`/`priority_map` are declared
`static const` **inside** the 448-times-per-frame `y` loop
(`hwroad.cpp:608-625`); as constant-initialised PODs these are likely hoisted
to `.rodata` with no runtime guard, but this is not verifiable from source
alone. The `switch(road_control&3)` (`hwroad.cpp:711`) is unambiguously
loop-invariant for the whole frame (set once by `write_road_control`,
`oroad.cpp:1501-1521`) yet is re-evaluated on every one of the 448 rows.

## 4. Data structures and tables

| Structure | Location | Size | Touched per frame | Notes |
|---|---|---|---|---|
| `roads[]` decoded road bitmap | `hwroad.hpp:42` | `0x40200` = 262,656 B (&asymp;256.5 KB) | Up to 224 scanlines x 2 roads x &le;512 B/row &asymp; up to 229 KB; **which** of the 256 rows per road is picked is data-dependent (elevation/curve) and can jump between distant rows | **8.2x the 32 KB L1D.** Built once at ROM load (`decode_road`, `hwroad.cpp:171-194`), never rewritten per frame. Sequential within a row (prefetcher-friendly); scattered across rows, contending for the 512 KB L2 with 3 other threads. |
| `ram` / `ramBuff` road RAM | `hwroad.hpp:45-46` | 4096 B each, 8 KB total | Most of it rewritten every frame by `oroad.cpp`'s `blit_road`/`output_hscroll`/`copy_bg_color` (`oroad.cpp:1526-1616`); swapped by `read_road_control` (`hwroad.cpp:218-232`) | Fits L1D; not a bottleneck. |
| `color_table[32]` | stack, `hwroad.cpp:600`/`303` | 64 B | Rebuilt (10 entries) once per even output row (224x/frame) | Trivial; likely register-resident. |
| `priority_lookup[8][8]`, `priority_map[2][8]` | `hwroad.cpp:608-625` | 64 + 16 B | Read every inner-loop iteration | Trivial; effectively free once resident. |

Nothing else of note is allocated or sized per frame; `HWRoad::decode_road`'s
262 KB build is a one-time init cost, not part of the per-frame budget.

## 5. Cost model

Assumptions stated explicitly: 1.0 GHz A53, in-order dual-issue, exactly one
load/store pipe (&le;1 memory op/cycle), the road+tiles group costs 2.20 ms
measured, of which the foreground road pass is the dominant contributor
argued below (background is a rare, memset-shaped, well-vectorisable fill).

**Foreground hires `case 1`** dominates: 143,360 inner-loop iterations/frame,
each with (a) 2 texture loads (`src0`/`src1`), (b) 1 `priority_lookup` load,
(c) 1 `color_table` load, (d) 1 32-bit store = **5 memory ops per 2 output
pixels, ~2.5 mem-ops/pixel**. At &le;1 mem-op/cycle this is a hard floor of
~2.5 cycles/pixel &times; 286,720 pixels &asymp; 716,800 cycles &asymp; **0.72
ms/frame at 1.0 GHz** from the load/store pipe alone, before ALU or branch
cost &mdash; roughly a third of the measured 2.20 ms road+tiles budget. Per
scanline: 320 iterations x 5 mem-ops = 1600 mem-ops, plus one data-dependent
branch per iteration (~85%/15% split per the code's own comment) at up to ~7
cycles if mispredicted: worst case 320 x 0.15 x 7 &asymp; 336 extra
cycles/row, &asymp;150,000 cycles/frame &asymp; 0.15 ms &mdash; though real
misprediction rates depend on road content and are not determinable from the
code. Instruction count per pixel-pair: 2 loads + 2 bound compares + 1
table load + 1 branch + 1-2 color-table loads + 2 position updates + 1 store
&asymp; 10-12 instructions/2px (~5-6/pixel), consistent with the load/store
pipe, not dual-issue width, being the constraint.

**Background hires**: at most 224 trips x 640 32-bit stores = 143,360
stores/frame *if every row were solid-fill*, but in practice only sky rows
qualify, so real store count is well under this ceiling and the loop shape
(`std::fill_n` of a repeated word) is one a compiler can turn into wide
vector stores automatically, unlike the foreground loop's per-pixel
conditional values. This pass is not judged to be the dominant cost.

**Which variant dominates**: `render_foreground_hires`, `case 1`
(`hwroad.cpp:729-753`), because it is the only loop that (a) always runs at
full frame width on the shipping resolution and control state, (b) cannot
auto-vectorize past 32 bits due to the per-pixel data-dependent table
lookups, and (c) carries a real, data-dependent, comment-confirmed branch on
every 2-pixel step.

## 6. Ranked list of what looks expensive

1. **32-bit-only stores in the dominant loop.** `hwroad.cpp:747` (and the
   lores/case-0/2/3 equivalents at `hwroad.cpp:390, 436, 481, 509, 723,
   747`) issue one 32-bit store per 2 output pixels; nothing wider is ever
   used for the road layer, matching the sprite-work lesson that fewer/wider
   stores are the win this core rewards. At 143,360 stores/frame, batching to
   NEON 64-/128-bit stores (amortising several `pix`/branch computations per
   store) is the largest single lever visible in this file.
2. **Data-dependent branch on `priority_lookup[pix0][pix1]`**
   (`hwroad.cpp:740`, and `hwroad.cpp:410, 426, 456, 472` for the lores/case-2
   variants), explicitly noted by the author as ~85/15 &mdash; a real branch
   rather than a branchless select, firing up to 143,360 times/frame with a
   ~7-cycle misprediction cost on the ~15% side.
3. **Frame-invariant `switch(road_control&3)` re-evaluated per scanline**
   (`hwroad.cpp:256, 534` background; `hwroad.cpp:368, 711` foreground) 224 or
   448 times per frame for a value fixed once per frame by
   `write_road_control` (`oroad.cpp:1501-1521`). Hoisting this to a per-frame
   dispatch (mirroring how `render_background`/`render_foreground` are
   already selected once via function pointer at `hwroad.cpp:138-144`) would
   remove hundreds of redundant, if well-predicted, branches and let the
   compiler specialise the row loop per control state.
4. **Ternary bounds checks gating every texture sample**
   (`hwroad.cpp:721, 737-738`) &mdash; two per iteration in the hot path,
   guarding loads from the 256 KB `roads[]` table; branch vs. predicated
   load is not determinable from source, but both are on the critical path
   of all 143,360 iterations.
5. **`roads[]` table exceeds L1D by 8x** (`hwroad.hpp:42`), with
   data-dependent row selection across scanlines (`hwroad.cpp:701-703,
   739-750`) defeating within-frame locality; L2 contention with 3 other
   threads makes this worse than the raw size suggests. Not fixable by
   store-width changes alone; would need a smaller working table or
   scanline-order reuse (not determinable from the code whether adjacent
   scanlines commonly reuse the same row index).

No destination read-modify-write was found anywhere in `hwroad.cpp` &mdash;
every store is a fresh write of a computed value, so there is nothing to
eliminate on that specific axis for the road layer (unlike, potentially, a
blended sprite/tile layer).

## 7. Correctness hazards / dead code noticed in passing

- **Stale comment in `render_background_lores`** (`hwroad.cpp:288-289`):
  "Fill both y and y+1 scanlines... width * 2 pixels = 2 lines" is
  copy-pasted from the hires version but the lores loop steps `y` by 1 and
  `std::fill_n(out32, w>>1, ...)` only fills `w>>1` 32-bit words = one row
  (`w` pixels), not two. The code is correct for lores (no doubling needed);
  only the comment is wrong. Since lores is dead in the shipping
  configuration this has no runtime effect today but would mislead anyone
  re-enabling `hires=0` or refactoring from the comment.
- **`x_offset` (`hwroad.hpp:36`) is set to 0 once in `init` and never written
  again** (confirmed by a full-tree search); it is added into every `hpos`
  calculation (`hwroad.cpp:374,399-400,446-447,493,717,732-733,756-757,778`)
  as a permanent no-op. Harmless, but dead state kept live in every hot-loop
  expression; a different variable from the unrelated `config.video.x_offset`
  UI setting used in `rendersurface.cpp`.
- **Row-skip `continue` inside a `switch` inside a `for` loop**
  (`hwroad.cpp:371-372, 490-491, 715-716, 776-777`, cases 0/3 in both
  resolutions): the `continue` targets the enclosing `for (y...)`, which is
  correct C++ but easy to break if the switch is ever wrapped in another
  loop during refactoring; worth a comment if the loop nest changes.
- Nothing else resembling a buffer-overrun, uninitialised read, or
  off-by-one was found in the code read for this document; the RAM
  double-buffer swap in `read_road_control` (`hwroad.cpp:218-232`) and the
  `& 0x7FF` / `& 0xFFF` masking throughout look internally consistent with
  the declared table sizes.
