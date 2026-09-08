# Implementation specification: sprite renderer rewrite (S0-S6)

Branch `ntsc-rearch` (harness pattern from `sprite-a53`). Companion to `DESIGN_FABLE.md`
(structure), the in-situ bandwidth review (design of record; its conclusions are now inlined at their use sites), `PERFORMANCE_MEASUREMENTS.md` campaign 3
(the measurements that fix stage 1's content), `DESIGN_GLOW.md` (glow rule) and
`SCHEDULE_VERIFICATION.md` (scaling law). Structural template: `tools/road_tiles/analysis/IMPLEMENTATION_SPEC.md`.
Line numbers refer to `src/main/hwvideo/hwsprites.cpp` (1003 lines), `hwsprites.hpp` (46),
`video.cpp` (703), `main.cpp` (1051), `engine/osprites.cpp` (1036) as they stand on this branch;
re-anchor on the quoted code if the file has drifted.

Each section is self-contained: an implementer reads S0 plus their section. Sections are in
dependency order (S0.10).

Facts that are fixed and must not be re-derived:

- Target Cortex-A53 AArch64 (Pi Zero 2 W `test mule`, Pi 3 `pi3`, Pi 2 v1.2 `pi2-local`). The tree
  must also build on x86-64 GCC/Clang/MSVC and ARM without NEON. NEON is guarded by `CB_HAVE_NEON`
  from `hwarch.hpp:66-71` with a scalar fallback; `CB_FORCE_SCALAR` selects the fallback everywhere.
- Frame buffer: 16-bit palette slots, `pixels` 64-aligned with a 64-halfword front pad and two spare
  rows (`video.cpp:66-73`). Modes: 640x448 / 808x448 hi-res, 320x224 / 404x224 lo-res. Every layer
  stores values below `0x1000`; sprites OR bit 12 (`0x1000 = S16_PALETTE_ENTRIES`) for shadow. No
  value ever reaches `0x2000` (the harness asserts it; it is the precondition for `|= 0x1000` equalling
  the upstream `&= 0xfff; += 0x1000`).
- `sprite_layer->render(pixels, 8)` is the single call site (`video.cpp:246`), after the road
  foreground and before the text layer. Only sprites whose priority field is 3 are drawn (`427-428`).
- Scaling law (`SCHEDULE_VERIFICATION.md`, exhaustive): horizontal output index of source pixel `i` is
  `N(i) = ceil(512*i/zoom)`, run `r(i) = N(i+1)-N(i)`; destination row `d` reads source row
  `src(d) = (d*zoom) >> 9`. `xacc` resets per row so `r(i)` depends only on `(zoom, i)`. Zoom regimes:
  hi-res shipping (`ZOOM_LOOKUP >> 1`) `0x80..0x1F8`, runs 1-4; lo-res `0x100..0x3F0` and
  `hiresprites` up to `0x7E0`, runs 0-2, run 0 legal.
- Structure: LINEBUF (`PERFORMANCE_MEASUREMENTS.md` campaign 1: 5.81 cyc/px vs FANOUT 7.36, REF 11.70). In-situ the
  shipped pass is 11.6 cyc/px, 0.93-0.95 ms typical (82K px), destination-store-bound 60-80% of L2
  traffic, exposed memory term 0.10 ms typical / 0.37 ms extreme (`PERFORMANCE_MEASUREMENTS.md` campaign 3, M-A).
  Target 7.3-8.8 cyc/px in-game (from the in-situ bandwidth review).

Structural facts that are easy to get wrong:

1. **The shipped output is history-dependent (finding F1).** The pre-pass (`hwsprites.cpp:490-494,
   535, 580`) builds the lazy `sprites_flipped` / `sprites_shadowinfo` entries for source rows
   `0 .. sprite_height-1` with `sprite_height = (H*zoom) >> 9` (`H = ytarget - top`, hi-res doubled;
   `zoom` hi-res halved), but the row loop reads source row `src(d) = (d*zoom)>>9` up to
   `src(H-1)`, which equals `sprite_height` exactly when `(H*zoom) mod 512 >= zoom`. That row is then
   unbuilt unless an earlier sprite in the *session* built it: a flipped sprite reads the `0xffffffff`
   sentinel (`541`) and draws nothing on those destination rows; a non-flipped shadow-flagged sprite
   sees `shadowinfo == 0xff` (`642`) and draws its `0xA` nibbles as colour. `reset()` (`121-128`,
   called from `menu.cpp:331`) restarts the history. Requirement 5 (drop the tables) makes this class
   unreproducible by construction. Resolution: the correctness oracle is the pixel-serial reference
   `render_px` (S0.4), which classifies every row it draws; S0.7 measures how often F1 manifests on
   captured frames and in the whole-game checksum run, and every difference between the shipped
   output and the oracle must be attributed to F1 (or F2, item 6) by the classifier. These are the
   only permitted differences from the shipped glow-off output. If S0.7 measures zero occurrences,
   the rewrite is bit-exact with the shipped binary outright.
2. **Right-to-left sprites are normalised, not drawn right-to-left.** `509-519`: `xdelta == -1`
   becomes `xdelta = 1`, `xpos -= width << hires`, `flip` toggles and `addr` moves to the other end
   of the row. The geometry of the shipped output is therefore *this* normalised geometry (not
   upstream's), and the rewrite reproduces it exactly. Direction survives only as the `rtl` bit that
   the glow rule consumes (S2); with glow off, draw order is unobservable.
3. **Word-level control, pixel-level clipping.** The terminator `(w & 0xf0) == 0xf0` is tested after
   each whole word (`678`), on the *mirrored* word for flipped rows (equivalent to upstream's
   `0x0f000000` test on the unmirrored word); the screen-edge test is per word at the loop head
   (`665`); clipping to `[x1, x2)` is per pixel (`214`). The `clip` bit (`437`, engine
   `osprites.cpp:794-800`) only selects the unclipped fast path; the rewrite ignores it except as a
   statistic and always clips exactly.
4. **Bit 12 is not guaranteed clear at sprite-pass start.** When `fg_enabled` is false
   (`video.cpp:199`: `fix_bugs` and `horizon_base == HORIZON_OFF`, the map and music screens) the road
   foreground does not repaint road rows, so a row may carry bit 12 from an earlier frame. Any
   optimisation that assumes "no shadow this frame means no bit 12" (S3's row bitmap) must be told
   whether the destination is clean.
5. **The 3-row groups double-draw one row** (`732-737` advance 2 rows for `count == 3`): output-
   identical because colour writes and shadow ORs are idempotent. The rewrite writes each
   `(row, x)` at most once per sprite; the glow rule (S2) depends on that.
6. **The shipped unclipped path can write outside the row (finding F2).** With `clip == 0` the
   `_nc` cases (`741-816, 897-972`) apply no x test at all and rely on the engine's `fully_inside`
   bound (`osprites.cpp:770-800`), which is computed from `calc_width` (a floor,
   `osprites.cpp:747`) and `2*offset`, while the renderer draws `ceil(512*W/zoom)` pixels (hi-res
   `ceil(1024*W/z)`) and adds `offset` only when `hiresprites` is on (`601-602`). The bound is loose
   by up to about 4 hi-res pixels at either edge (independent hazard audit of the pre-rewrite kernel), so for a frame
   or two while a sprite crosses an edge the shipped renderer stores its edge pixels into the
   previous row's right end, the next row's left end, or the pad. Requirement 8's exact clipping
   cannot reproduce this. Resolution as for F1: the classifier in S0.7 must attribute every such
   difference, and S0.7 reports the count.

---

## S0 - Build and test harness

### S0.1 Files

| Action | Path |
|---|---|
| Edit | `src/main/hwvideo/hwsprites.hpp`, `hwsprites.cpp` - `render_ref` twin, `render_px` oracle, accessors, friend |
| Edit | `src/main/hwvideo/hwarch.hpp` - `cb_bswap32`, `cb_prefetch_r/w` |
| Edit | `src/main/video.cpp`, `video.hpp` - `-dumpspr` capture, `dest_clean` argument, optional stats |
| Edit | `src/main/main.cpp` - `-dumpspr`, `-noglow`/`-glow`, `-spritereplay N` flags |
| Edit | `CMakeLists.txt` - options `SPRITE_DUMP`, `SPRITE_REF_PX` (default OFF); fix `PIXEL_ACCURACY` (S0.2) |
| New | `tools/sprite_bench/test/sprite_test.cpp` - the driver |
| New | `tools/sprite_bench/test/stubs/frontend/config.hpp` - stub `Config` |
| New | `tools/sprite_bench/test/CMakeLists.txt` - standalone project (copy `tools/road_tiles/test/CMakeLists.txt`, two targets) |
| New | `tools/sprite_bench/test/golden/` - checked-in hashes |

### S0.2 Macros

- `CB_RENDER_TEST` (existing scheme, `hwarch.hpp:85-100`): compiles `render_ref` and the two lazy side
  tables it needs, and declares `friend struct SpriteTestAccess;`. Driver only.
- `CB_SPR_DUMP`: compiles the `-dumpspr` capture (S0.6). CMake option `SPRITE_DUMP`, default OFF.
- `CB_SPRITE_REF_PX`: the game routes every sprite through `render_px` (S0.4). CMake option
  `SPRITE_REF_PX`, default OFF. Used only to produce reference checksums (S0.8).
- `CB_SPRITE_STATS`: per-frame counters (S0.9). Driver always; game via `-DCB_SPRITE_STATS=1`.
- `CB_FRAME_CKSUM`, `CB_MAX_FRAMES`: existing whole-frame gate (`video.cpp:261-272`, `main.cpp:675-687`).
- `PIXEL_ACCURACY`: the CMake option (`CMakeLists.txt:68, 280-284`) already passes `=1` by default;
  delete the unguarded `#define PIXEL_ACCURACY 0` at `hwsprites.cpp:58` and replace it with
  `#ifndef PIXEL_ACCURACY / #define PIXEL_ACCURACY 1 / #endif`. From S0 on, glow is a *runtime* member
  `bool glow` initialised to `PIXEL_ACCURACY != 0` in the constructor and settable by `set_glow(bool)`
  (game flags `-noglow` / `-glow` in `main.cpp` next to `-perftest`, `852`). The driver tests both
  values in one binary.

### S0.3 The `_ref` twin

Under `CB_RENDER_TEST`, copy `hwsprites::render` (`414-1003`) verbatim as private
`render_ref(uint16_t* pixels, uint8_t priority)`, together with `sprites_flipped`,
`sprites_shadowinfo` (`hwsprites.hpp:33-40`, moved under the same guard once S1 drops them from the
game), the twelve `draw_pixel_*` macros (`213-408`, same guard) and a `reset_ref_tables()` that
performs the two `std::fill_n` of `reset()` (`126-127`). Delete from the copy only the dead
instrumentation: `reps` and its `i` counter (`275, 278, 289, 416`), `freq`, the two
`high_resolution_clock::now()` and `draw[jump_key] +=` (`417-418, 423, 656, 981`) and the commented
block `984-1002`. Exactly one
behavioural edit is permitted, behind a member `bool ref_prepass_fix = false`: when true, the two
pre-pass loop bounds `y < sprite_height` (`535`, `580`) become `y <= sprite_height`. This is the F1
switch: `ref_prepass_fix = true` builds every row the draw loop can read, so the twin becomes
history-independent and must then equal `render_px(glow = false)` everywhere (S0.7).

### S0.4 The pixel-serial oracle `render_px`

Compiled always (it is also the in-game fallback for out-of-envelope sprites, S1.2). Signature
`void render_px(uint16_t* pixels, uint8_t priority, bool glow)`; per-sprite body
`draw_sprite_px(const SpriteRec&, uint16_t*, bool glow)`. It is the definition of correct output for
both reference points: `glow = false` is the shipped output (modulo F1), `glow = true` is the
`PIXEL_ACCURACY 1` output with hardware draw order.

**Record parse** (shared with S1 as `parse_record(uint16_t data, SpriteRec&)`, returns false to
skip). Verbatim from `425-462, 468-469, 476-487, 509-519, 601-611`, in this order:

```cpp
if (ramBuff[d+0] & 0x8000) -> end of list
sprpri = 1 << ((ramBuff[d+3] >> 12) & 3); if (sprpri != priority) skip;
hide = ramBuff[d+0] & 0x5000; height = (ramBuff[d+5] >> 8) + 1; if (hide) skip;
bank = ((ramBuff[d+0] >> 9) & 7) % 4;  top = (ramBuff[d+0] & 0x1ff) - 0x100;  clip = ramBuff[d+0] & 0x2000; // stat only
addr = ramBuff[d+1] (int32);  pitch = ((ramBuff[d+2] >> 1) | ((ramBuff[d+4] & 0x1000) << 3)) >> 8;   // 0..255
shadow = (ramBuff[d+3] >> 14) & 1;  zoom = ramBuff[d+3] & 0xfff;
ydelta = (ramBuff[d+4] & 0x8000) ? 1 : -1;  flip = (~ramBuff[d+4] >> 14) & 1;  xdelta = (ramBuff[d+4] & 0x2000) ? 1 : -1;
width = ramBuff[d+4] & 0x7ff;  color = 0x800 + ((ramBuff[d+5] & 0x7f) << 4);
xpos = ramBuff[d+6];  offset = (int16_t)ramBuff[d+15];
if (xpos < 0x80 && xdelta < 0) xpos += 0x200;  xpos -= 0xbe;
ytarget = top + ydelta * height;  xpos += config.s16_x_off;
if (config.video.hires) { xpos <<= 1; top <<= 1; ytarget <<= 1; zoom >>= 1; }
rtl = (xdelta == -1);
if (rtl) { xpos -= width << hires; if (flip) { flip = 0; addr -= pitch - 1; } else { flip = 1; addr += pitch - 1; } }
if (config.video.hiresprites == 1) xpos += offset;
if (flip) addr = addr - pitch + 1;          // row start, both flip states; may be negative (int32)
H = ytarget - top (positive iff ydelta == 1);  if (zoom == 0) skip;   // shipped would loop forever
```

**Per sprite.** `pend = x2 - xpos`; if `pend <= 0` nothing is drawn (every row starts at `xpos`).
For `d` in `0 .. |H|-1`: `y = top + ydelta*d`; skip if `y < 0 || y >= s16_height`. Source row `j` is
tracked with the reference accumulator (`976-979`: `yacc += zoom; addr_j += pitch*(yacc>>9); yacc &= 0x1ff`
after each `d`), not the closed form. Word index of word `k` of row `j`:
`widx = (bank << 16) + addr + pitch*j + (flip ? pitch-1-k : k)` in `int64`; an index outside
`[0, SPRITES_LENGTH)` ends the row (shipped behaviour there is undefined memory access; this is the
one defined divergence, unreachable with real data). Expand the row into `ex[]` (nibbles, output
order): `p = 0, xacc = 0, k = 0`; loop: `if (p >= pend || k == SPR_WMAX) break; w = sprites[widx(k)];
if (flip) w = nibrev(w)`; for nibble `n` from bits 28..31 down to 0..3: `while (xacc < 0x200) {
ex[p++] = n; xacc += zoom; } xacc -= 0x200;`; `k++`; `if ((w & 0xf0) == 0xf0) break;`.
`nibrev(w) = bswap32(((w >> 4) & 0x0F0F0F0F) | ((w << 4) & 0xF0F0F0F0))`. `SPR_WMAX = 256` (tunable)
bounds unterminated rows (never in ROM data; fuzz only); shipped would read on until the screen edge.
Then apply in hardware order: `q` ascending `0..p-1` for `rtl == 0`, descending for `rtl == 1`:

```cpp
x = xpos + q; if (x < x1 || x >= x2) continue;  n = ex[q];  row = pixels + y*s16_width;
if (shadow && n == 0xa)        row[x] |= 0x1000;
else if (n != 0 && n != 15)  { if (glow && x > x1) row[x-1] &= 0xfff;  row[x] = n | color; }
```

This is upstream `scratchpad/upstream/hwsprites.cpp:155-170, 250-330` with the fork's record layout,
normalisation and `|=` shadow; `ex[]` is `uint8_t[SPR_EX_LEN = 12288]`: `p < pend + 8*512` (one word
at `zoom = 1`), and `pend = x2 - xpos <= 808 + 2*(0xbe - 0x80) + 2*0x7ff + 4 = 5026` because
`xpos >= -(0xbe - 0x80)` after the `0x200` wrap for right-to-left records, `-0xbe` otherwise, less
`width << 1`, less `|offset|`. Since expansion has no feedback into drawing, "expand then apply" equals upstream's
interleaved form. With `glow == false` the order is unobservable, so `render_px(false)` equals
`render_ref(ref_prepass_fix = true)` for every engine-emitted record; S0.7 proves it.

### S0.5 Test access and accessors

Add unconditionally: `const uint16_t* sprite_ram_buffer() const { return ramBuff; }`,
`uint16_t get_x1() const`, `get_x2() const`, `void set_glow(bool)`, `bool get_glow() const`,
`void set_replay_mode(int)` (S1), `void load_sprite_ram(const uint16_t* buf)` (copies 0x800 halfwords
into `ramBuff`). Under `CB_RENDER_TEST`: `friend struct SpriteTestAccess;` giving the driver
`render_ref`, `ref_prepass_fix`, `reset_ref_tables`, `render_px`, `draw_sprite_px`, `parse_record`,
and (from S1) `decode_row`, `replay`, the scratch buffers and the counters.

Remove `#include "video.hpp"` and `<chrono>` from `hwsprites.cpp:1, 5` (nothing in the file uses
`video`; `video.hpp:16-19` drags in `roms.hpp` and would break the driver) and the file I/O macros
`60-81`. `hwsprites.hpp:4, 21-22`: drop `<chrono>`, `setup`, `draw[]`.

### S0.6 Sprite capture (`-dumpspr <dir> <interval> <count>`)

Under `CB_SPR_DUMP`, parsed in `main.cpp` next to `-dump` (`856-880`), three globals. In
`Video::prepare_frame`, immediately before `sprite_layer->render` (`video.cpp:246`) write
`<dir>/spr_%06u.bin` every `interval`-th frame until `count` files exist, and immediately after the
call append the post-sprite frame to the same file. Format v1, little-endian:

```
offset  size          field
0       8             magic "CBSPRD01"
8       2             s16_width      10  2   s16_height     12  2   s16_x_off
14      1             hires          15  1   widescreen     16  1   hiresprites
17      1             fg_enabled (the dest_clean value, S3)   18  1   glow (sprite_layer->get_glow())
19      1             reserved 0
20      2             x1             22  2   x2             (sprite_layer->get_x1()/get_x2())
24      4             frame_no (video.prof_frames)
28      4096          ramBuff (0x800 x uint16, sprite_ram_buffer())
4124    W*H*2         pixels before the sprite pass (s16_width * s16_height halfwords)
+       W*H*2         pixels after the sprite pass (the shipped output, real session history)
```

Capture with the *unmodified* renderer (`git stash` S1+ or build from the S0 commit), on WSL x86-64
and on the Pi: `./cannonball-se -t 1 -perftest -dumpspr <dir> 37 300` through the
attract cycle, plus a recorded gameplay session, in each of hi-res, hi-res wide, lo-res,
`hiresprites` (config toggles). Store captures outside the repo; the post-sprite buffer is the
shipped output *with* the real lazy-table history, which no harness can otherwise reproduce (F1).
`hwsprites::swap()` runs in `tick()` before `prepare_frame`, so `ramBuff` is stable during capture.

### S0.7 The driver (`tools/sprite_bench/test/sprite_test.cpp`)

Compiles `src/main/hwvideo/hwsprites.cpp` unmodified plus the driver, include order `stubs` then
`src/main`. Stub `frontend/config.hpp`:

```cpp
#pragma once
#include "stdint.hpp"
struct Config { uint16_t s16_width, s16_height, s16_x_off; struct { int hires; int hiresprites; } video; };
extern Config config;
```

(`hwsprites.cpp` references exactly `config.s16_width`, `s16_x_off`, `s16_height`, `video.hires`,
`video.hiresprites`; grep before building.) Definitions `CB_RENDER_TEST=1 CB_SPRITE_STATS=1`; targets
`sprite_test` (native, `-mcpu=cortex-a53` on aarch64) and `sprite_test_scalar` (`-DCB_FORCE_SCALAR=1`).
Inputs: `--roms <dir>` with the eight sprite ROMs interleaved 4-way as `roms.cpp:65-73`
(`mpr-10371.9 @0 lane 0, 10373.10 lane 1, 10375.11 lane 2, 10377.12 lane 3; 10372.13 @0x80000 lane 0,
10374.14, 10376.15, 10378.16`), 1 MB image passed to `hwsprites::init`; without `--roms`, a seeded
random image where nibble 6 of every 6th word is forced to 0xF so rows terminate (plus a second
synthetic image dense in 0/F/A nibbles with terminators at every position, `DESIGN_FABLE.md` 6.4b).
`--frames <dir>`, `--seed N`. Sub-commands:

- `selftest`: `render_ref(fix=0)` vs the shipping `render()` before S1 exists (twin fidelity), then
  `render_ref(fix=1)` vs `render_px(glow=false)` on 20,000 random lists (fuzz generator below) with
  the F1 condition tallied. Zero mismatches required for the second comparison.
- `unit`: per-section tests (S1.4 onwards) plus S0's own: (a) `glow_check.py` Test 1 ported: every
  type sequence up to length 7 over a 9-pixel row, every bit-12 pattern, both directions, clip
  windows `(0,W)`, `(1,W)`, `(0,W-2)`, `draw_sprite_px(glow=true)` vs a direct transcription of the
  upstream per-pixel loop; (b) Test 2: random 2-5 sprite lists, per-sprite deferral (S2's ring) vs
  inline must match, and an end-of-frame deferral control must *fail*.
- `fuzz <n>`: `n` random sprite lists of 1-122 records over the real or synthetic ROM, edge-biased:
  `xpos` placing the sprite off-screen, straddling `x1`/`x2` (including `x1 > 0` windows set via
  `set_x_clip`-equivalent direct assignment), `top` straddling 0 and `s16_height`; `zoom` uniform in
  `1..0xfff` with extra mass on the three shipping tables and on `0x200`, `0x401..0x7ff`;
  all `(flip, xdelta, shadow, hiresprites)` combinations; `pitch` 0..255; `addr` uniform including
  values whose row start goes negative or past the bank; `ydelta = -1` in 5% of records; four modes.
  Backgrounds: seeded random halfwords `< 0x2000` with bit 12 set in stripes. Compares the
  function under test against `render_px` at both glow values; whole-allocation `memcmp` (front pad,
  rows, spare rows); prints record, mode, first differing `(y, x)` and both values on failure.
- `replay`: per captured frame, load config/x1/x2/`ramBuff`, buffer A = captured pre-sprite frame:
  render with the function under test into A, `memcmp` against the captured post-sprite frame
  (glow-off captures) or against `render_px(glow=true)` on a copy (glow-on). A mismatch against the
  captured post frame is then classified: re-render with `render_px(false)`; if the new output equals
  `render_px`, each differing pixel is attributed to F1 iff it lies on a destination row `d` of
  some sprite with `src(d) == sprite_height` (recomputed from the record with the shipped formula),
  or to F2 iff some `clip == 0` sprite, re-rendered by `render_px` alone into a scratch with the
  window widened to `[x1 - 16, x2 + 16)`, produces at the unwrapped position (`(y - 1, x + s16_width)`
  for `x >= s16_width - 16`, `(y + 1, x - s16_width)` for `x < 16`) exactly the shipped value; print
  per-frame F1 and F2 pixel and sprite counts. Any unclassified pixel fails.
- `golden`: FNV-1a 64 (`video.cpp:266-270` form) of `render_px(false)` and `render_px(true)` output
  per captured frame, checked in as `golden/<set>.txt`; `replay` recomputes and fails on drift.
- `bench`: 2,000 iterations over a captured frame set, cycles per output pixel for `render_px`,
  `render_ref` and the function under test; on the Pi also `perf_event_open` 0x03/0x17/0xE8.

### S0.8 Whole-game gate

Build three binaries with `-DCB_FRAME_CKSUM=1`: the S0 commit (shipped renderer), the same with
`-DSPRITE_REF_PX=ON` (oracle in-game, `-noglow` and default), and the stage under test. Run each from a
cold start: `CB_MAX_FRAMES=3000 ./cannonball-se -t 1 -perftest [-noglow]`, hi-res non-wide, and repeat
for hi-res wide, lo-res and `hiresprites`. Gate: stage checksum equals the `SPRITE_REF_PX` checksum at
the same glow setting, in all four modes. Report: shipped checksum vs `SPRITE_REF_PX -noglow`
checksum; equal means neither F1 nor F2 manifested in that run (then the stage is bit-exact with
the shipped binary); different means `replay`'s F1 + F2 count over the captured set must be
non-zero, or the run fails. `-t 1 -perftest` is the deterministic condition (`main.cpp:677`).

### S0.9 Measurement

In-game: the 2-second report (`main.cpp:692-720`) `SPRITES` column from `prof_ns_sprites`
(`video.cpp:257`). Add, under `CB_SPRITE_STATS`, `Video::prof_spr_px` (output pixels visited: for
each replayed destination row the clipped row extent `min(row_end, cx1) - max(0, cx0)`; the shipped
counter of `PERFORMANCE_MEASUREMENTS.md` campaign 3 counted DDA iterations, close but not identical - re-baseline
once with `SPRITE_REF_PX` instrumented the same way) and print px/frame and cyc/px in the report so
windows can be banded as typical (60-100K), moderate (150-300K), extreme (>300K). Optional
`CB_SPRITE_PMU` (Linux only) opens the `perf_event_open` group of `PERFORMANCE_MEASUREMENTS.md` campaign 3
(cycles, 0x03, 0x17, 0xE8) around the render call. Procedure per stage: `test mule`, governor
`performance`, `vcgencmd get_throttled` 0x0, shipping config (hi-res, Blargg on, `-60 -t 4`),
two 300-s attract captures, medians per band, before and after.

### S0.10 Order and dependencies

```
S0 -> S1 -> S2 -> S3 -> S4a (NEON replay) -> S4b (decode word classes) -> S4c (NEON expansion, optional)
            S1 -> S5 (independent of S2-S4; re-run after S4)
            S1 -> S6
```

Before S1: implement S0, run `selftest` on WSL and the Pi, capture frames (S0.6), commit `golden/`.
Every stage ends with `unit`, `fuzz 200000`, `replay` (both driver targets, both hosts) and S0.8.

---

## S1 - LINEBUF core: schedule tables, decode once, replay D/D', side tables dropped, ROM prefetch

### S1.1 Files and functions

`hwsprites.hpp`/`.cpp`: `render` (`414-1003`) replaced; `init` (`94-118`), `reset` (`121-128`),
`set_x_clip` (`131-151`); header members `33-40` removed from the game build; `hwarch.hpp`
(`cb_bswap32`, `cb_prefetch_r`); `main.cpp` (`-spritereplay N`).

### S1.2 Behaviour

**Constants** (all tunable, defaults): `SPR_LB_LEN 4160` (line buffer halfwords), `SPR_WMAX 256`,
`SPR_REC_LEN 8*SPR_WMAX + 8`, `SPR_SCHED_LEN 8*SPR_WMAX`, `SPR_FAST_ZOOM_MIN 0x80`,
`SPR_PEND_MAX SPR_LB_LEN - 64`, `SPR_ROM_PF_ROWS 2`.

**Dispatch.** `render(pixels, priority, dest_clean = false)` clears the frame state (S3), then for each
record: `parse_record`; if `ydelta == -1 || zoom < SPR_FAST_ZOOM_MIN || pend > SPR_PEND_MAX` call
`draw_sprite_px` (fallback; `stat_fallback++`, expected 0 in-game: `set_hrender` forces bit 15
(`osprites.cpp:1020`), no table zoom is below 0x100 raw), else `draw_sprite_fast`. `pend <= 0` skips.
`init` calls `set_x_clip(false)` after conversion so `x1`/`x2` are never uninitialised
(independent hazard audit of the pre-rewrite kernel). `reset` clears only `ram`/`ramBuff`.

**Schedule table.** `const uint8_t* sched(uint32_t zoom)` returns `run[SPR_SCHED_LEN]` for the
effective (post-halving) zoom, built on first use by the reference accumulator exactly as `221-232`
(`xacc = 0; for i: r = 0; while (xacc < 0x200) { r++; xacc += zoom; } xacc -= 0x200; run[i] = r;`),
stored in `std::unique_ptr<uint8_t[]> sched_tab[0x1000]` (about 160 distinct zooms per mode, 2 KB
each; the 2.25 MB of dropped tables dwarfs it). Assert `r <= 4` for `zoom >= 0x80`.

**Vertical loop** (`draw_sprite_fast`): `d_lo = max(0, -top)`, `d_hi = min(H, s16_height - top)`;
`cx0 = x1 - xpos`, `cx1 = pend`. `d = d_lo`; while `d < d_hi`: `j = (d*zoom) >> 9`; `da = d`; advance
`d` while `d < d_hi && ((d*zoom) >> 9) == j`; `db = d`; `decode_row(j)`; `replay(j, da, db)`. This is
the closed form C3 of `SCHEDULE_VERIFICATION.md`, division-free. Before decoding row `j`, prefetch
source row `j + SPR_ROM_PF_ROWS`: `cb_prefetch_r` on the words at `k = 0` and `k = pitch - 1` of that
row (bounds-checked); at sprite start prefetch rows `j0` and `j0 + 1` likewise.

**Decode** (`decode_row`), source row `j`, `rowbase = (bank << 16) + addr + pitch*j` (int64):

```cpp
int pos = 0, words = 0, nrec = 0; uint8_t prev = GAP; const uint8_t* run = sched(zoom);
for (;;) {
    if (pos >= pend || words == SPR_WMAX) break;
    int64_t widx = flip ? rowbase + pitch - 1 - words : rowbase + words;
    if (widx < 0 || widx >= SPRITES_LENGTH) break;
    uint32_t w = sprites[widx]; if (flip) w = nibrev(w);              // rev + nibble swap, no side table
    for (int k = 0; k < 8; k++) {
        unsigned n = (w >> (28 - 4*k)) & 15;
        uint8_t t = (n == 0 || n == 15) ? GAP : (shadow && n == 10) ? SHADOW : COLOUR;   // SWAR class arrives in S4b
        if (t != prev) { emit(t, pos); prev = t; }
        uint16_t v = n | color; uint64_t v4 = v * 0x0001000100010001ULL; memcpy(line + pos, &v4, 8);  // always 4 copies
        pos += run[words*8 + k];
    }
    words++;
    if ((w & 0xf0) == 0xf0) break;
}
emit(GAP, pos);   // sentinel; emit's rules guarantee the last record is a GAP
```

`emit(t, p)`: if `nrec > 0 && rec[nrec-1].pos == p` then `nrec--` (a zero-length record from a run-0
pixel is dropped); then if `nrec > 0 && rec[nrec-1].type == t` return (merge); else
`rec[nrec++] = {p, t, 0}`. Records are therefore maximal runs of one type, which S2 relies on.
`struct SpanRec { uint16_t pos; uint8_t type; uint8_t flags; }`. The 8-byte store of four copies is
always safe: overrun lands on positions the next source pixels overwrite, or beyond the sentinel
where nothing is read; the per-word `pos >= pend` test bounds `pos <= pend + 34 < SPR_LB_LEN`.
Shadow classification is per nibble during decode; `sprites_shadowinfo` and `sprites_flipped` are
gone from the game build.

**Replay** (`replay(j, da, db)`), one function, `replay_mode` member (0 = D, 1 = D'; set by
`set_replay_mode`, game flag `-spritereplay N`, default fixed at S1.5):

```cpp
for (k = 0; k + 1 < nrec; k++) {
    if (rec[k].type == GAP) continue;
    int lo = max(rec[k].pos, cx0), hi = min(rec[k+1].pos, cx1); if (hi <= lo) continue;
    if (rec[k].type == COLOUR) {
        D : for (d = da; d < db; d++) memcpy(row(d) + lo, line + lo, (hi - lo) * 2);
        D': for (p = lo; p + 4 <= hi; p += 4) { uint64 v = load8(line + p); for (d) store8(row(d) + p, v); }  then halfword tail per row
    } else {  // SHADOW, inline read-modify-write, never deferred
        for (d = da; d < db; d++) for (p = lo; p < hi; p++) row(d)[p] |= 0x1000;
    }
}
```

`row(d) = pixels + (top + d) * s16_width + xpos`. Colour replay never reads the destination; shadow
replay never writes outside `[lo, hi)`. Both modes produce identical output (distinct addresses,
each `(row, x)` written once per sprite), so the switch is measured, not verified separately.

### S1.3 Must not change

- Every line of the record parse (S0.4) including `xpos < 0x80` wrap, the `0xbe`, `s16_x_off`
  before the hi-res doubling, `zoom >>= 1` truncation, `width << hires`, `offset` after
  normalisation, `bank % 4`, `pitch` as an unsigned 8-bit result.
- Terminator on the (mirrored) word after the whole word; screen-edge test per word before it;
  per-pixel clip to `[x1, x2)` via the span clip; rows outside `[0, s16_height)` skipped.
- Shadow OR is inline and `|= 0x1000`; colour store is `n | color`; nibbles 0 and 15 draw nothing
  regardless of the shadow flag; nibble 10 is colour when the shadow flag is clear.
- Both zoom regimes: runs 1-4 and runs 0-2 through the same decode; run 0 produces no output and no
  record (`emit`'s drop rule).
- Widescreen: `x2 = 808` or `404`, `s16_x_off = 42`; `x1 > 0` windows must still work (driver only:
  every game caller passes `set_x_clip(false)`, `outrun.cpp:687, 744, 834`, `omap.cpp:76`,
  `main.cpp:652`).
- `render_px` and `draw_sprite_fast` must agree on the `SPR_WMAX` and index-bounds rules (S0.4).
- Nothing crosses sprites: `line`, `rec`, `nrec` are per row; the only per-frame state is S3's.

### S1.4 Test

`unit s1a` (schedule): for every zoom `1..0xfff` and `i < SPR_SCHED_LEN`, `run[i] == ceil(512*(i+1)/zoom) - ceil(512*i/zoom)`
and the vertical loop's `j` sequence for `d < 1024` equals the accumulator of `976-979`.
`unit s1b` (nibrev): all 2^32 words, `nibrev(w)` nibble `k` equals `w` nibble `7-k` (about a minute).
`unit s1c` (records): 100,000 random type sequences with runs 0-4, records from `decode_row` equal
the maximal runs of the expanded sequence. `fuzz 500000` and `replay` against `render_px(false)` with
`glow` forced off; both `replay_mode` values; both driver targets, WSL and Pi. Negative control: force
`clip`-bit-driven skipping of the clip test and confirm `fuzz` fails (proves the harness sees
overruns into the pad).

### S1.5 Measure and decide

`prof_ns_sprites` per band. Kill rule (`DESIGN_FABLE.md` S1): less than 1.5x over the shipped pass
at typical load means the model is wrong; stop and re-measure. Expected 1.6-1.9x in-game (bench
2.0x minus the common memory term). D/D' decision: two 300-s runs per mode, compare the extreme
band ms/frame and 0xE8; ship the lower extreme-band figure as the default `replay_mode`, keep the
other behind the flag. Colour-span `cb_prefetch_w` of the next destination row (in-situ bandwidth review,
section 7 item 6) is a third switch measured the same way; keep it only if 0xE8 and ms both fall.

### S1.6 Dependencies

S0. Not shippable to `master` on its own (glow is off); S2 follows immediately.

---

## S2 - Glow (PIXEL_ACCURACY), deferred per sprite

### S2.1 Files and functions

`hwsprites.cpp`: `replay`, `draw_sprite_fast` (flush point), new `glow_push`/`glow_flush`;
`hwsprites.hpp`: `uint16_t* glow_ring[SPR_GLOW_RING]`, `int glow_n`. `SPR_GLOW_RING = 64` (tunable).

### S2.2 Behaviour

In `replay`, for a COLOUR record `k` after the `hi <= lo` skip:

```cpp
bool clear_left = !(rtl && k > 0 && rec[k-1].type == SHADOW);       // DESIGN_GLOW section 2 amendment
if (glow && clear_left && lo > cx0)                                   // x(lo) > x1, in index space
    for (d = da; d < db; d++) glow_push(row(d) + lo - 1);
```

`glow_push(p)`: if `glow_n == SPR_GLOW_RING` then `glow_flush()`; `glow_ring[glow_n++] = p`.
`glow_flush()`: `for i < glow_n: *glow_ring[i] &= 0xfff; glow_n = 0`. `draw_sprite_fast` calls
`glow_flush()` after its last row (per sprite, never per frame: `DESIGN_GLOW.md` section 3's two-sprite
counterexample). `draw_sprite_px` implements glow inline (S0.4) and needs none of this.

Why this is the reference result: one clear per maximal visible colour run, at the left neighbour
of its first drawn pixel, is the only clear that can change anything (`DESIGN_GLOW.md` section 1);
`emit` makes records maximal; `rec[k-1]` is non-empty by construction, so it is the screen pixel at
`lo-1` whenever `lo == rec[k].pos`, and when `lo == cx0` the `lo > cx0` test suppresses the clear as
the reference's `x > x1` does. Ordering: records are walked left to right, so for a left-to-right
sprite the shadow OR at `lo-1` (previous record, same row) is issued before the push and the flush
runs after it, giving `pre & 0xfff`; for a right-to-left sprite the hardware clears first and ORs
second, giving `pre | 0x1000`, which the suppression reproduces. Within a sprite no address is
written twice, so any flush point up to sprite end is safe.

### S2.3 Must not change

- Colour replay remains store-only; the clear is the only destination read the colour path adds,
  and it happens at the flush, on a line the same sprite stored to microseconds earlier.
- `lo - 1 >= x1` always holds when pushed (`lo > cx0`), so the ring never points left of the window
  or into the front pad on row 0.
- The deferred clear must never be applied after a later sprite's shadow OR to the same address:
  flushing at sprite end guarantees it. Do not batch across sprites.
- With `glow == false` S2 must be a no-op (the S1 gate re-run).
- The replay overrun assertion (S6 canaries) admits `x(lo) - 1`, one pixel left of the sprite's
  extent, for the ring only.

### S2.4 Test

`unit s2a`: S0's Test 1 port against `draw_sprite_fast(glow = true)`. `unit s2b`: the source-space
test of `DESIGN_GLOW.md` 5.2: random nibble rows containing 0, 15, 10 under both shadow modes, every
zoom in the three tables plus `0x200` and `0x401..0x7ff`, both `flip`, both `xdelta`, single-row
sprites, `draw_sprite_fast` vs `draw_sprite_px`, both glow values. `unit s2c`: Test 2 (2-5 sprite
lists, shadow-first and colour-first orders; the end-of-frame control must fail). `fuzz 500000` at
glow on; `replay` glow-on against `render_px(true)`. Coverage counters (driver): clears pushed,
clears suppressed by the `rtl` rule, clears that changed a value, flushes at ring-full; all non-zero
over `fuzz`, and the "changed a value" count over real captures is reported (`DESIGN_GLOW.md` 5.6).

### S2.5 Measure

`prof_ns_sprites` with `-glow` vs `-noglow`; expected 0.01-0.03 ms typical. Driver counter of ring
entries per frame (expected ~1,200 typical). If the delta exceeds 0.1 ms, add `cb_prefetch_r` at
push time or halve the ring.

### S2.6 Dependencies

S1. Gate for S3 (the bitmap filters S2's pushes).

---

## S3 - Per-row shadow bitmap and shadow-destination prefetch

### S3.1 Files and functions

`hwsprites.cpp`: `render` (frame setup), `replay` (SHADOW branch, glow push); `video.cpp:246`
(`render(pixels, 8, fg_enabled)`); `hwsprites.hpp`: `uint64_t shadow_rows[8]`. `SPR_SHADOW_PF_ROWS = 2`.

### S3.2 Behaviour

At the top of `render`: `shadow_rows[] = dest_clean ? all zero : all ones`. `dest_clean` is
`fg_enabled` from `video.cpp:199` (every row was repainted by road background or foreground this
frame, values below `0x1000`); the `_ref` twin keeps the two-argument signature. In `replay`'s
SHADOW branch, per destination row before the RMW loop: `shadow_rows[y >> 6] |= 1ull << (y & 63)`
(`y = top + d`). In S2's push: `if (shadow_rows[y >> 6] >> (y & 63) & 1) glow_push(...)`. Bit 12 is
only ever set by a shadow OR, so on a clean row with no shadow this frame the clear is a provable
no-op; on a non-clean frame every bit is set and nothing is filtered.

Prefetch: at the top of `replay(j, da, db)`, for every SHADOW record with non-empty clipped span
`[lo, hi)`, for `d` in `[db, min(db + SPR_SHADOW_PF_ROWS*(db - da), d_hi))`, `cb_prefetch_w` on every
64-byte line of `row(d) + lo .. row(d) + hi` (`PRFM PSTL1KEEP`; `__builtin_prefetch(p, 1, 3)`; no-op on
MSVC). This covers the next two source rows' shadow spans, which lie at nearly the same `x`, with
roughly two rows of decode time (in-situ bandwidth review, amendment 2).

### S3.3 Must not change

- The shadow OR itself: inline, `|=`, per pixel, within the clipped span only.
- The bitmap must be set by *every* shadow row write of *every* sprite before any push that could
  depend on it; setting it before the row's RMW loop guarantees this for the same row.
- `dest_clean` must be false whenever the caller cannot prove the buffer is clean (driver random
  backgrounds: always false; `render_px` path ignores it).
- Prefetch touches nothing; bounds-check rows against `d_hi`.

### S3.4 Test

`unit s3`: 10,000 random lists rendered with `dest_clean = true` onto a background with bit 12
clear everywhere (the only case the flag may be true) vs `render_px(true)`; the same lists with
`dest_clean = false` onto random bit-12 backgrounds. Negative control: pass `dest_clean = true` with
a bit-12 background and confirm the test fails. `fuzz`, `replay` (captured `fg_enabled` drives the
flag), S0.8 in all modes including a map-screen segment (`omap.cpp:47` sets `HORIZON_OFF`).

### S3.5 Measure

Driver counters: pushes filtered by the bitmap (expected most in sky rows). In-game
`prof_ns_sprites` and 0xE8/0x03 with the prefetch on and off (`SPR_SHADOW_PF_ROWS = 0` build).

### S3.6 Dependencies

S2.

---

## S4 - NEON replay and shadow OR; decode word classes; optional NEON expansion

### S4.1 Files and functions

`hwsprites.cpp`: `replay` (S4a), `decode_row` (S4b, S4c); `hwarch.hpp` for any new primitive.
The scalar forms of S1 remain the `!CB_HAVE_NEON` path and what MSVC ships.

### S4.2 Behaviour

**S4a, replay.** COLOUR span of `n = hi - lo` halfwords: if `n >= 8`, `vld1q_u16`/`vst1q_u16` in
16-byte chunks from `lo`, final chunk re-based to `hi - 8` (overlaps the previous chunk, never passes
`hi`); if `n < 8`, the S1 `memcpy`. D' loads each chunk once and stores it to all rows `[da, db)`.
SHADOW span: `vorrq_u16(vld1q_u16(p), vdupq_n_u16(0x1000))` on whole 16-byte chunks inside
`[lo, hi)`, scalar head and tail so no halfword outside the span is read or written (the tile kernel
precedent of writing back unchanged values is not used here: S2's ring and canaries assume the span
is the exact write set). All accesses unaligned (`row(d) + lo` is 2-byte aligned).

**S4b, decode word classes** (scalar, all targets). Before the nibble loop of S1.2 classify `w`:
`w == 0` (GAP word): `if (prev != GAP) emit(GAP, pos), prev = GAP; pos += wsum[words]` where
`wsum[m] = sum(run[8m..8m+7])` is a second per-zoom table built with `run`; no stores.
`w == 0xAAAAAAAA && shadow` (all-shadow word): same with SHADOW. Otherwise the SWAR test of
`DESIGN_FABLE.md` 2.4 (`z = (w - C1) & ~w`, `f = (~w - C1) & w`, `a = ((w ^ 0xAAAAAAAA) - C1) & ~(w ^ 0xAAAAAAAA)`
under `smask = shadow ? ~0u : 0`, `mixed = ((z | f | a) & 0x88888888) != 0`): if not mixed it is a
COLOUR word: `if (prev != COLOUR) emit(COLOUR, pos), prev = COLOUR;` then the eight
`{value, 8-byte store, pos += run}` steps with no type work; else the S1 per-nibble loop (MIXED). The
terminator test follows in every class (a GAP word cannot terminate; an all-shadow word cannot; keep
the test anyway).

**S4c, NEON expansion (optional).** COLOUR words only: eight nibbles to a `uint16x8_t` of `n | color`
(`vdup`, `vshl`/`vand` per nibble position, `vzip1_u8`; nibble order `k = 0` is bits 28-31, the
leftmost pixel), then scatter into `line` by the run table with `vst1q_lane_u16`/`vst1_u16` patterns
per run-length class, or the `tbl` index-vector form of `DESIGN_FABLE.md` section 4 with the index
vector generated per sprite from `run[]`. Kill rule: less than 10% on the S4b build, or any
regression on `pi2-local`.

### S4.3 Must not change

- Output of S4a/S4b is the S1 output; S4b changes only the *work* per word, never the record
  sequence (`emit` rules identical; a GAP word after a GAP record emits nothing).
- The SWAR any-zero-nibble form has no false positives, so the COLOUR path is exact; the shadow term
  is masked by `smask`, so nibble 10 is colour when the flag is clear.
- The re-based final chunk requires `n >= 8`; `n < 8` must not use it.
- Scalar and NEON builds must be bit-identical; `CB_FORCE_SCALAR` compiles the S1 code.

### S4.4 Test

`unit s4a`: exhaustive SWAR classifier over all 2^32 words, both `smask` values, vs the per-nibble
definition (about 40 s). `unit s4b`: for every `(lo, hi)` with `0 <= lo < hi <= 96` on a 4-row scratch
with guard rows and columns, random line content, COLOUR replay and SHADOW replay vs the S1 scalar
forms (kept as `_s1` twins under `CB_RENDER_TEST`), `memcmp` all 4 rows; both replay modes with
`da..db` of 1-4 rows. `unit s4c` (if S4c built): every ROM word of `sprites[]` (1M) plus 100,000 random
words, every zoom in the three tables, expansion into `line` vs the S1 decode's `line` and records.
`fuzz 1000000`, `replay`, S0.8; on the Pi both targets, and on x86-64.

### S4.5 Measure

`prof_ns_sprites` per band, 0xE8, instructions per pixel from `bench`. Kill rules: S4a < 5% (then the
routine is at the memory floor and S4b/S4c are not worth doing, `DESIGN_FABLE.md` section 7); S4b < 10%.

### S4.6 Dependencies

S3 (the shadow branch it vectorises carries S3's bitmap set). S4c depends on S4b.

---

## S5 - Shrink regime (lo-res, `hiresprites`): output-iteration decode

### S5.1 Files and functions

`hwsprites.cpp`: `decode_row` variant `decode_row_shrink`, selected per sprite when `zoom >= 0x200`.

### S5.2 Behaviour

For `zoom >= 0x200` each source pixel yields 0 or 1 output pixel, so iterate outputs instead: for
`p` from 0 while `p < pend`: `i = (p*zoom) >> 9`; word `k = i >> 3` (load a new word, mirrored if
`flip`, when `k` changes; apply the bounds and `SPR_WMAX` rules; test the terminator when leaving
word `k`, i.e. as soon as `i` moves past nibble 7 of a word whose `(w & 0xf0) == 0xf0` - the
reference tests it after the whole word is *drawn*, so outputs of that word are still produced);
nibble `n` from `i & 7`; type and `line[p] = n | color`; `emit` on type change with the same rules.
Source pixels with run 0 are simply never visited, which is exactly the reference's behaviour
(`SCHEDULE_VERIFICATION.md` C2). Zoom `0x200` (all runs 1) may take either path.

### S5.3 Must not change

- The terminator semantics above: a row that terminates still emits every output of the terminating
  word.
- `src(p) = (p*zoom) >> 9` in 32-bit arithmetic; `p <= 4096`, `zoom <= 0xfff`.
- Hi-res shipping zooms (`< 0x200`) are untouched by S5.

### S5.4 Test

`unit s5`: single-row sprites, every zoom `0x200..0xfff`, random rows over the dense synthetic ROM,
both `flip`, `decode_row_shrink` vs `decode_row` (records and the `line` content over every colour
span must be identical). `fuzz` and `replay` in lo-res and `hiresprites` modes; S0.8 lo-res and
`hiresprites`.

### S5.5 Measure

`prof_ns_sprites` in lo-res and `hiresprites`+hi-res. Kill rule: less than 20% on lo-res frames, or
lo-res deprioritised by the project (then S1's generic decode stays; it is already correct for
run 0).

### S5.6 Dependencies

S1 (S4b's word classes apply to the magnify path only; S5 has its own loop).

---

## S6 - Polish

### S6.1 Items

1. Left-edge fast-forward: when `cx0 > 0`, skip words whose output range ends before `cx0`, scanning
   each skipped word only for the terminator (a terminator left of the window ends the row in the
   reference), then start decoding at `i0 = (cx0*zoom) >> 9` with `pos = ceil(512*i0/zoom)` from a
   prefix-sum table; `SPR_LB_LEN` may then shrink to 1024 with `pend` rebased. Keep the `rtl` rule
   intact: the record before the first decoded pixel is unknown, but its pixel is at most `cx0 - 1`
   and a clear at `lo - 1 >= cx0` never targets it.
2. Debug canaries under `CB_RENDER_CHECKS`: guard words after `line` and `rec`, per-row assertion that
   every replay store lies in `[x1, x2)` on a row in `[0, s16_height)`, and that every ring entry
   also lies in `[x1, x2)` (guaranteed by `lo - 1 >= cx0`, never one pixel further left).
3. Remove the `clip` bit's residual use (statistic only), the `width` field's use except in
   normalisation, and document `data[7]` (`rawh`) and `data[15]` as engine scratch the renderer
   does not read except `offset`.
4. Finalise flags: `-noglow`/`-glow`, `-spritereplay`, and drop `SPRITE_REF_PX` from the release
   binary (keep the option). Restore the `PIXEL_ACCURACY` CMake help text to the truth.
5. `sched_tab` static vs per-sprite generation: a tie (in-situ bandwidth review); revisit
   only if 0x17 shows schedule-table misses in `bench`.
6. Update `../README.md` (which absorbed `PROBLEM_STATEMENT.md` and `COMPARISON.md`) with F1 and the resolved
   `PIXEL_ACCURACY` state.

### S6.2 Test and measure

Every item re-runs `fuzz`, `replay`, S0.8; item 1 additionally `unit s1c` and a sweep of `xpos` from
`-(4*width)` to `x2` for 200 random sprites. No performance target; item 1 is measured on frames with
many left-clipped sprites (widescreen off, attract).

### S6.3 Dependencies

S4 (item 1 touches the decode after its word classes exist).

---

## Appendix A - Summary of new and changed symbols

| Symbol | Where | Section |
|---|---|---|
| `render_ref`, `ref_prepass_fix`, `reset_ref_tables`, `SpriteTestAccess` | `hwsprites.*` (CB_RENDER_TEST) | S0 |
| `render_px`, `draw_sprite_px`, `parse_record`, `SpriteRec`, `nibrev`, `SPR_WMAX`, `SPR_EX_LEN` | `hwsprites.*` | S0 |
| `sprite_ram_buffer`, `get_x1/x2`, `set_glow/get_glow`, `load_sprite_ram`, `glow` | `hwsprites.hpp` | S0 |
| `cb_bswap32`, `cb_prefetch_r`, `cb_prefetch_w` | `hwarch.hpp` | S0/S1/S3 |
| `CB_SPR_DUMP`, `-dumpspr`, `CB_SPRITE_REF_PX`, `CB_SPRITE_STATS`, `CB_SPRITE_PMU`, `prof_spr_px` | CMake, `main.cpp`, `video.*` | S0 |
| `draw_sprite_fast`, `decode_row`, `replay`, `emit`, `sched`, `sched_tab`, `line`, `rec`, `SpanRec`, `replay_mode`, `set_replay_mode`, `-spritereplay`, `SPR_*` constants | `hwsprites.*`, `main.cpp` | S1 |
| `glow_ring`, `glow_n`, `glow_push`, `glow_flush`, `SPR_GLOW_RING` | `hwsprites.*` | S2 |
| `shadow_rows`, `render(pixels, priority, dest_clean)`, `SPR_SHADOW_PF_ROWS` | `hwsprites.*`, `video.cpp` | S3 |
| `wsum` table, SWAR classifier, NEON span copy/OR, `_s1` twins | `hwsprites.cpp` | S4 |
| `decode_row_shrink` | `hwsprites.cpp` | S5 |

## Appendix B - Edge-case checklist (tick every box per section before merging)

- [ ] hi-res 640x448, hi-res wide 808x448, lo-res 320x224, lo-res wide 404x224; `hiresprites` on with hi-res on and (hotkey path) with hi-res off (raw zooms to `0x7E0`)
- [ ] zoom regimes: runs 1-4 (`0x80..0x1FF`), all-1 (`0x200`), runs 0-1 (`0x201..0xfff`), `zoom == 0` skipped, `zoom < 0x80` via `render_px`
- [ ] `(flip, xdelta)` all four, with and without the shadow flag; `rtl` shadow-before-colour boundary at glow on
- [ ] sprites straddling `x1`, `x2`, top and bottom; fully off-screen; `pend <= 0`; `x1 > 0` clip windows (driver)
- [ ] terminator in word 0; terminator past the screen edge; unterminated row (`SPR_WMAX`); word index below 0 or past `SPRITES_LENGTH`
- [ ] `pitch` 0 and 255; `addr` normalisation going negative; `bank` 4-7 wrapping to 0-3
- [ ] run-0 colour pixel between shadow pixels; zero-length records dropped; adjacent equal records merged
- [ ] glow: clear at `x1` suppressed, clear left of the sprite's own extent applied, ring-full flush mid-sprite, flush at sprite end, never per frame
- [ ] `dest_clean` false on map/music screens and in the driver; bitmap all-ones then
- [ ] F1/F2 classifier attributes every shipped-vs-oracle difference (both counts reported); whole-game checksum equal to the `SPRITE_REF_PX` build at both glow settings
- [ ] `CB_FORCE_SCALAR` driver target passes on the Pi; MSVC x86-64 game build compiles with no NEON, no `__builtin_*`
- [ ] `render_px` fallback count zero over the captured set; `prof_spr_px` bands reported before and after
