# Sprite Renderer Comparison: cannonball-se vs. upstream djyt/cannonball

Scope: `hwsprites::render()` and its supporting data path (sprite RAM decode,
zoom/clip, ROM pixel decode, pixel plot). Fork side is the working tree of
`cannonball-se` on branch `ntsc-rearch` (`src/main/hwvideo/hwsprites.cpp`,
1003 lines; `hwsprites.hpp`, 46 lines). Upstream side is `djyt/cannonball`
master, `hwsprites.cpp` (323 lines) / `hwsprites.hpp` (35 lines), fetched
into the scratchpad for this comparison. Line numbers below refer to those
two files unless stated otherwise.

## 1. Upstream algorithm walk-through

`hwsprites::render(priority)` (upstream lines 192-324) loops over the fixed
128-entry sprite table in steps of 8 words (`SPRITE_RAM_SIZE = 128*8`,
hpp:25). For each entry:

- Terminates the whole loop on the list-end bit (`ramBuff[data+0] & 0x8000`).
- Filters by `sprpri` (2-bit priority field, `1 << ((data+3]>>12)&3)`)
  against the caller's `priority` argument — `video.cpp` calls
  `sprite_layer->render(8)` once per frame, so only priority-3 sprites are
  actually drawn (this is identical, and identically odd-looking, in both
  trees).
- Decodes hide flag, height, bank, top scanline, ROM address, pitch,
  x-position, shadow flag, vertical zoom, y/x draw direction, flip, and
  horizontal zoom straight out of the 8-word record (lines 205-219).
- Clamps `vzoom`/`hzoom` to a floor of `0x40` (line 240-241) — an artifact
  of unconfirmed hardware behaviour, not present in the fork (see §4).
- Adjusts `xpos` for widescreen (`+= config.s16_x_off`) and, if
  `config.video.hires`, doubles `xpos`/`top`/`ytarget` and halves both zoom
  factors (lines 246-257).
- Inner y-loop (`for (y = top; y != ytarget; y += ydelta)`), y-clipped with
  `if (y >= 0 && y < config.s16_height)` (line 262). Within it, `flip`
  selects one of two nearly-identical inner loops (non-flipped: lines
  268-291; flipped: 293-316), each an x-loop that reads one 32-bit ROM word
  per iteration, unpacks 8 nibbles MSB→LSB (or LSB→MSB when flipped), and
  for each nibble runs the `draw_pixel()` macro (defined at line 174,
  non-PIXEL_ACCURACY variant) inside a `while (xacc < 0x200) { ...;
  x += xdelta; xacc += hzoom; }` fractional-zoom accumulator — i.e. each
  nibble can be stretched into 0, 1 or more output pixels depending on
  `hzoom`.
- `draw_pixel()` (line 174-188): test `x` against the clip window, reject
  `pix == 0` (transparent) and `pix == 15` (special/unused), then either OR
  in the shadow flag (`pix == 0xa` and `shadow`) or write
  `pPixel[x] = pix | color`. The commented-out `#if PIXEL_ACCURACY` variant
  (line 145, default off, upstream sets `PIXEL_ACCURACY 0` at line 52) adds
  the "glowy edge" behaviour: when an opaque, non-shadow pixel is drawn, the
  pixel immediately to its left has its shadow bit cleared
  (`pPixel[x-1] &= 0xfff`), reproducing a documented one-clock-cycle
  hardware quirk (credited to "Alex B." in the header comment).
- Row end: `yacc += vzoom; addr += pitch * (yacc >> 9); yacc &= 0x1ff;` —
  the classic fractional row-accumulator that skips ROM rows when zoomed
  down and repeats rows when zoomed up.

Rough per-pixel cost in the hot path: 1 nibble extract, a 2-4 term boolean
clip/transparency test, a branch, and (usually) either nothing or one
16-bit store — call it 4-8 scalar instructions per candidate pixel, run
inside a `while` whose trip count is data-dependent (0-2 for typical zoom
ranges). There is exactly one loop body per flip state; shadow, clip and
row-count are all handled by branches inside that one body, so instruction
cache footprint is small and uniform.

## 2. Current fork algorithm walk-through

The fork's `render()` (`src/main/hwvideo/hwsprites.cpp:414-1003`) keeps the
same outer per-sprite decode (lines 421-462) but the record is now 16 words
(`SPRITE_RAM_SIZE = 128*16`, hpp:29) — 8 extra scratch words per sprite,
of which word 15 (`offset`, line 456) carries a precomputed hi-res x-nudge
and word 0 bit `0x2000` (line 437, `bool clip = (ramBuff[data+0] & 0x2000)`)
carries the **clip-required** flag described in §5. `vzoom`/`hzoom` are
merged into one `zoom` field (`ramBuff[data+3] & 0x0fff`, line 442) — the
fork's sprite hardware model treats horizontal and vertical zoom as always
equal, so the separate `hzoom` clamp from upstream is simply gone (no
floor-clamp at all, line ~442 vs upstream 240-241).

**Direction normalisation.** Lines 509-519: if `xdelta == -1` the code
flips it to `+1` up front, shifts `xpos` left by the rendered width, and
toggles the `flip` flag / ROM start address accordingly. This means the hot
loop never again branches on `xdelta` — every sprite is drawn left-to-right
— at the cost of this one-time transform per sprite per frame.

**Lazy flipped-ROM cache.** Lines 521-598 (new versus upstream, added in
commit `c51dc0e`). Because flip is now resolved outside the pixel loop, the
fork needs pre-flipped nibble data rather than reading backwards. On first
encounter of a given (bank, row) it nibble-swaps the row from `sprites[]`
into `sprites_flipped[]` (hpp:34) and records, in `sprites_shadowinfo[]`
(hpp:40), whether that 8-pixel-wide ROM word contains a `0xA` (shadow)
nibble, using a bit trick (`pxt = pixels ^ 0xAAAAAAAA;
((pxt - 0x11111111) & ~pxt & 0x88888888) != 0`, lines 551-553/588-589) that
tests all 8 nibbles for the value 0xA in ~4 ALU ops with no branch. Both
caches are sentinel-initialised (`0xffffffff` / `0xff`, `reset()`,
hwsprites.cpp:124-127) and populated once per ROM row for the life of the
loaded ROM — this is a runtime memoization table, not a build-time asset
(see §5).

**Per-scanline row count / jump-table dispatch.** Lines 618-660 are the
core structural change. For each drawn scanline the code computes, from the
fractional zoom accumulator, how many consecutive output scanlines
(`count`, 1-3; the `while` loop at 634-637 is capped at
`std::min(3, ...)` so `count` can never reach 4) share the same source ROM
row, then builds a 4-bit `jump_key` (lines 647-656):

```
jump_key = (count-1)              // bits 0-1: 0..2 rows-1 (case 3/7/11/15 unreachable, capped at 3)
         | (clip ? 0 : 4)         // bit 2: no-clip fast path
         | (shadowfound ? 8 : 0); // bit 3: this row has a shadow nibble, from sprites_shadowinfo
```

`switch (jump_key)` (line 660) then dispatches to one of 16 fully unrolled
case bodies (12 of them reachable — cases 3/7/11/15 are stub `break;`
statements the comment calls "<1%" but which the `count` cap makes
unreachable rather than merely rare). Flip is *not* part of the key any
more — it was resolved earlier by pointing `spritedata` at either
`spriterom` or `spriterom_flipped` (lines 605-611).

**Multi-row write.** Each reachable case body (e.g. case 1, lines 683-709)
declares 1-3 extra row pointers (`pPix2`, `pPix3`) alongside `pPix1` and the
per-nibble draw macro (`draw_pixel_2row()`, etc.) writes the *same* decoded
pixel to all of them in one pass — i.e. when the sprite is zoomed up enough
that 2-3 destination scanlines come from one source ROM row, the ROM word is
fetched and decoded once and stored 2-3 times, instead of upstream's
approach of re-entering the whole y-loop (and re-fetching/re-decoding the
ROM row) once per output scanline.

**Clip fast path.** The 12 active `draw_pixel_*` macros (lines 219-408) are
the cross of {1,2,3 rows} × {clip-checked / `_nc` no-clip} × {shadow-aware /
`_ns` no-shadow}. The clip test itself is a single unsigned-compare trick
(`IF_IS_DRAWABLE()`, line 214): `((unsigned)(pix-1) < 14u) &&
((unsigned)(x-x1) < span)` — this fuses "not transparent (0) and not 15"
into one range check on `pix-1` (valid range 1..14), exactly as upstream's
two separate comparisons but branch-free-friendly. The `_nc` variants drop
the second term entirely (`IF_IS_DRAWABLE_NO_CLIP()`, line 216) — safe only
because `clip` (from the precomputed sprite-table bit, §5) has already
established the sprite is fully on-screen, so the outer x-loop for those
cases changes from a bounded `for (... x < scrn_width ...)` to an unbounded
`do { ... } while ((pixels & 0xf0) != 0xf0);` (e.g. lines 746-759).

**Hi-res path.** Gated by the independent `config.video.hiresprites` menu
option (default 0, "beta" per the commit message). When on,
`OSprites::do_sprite()` (`src/main/engine/osprites.cpp:590-780`, commit
`c51dc0e`) selects sprite bitmap size classes from a second table,
`ZOOM_LOOKUP_HIRES[]` (`src/main/engine/data/ozoom_lookup.hpp:29+`, 286
new lines), that deliberately picks a *larger* underlying ROM sprite frame
than the requested zoom needs and records the true zoom delta in a new
`offset` field (`osprite::set_offset`, `osprite.cpp:162`, storing into RAM
word 15) so that `hwsprites::render()` can add a fine x-nudge
(`xpos += offset`, hwsprites.cpp:601-602) after doubling every screen-space
quantity for the 640×448 hi-res canvas. This buys sharper-looking sprites
in hi-res mode at the cost of a second, hand-tuned lookup table and extra
per-sprite arithmetic in `do_sprite()`.

**PIXEL_ACCURACY.** Declared at hwsprites.cpp:58 as an unconditional
`#define PIXEL_ACCURACY 0`, gating an `#if`/`#else` (lines 182-410) between
upstream's original glow-reproducing `draw_pixel()` macro (kept verbatim,
lines 192-207, unused unless `PIXEL_ACCURACY` is 1) and the fork's fast
12-macro family described above. See §4 for why this compile option is
effectively dead.

## 3. Commit-by-commit history

Commits below are ordered by actual commit date (the task's suggested order
placed `2964a8e` before `d15e76b`; git history shows the reverse — `d15e76b`
is five days older). `d852939` and `aa77386` are included for context only.

| SHA | Date | Intent (one line) | Size (hwsprites.cpp) | Performance claim (verbatim) | Survives in current file? |
|---|---|---|---|---|---|
| `d852939` | 2025-02-26 | Earliest sprite micro-opt: single unsigned range check for clip test, branch-hint dummy pixel | +18/-3 | *(none stated; commit covers whole audio/video rework: "Revised and optimised for Raspberry Pi Zero 2W")* | Yes — the unsigned range-check trick is the ancestor of `IF_IS_DRAWABLE()` |
| `aa77386` | 2025-08-27 | CannonBall-SE rebrand / audio+render overhaul | 0 (doesn't touch hwsprites.cpp) | *(none re: sprites)* | N/A |
| `d15e76b` | 2025-09-27 | Merge flip/non-flip loops into one, add prefetch + `std::byteswap` nibble-swap-on-flip, memcpy-based ROM load, remove Boost | 137 lines | *(none quantified in message; title: "various bug fixes")* | No — reverted by `2964a8e` five days later |
| `2964a8e` | 2025-10-02 | Undo the merged/prefetch loop; split back into explicit shadow/no-shadow, flip/non-flip inline-function paths keyed on `any_shadow_nibble()` | 478 lines changed | Title: "Revised hwsprites to improve performance on Pi-1" (no numeric claim in body) | No — superseded by `6c7eef2`'s revert |
| `6c7eef2` | 2025-10-12 | Revert to a MAME-shaped single loop; make S16 glow unconditional via safe `pDst[x-1]` write (buffer padding trick); drop the runtime `s16accuracy` toggle | 340 lines changed | Title: "Reverted to MAME hwsprites and re-optimised; S16 glowy edges now always on" | Partially — the `pDst[x-1]`-is-safe reasoning is retained in comments; the "always on" glow itself was later gated off (`0cb3024`) |
| `817ddd3` | 2025-10-13 | Bug fix: restore the y-clip test that `6c7eef2` had commented out | 3 lines | Title only: "Restored Y-clip check erroneously left disabled in commit 6c7eef2" | Yes, y-clip is present (hwsprites.cpp:623) |
| `0cb3024` | 2025-11-02 | Reintroduce `PIXEL_ACCURACY` as a compile-time (CMake) choice; split loops by shadow/xdelta/flip into 8 explicit blocks using `IF_IS_DRAWABLE()`; add `pixels` param to `render()` | 397 lines added | *"Approx 20% faster sprite emulation (measured on Pi2 v1.1); PIXEL_ACCURACY compile-time choice restored and this can now be set via CMake e.g. 'cmake .. -DPIXEL_ACCURACY=0' to disable S16 glowy edges."* | Partially — `IF_IS_DRAWABLE()` and the CMake option survive; the 8-way (shadow×flip×xdelta) split was superseded by the 16-way jump table in `938a598` |
| `938a598` | 2025-11-09 | Add clip-required bit to the sprite RAM table (computed in `osprites.cpp`); extend the dispatch to a 16-way `switch`/jump table keyed on direction, flip, shadow and clip | 416 lines added | *"Added clip(required) bit to sprites data table; revised hwsprites to use jump table to speed up sprites that don't need x-clip; hwsprites now ~30% faster than MAME implementation on Pi2"* | Partially — the clip bit and jump-table *concept* survive; this exact 16-way key (direction/flip/shadow/clip) was replaced by the count/clip/shadow key in `8a01f4a`/`c51dc0e` |
| `8a01f4a` | 2025-11-11 | Multi-row write path: compute how many output scanlines share a source ROM row and write 1-4 rows per decoded pixel; `PIXEL_ACCURACY` default flipped to 0 in-source | 517 insertions / 715 deletions (net -198) | Title: "Revised hwsprites.cpp to write up to 4 sprite rows together, significantly improving performance on Pi2" (no % given) | Partially — multi-row writing survives, capped at 3 rows instead of 4 in the current file |
| `c51dc0e` | 2025-11-25 | Hi-res sprite mode (beta): lazy flipped-ROM + shadow-info caches, direction normalisation, hi-res zoom-table/offset field, final 16-case switch keyed on `(count-1)\|clip\|shadowfound` | 874 lines changed (whole-file rewrite) | Title only: "Hires sprite rendering mode (beta)" (no numeric claim) | **Yes — this is the current file** |

## 4. Behavioural / output differences

Pixel output is **not** intended to be bit-identical to upstream in every
mode, and one path is provably dead:

- **`PIXEL_ACCURACY` is silently forced off, regardless of the CMake
  option.** `CMakeLists.txt:68` defaults `option(PIXEL_ACCURACY ... ON)` and
  (`CMakeLists.txt:279-283`) passes `-DPIXEL_ACCURACY=1` on the compiler
  command line by default. `hwsprites.cpp:58` then does an **unguarded**
  `#define PIXEL_ACCURACY 0` (no `#ifndef` guard, unlike the guarded
  version `0cb3024` originally introduced). A macro redefinition in the
  translation unit always wins over a command-line `-D`, so the build
  silently compiles the non-glow path no matter what CMake was told —
  the CMake toggle is dead. This also means the fork's much-repeated goal
  of reproducing S16 "glowy edges" (`6c7eef2`: *"S16 glowy edges now always
  on"*) is **not actually shipping** in the current tree: none of the 12
  active `draw_pixel_*` macros (lines 219-408) implement the
  `pPixel[x-1] &= 0xfff` glow write at all — only the dead `#if
  PIXEL_ACCURACY` branch (lines 192-207) does. Net effect: current fork
  output for shadowed sprite edges is plain (no glow), which incidentally
  matches upstream's own default (`PIXEL_ACCURACY 0` is also upstream's
  hardcoded default, upstream line 52) — but for the wrong reason, and
  contrary to the fork's own commit messages and CMake help text.
- **Shadow-bit write changed from mask-and-add to OR.** Upstream:
  `pPixel[x] &= 0xfff; pPixel[x] += S16_PALETTE_ENTRIES;`. Fork's fast
  path: `*pPix1 |= S16_PALETTE_ENTRIES;` (e.g. line 224).
  `S16_PALETTE_ENTRIES == 0x1000` (`globals.hpp:43`) is a single bit, so
  this is equivalent *provided* no higher bit is ever already set on that
  pixel — plausible given the documented pixel-value layout, but the fork
  no longer defensively clears/normalises the word first, so this is worth
  flagging as an assumption rather than a proven invariant.
- **`hzoom`/`vzoom` floor-clamp (`< 0x40 → 0x40`) removed.** Upstream keeps
  it (upstream lines 240-241) with a comment "not 100% confirmed"; the
  fork's single merged `zoom` field is used unclamped. At extreme
  distances this could allow smaller/zero-length inner loops than upstream
  would produce — a plausible but unverified behavioural divergence.
- **Sprite RAM record widened 8→16 words** (`hwsprites.hpp:29`,
  `osprites.cpp` `blit_sprites()` loop now writes 16 words per sprite).
  This is purely an internal representation change (scratch fields for
  `clip`, `rawh`, `offset`) and shouldn't be visible in output, but it is a
  real deviation from the original hardware's 16-byte (8-word) sprite
  record shape that any future hardware-accuracy comparison needs to be
  aware of.
- **Widescreen and hi-res-doubling logic are unchanged** (`x1`/`x2`
  clipping in `set_x_clip()`, `xpos += config.s16_x_off`, `<<1` scaling
  under `config.video.hires`) — both trees compute these identically.
- **New hi-res *sprite* mode** (`config.video.hiresprites`, independent of
  `config.video.hires`) has no upstream equivalent at all; it changes which
  ROM sprite-size class is used and nudges `xpos` by a per-sprite `offset`.
  It is explicitly labelled "beta" in the commit message and defaults off.
- **Dead/leftover instrumentation.** `hwsprites.hpp:21-22` exposes public
  `setup`/`draw[16]` `std::chrono` members that `render()` (hwsprites.cpp
  416-419, 981) populates every frame but that no other file in the
  codebase reads (grep confirms zero external references) — the only
  consumer, a `std::cout` dump, is commented out at hwsprites.cpp:984-1002.
  This has no output effect but is dead weight in both the header's public
  API and the hot loop (an extra `high_resolution_clock::now()` pair and
  array increment per sprite, per frame).
- **Unreachable switch cases.** `count` is capped at 3
  (hwsprites.cpp:632-637), so the "count==4" `jump_key` slots (cases
  3/7/11/15, e.g. line 740) are unreachable, not merely rare as their
  inline comments ("<1%") suggest.

## 5. Provenance of the "clip required" sprite-table bit

The bit is **not** produced by any offline script or build step — it is
computed live, once per sprite per frame, inside the game-engine layer:

- `OSprites::do_sprite()` (`src/main/engine/osprites.cpp:790-800`, added in
  commit `938a598`) already computes the sprite's on-screen bounding box
  (`sprite_x1/x2`, `sprite_y1/y2`) against the widescreen bounds
  (`x1_bounds`, `x2_bounds`) to decide whether to cull the sprite entirely.
  Immediately after that test it derives
  `bool fully_inside = (sprite_x1 >= x2_bounds) && (sprite_x2 < x1_bounds);
  bool clip = !fully_inside;` and calls `output->set_clip(clip)`
  (lines 798-800).
- `osprite::set_clip(bool)` (`src/main/engine/osprite.cpp:149-155`) sets or
  clears bit `0x2000` of RAM word 0 (`data[0x0] |= 0x2000` /
  `&= ~0x2000`), reusing a previously-unused bit adjacent to the hardware's
  own `hide` bits, documented at the top of both `osprite.cpp` and
  `hwsprites.cpp` as `+0 --c----- -------- JJP - clip required
  (partially off-screen)`.
- `hwsprites::render()` simply reads that bit back out of the RAM buffer it
  already owns (`ramBuff[data+0] & 0x2000`, hwsprites.cpp:437) — no
  separate table, file, or generation script exists; it is one extra bit
  riding inside the existing 32-byte-per-sprite RAM record (which, as
  noted in §4, was widened from 16 to 32 bytes specifically to make room
  for this and the other new scratch fields). Size: 1 bit × up to 128
  live sprites per frame, recomputed every frame — negligible footprint,
  zero persistent storage.

## 6. Code-size and structure metrics

| Metric | Upstream | Fork (current) |
|---|---|---|
| `hwsprites.cpp` total lines | 323 | 1003 |
| `hwsprites.hpp` total lines | 35 | 46 |
| `render()` body length | ~133 lines (192-324) | ~590 lines (414-1003) |
| Sprite-record width | 8 words / 16 bytes | 16 words / 32 bytes |
| Distinct inner pixel-plot macro variants | 1 (`draw_pixel()`, 2 counting the unused `PIXEL_ACCURACY` form) | 13 (`draw_pixel()` PIXEL_ACCURACY form + 12 `draw_pixel_{1,2,3}row{,_nc}{,_ns}` combinations) |
| Distinct fully-unrolled dispatch paths | 2 (flip / non-flip, shadow test inline) | 16 `switch` cases, 12 reachable (§4) |
| Unrolled macro call sites in the hot switch | n/a (single loop) | 96 (`12 reachable cases × 8 nibbles`) |
| Static sprite ROM/cache tables | `sprites[]`: 1 MiB | `sprites[]` 1 MiB + `sprites_flipped[]` 1 MiB + `sprites_shadowinfo[]` 256 KiB ≈ 2.25 MiB |
| Compile-time options affecting this file | none | `PIXEL_ACCURACY` (CMake, currently inert — §4) |

**Hot-code footprint estimate.** Each of the 12 reachable switch cases is a
self-contained loop that: fetches one ROM word, extracts 8 nibbles, and for
each nibble expands one `draw_pixel_Nrow[...]` macro — a compare, a branch,
and 1-3 stores through distinct row pointers, plus the `x`/`xacc`
zoom-accumulator update. At a rough 25-45 bytes of ARM/x86 machine code per
macro expansion (branch + up-to-3 stores + pointer increments), each case
body is on the order of 8 × 30 ≈ 240 bytes for the single-row cases up
to roughly 8 × 45 ≈ 360 bytes for the three-row/shadow cases, i.e. the
12 case bodies together are plausibly in the 3-4 KiB range before
considering the surrounding per-sprite setup code (ROM-flip cache builder,
count computation, jump-key composition — hwsprites.cpp:414-660, itself a
few hundred more bytes) and the compiler's likely partial inlining/unrolling
of the `while (xacc < 0x200)` loops. That puts the whole hot region
plausibly in the 4-6 KiB class — comfortably inside a 32 KiB L1I on its
own, but stacked against the NTSC filter's own multi-kernel jump table
(`snes_ntsc.cpp`, per this repo's other active rearchitecture) and the tile
and road renderers that also run every frame, cache pressure from having
12 rarely-shared code paths (versus upstream's 2) is a real, if unquantified,
risk — exactly the kind of thing the repeated "count"/"clip"/"shadow"
splitting was meant to trade against fewer *dynamic* branches per pixel.
No profiler output for I$-miss rates is present in this repo to confirm
either way; the `draw[16]`/`freq[32]` instrumentation described in §4 was
seemingly built for exactly this kind of measurement but its results are
never surfaced (output code commented out).

## Summary of technique survival

Of the distinct optimisation techniques tried across the eight commits,
the ones present in the shipped file are: the unsigned range-check clip
test (from `d852939`, generalised in `0cb3024`), the precomputed clip bit
sourced from the game engine (`938a598`), the count/row-count switch
dispatch (evolved from `938a598`→`8a01f4a`→`c51dc0e`), and the multi-row
write path (`8a01f4a`, capped at 3 rows in `c51dc0e`). Abandoned techniques
include: `__builtin_prefetch` + `std::byteswap`-based flip (`d15e76b`,
reverted by `2964a8e`), inline `__attribute__((always_inline))` C++
functions in place of macros (`2964a8e`, reverted by `6c7eef2`), the
runtime `config.video.s16accuracy` toggle (`6c7eef2`, replaced by the
now-broken compile-time `PIXEL_ACCURACY`), and the `xdelta`/flip axes in
the jump-table key (`938a598`, removed once direction normalisation and
the flipped-ROM cache made them redundant in `c51dc0e`).
