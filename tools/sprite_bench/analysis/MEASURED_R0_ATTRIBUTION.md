# Measured R0 attribution: where S4's per-frame sprite cost goes, and what the deficit tracks

Date 2026-09-05. Machine `sl-d303` (Raspberry Pi Zero 2 W, 4x Cortex-A53,
aarch64), CPU governor pinned to `performance` and **verified** immediately
before the run set (`GOVERNOR_CHECK: performance`, all four cores at a fixed
1,000,000 kHz throughout runs). 512 MB RAM, so all builds `-j1` and run
sequentially, never concurrently. GCC 14.2.0 (Debian 14.2.0-19), cmake+make,
no `perf` tool on this box, hence the cntvct_el0-based phase timers below
rather than a sampling profiler.

Two throwaway `git archive` exports, ROMs copied in from `~/cbgame/roms`
(33 files, md5-cross-checked against the source):

| tree | commit | md5 of binary |
|---|---|---|
| shipped | `bdf7e9b` | `e4a259dfb38e9a5fbb5924adada0a7c2` |
| S4 (NEON span replay), instrumented | `73dc0c1` | `4616148aa0bba5f32335ed5a41a36de6` |

The shipped binary's md5 matches the one recorded for the same commit in
`MEASURED_S4_INSITU.md` (built on a different box, same CMake defaults),
which cross-checks the build is reproducible. The S4 md5 differs from that
document's `7bb7ade4aa149405c2af757b331308ee` because this build additionally
carries the `CB_R0_PROF` phase-timer instrumentation described below;
`strings` confirms `BENCHR0` is present in the S4 binary and absent from the
shipped one.

**Build note.** The first `-j1` build attempt was interrupted by an
unplanned reboot on `sl-d303` partway through the shipped tree's compile
(`uptime` dropped from 14h46m to ~1 min between checks; `vcgencmd
get_throttled` read `0x0` both before and after, so the firmware
undervoltage flag does not confirm a brownout, but a hard reset mid-compile
on a heatsink-less Zero 2 W under sustained `-j1` GCC load is consistent
with one). Both build directories were wiped and the build restarted from
scratch; the second attempt completed end-to-end with `RC=0` and no warnings
promoted to errors.

## Instrumentation

**Windowed harness (both trees, `src/main/main.cpp`).** A throwaway block
(`patch_main.py`) prints cumulative `video.prof_ns_sprites` in 300-frame
windows keyed by `video.prof_frames`, plus a `CB_FRAME_CAP` hard stop.
Deterministic attract mode means both trees walk the identical frame
sequence, so windows are scene-matched by construction.

**Phase timers (S4 tree only, `src/main/hwvideo/hwsprites.cpp`, guarded by
`CB_R0_PROF`).** A `cntvct_el0` read (`mrs %0, cntvct_el0`, a fixed-frequency
counter) brackets every entry/exit of the five functions that do all of a
sprite's work:

- `draw_sprite_fast` -> `g_r0_total`, `g_r0_sprites` (both its early-return
  and its normal exit are wrapped)
- `decode_row` -> `g_r0_decode`, `g_r0_srcrows` (single exit)
- `replay_colour_span` -> `g_r0_copy`, `g_r0_px` (its NEON early-return
  *and* its scalar-fallthrough exit are both wrapped)
- `replay_shadow_span` -> `g_r0_copy`, `g_r0_px` (single exit)

Since `decode_row`/`replay_*` run *inside* `draw_sprite_fast`'s timed
region, `OTHER = total - decode - copy` is everything else: the function-call
overhead, bounds/zoom arithmetic, the ROM/shadow prefetch scans, and
`glow_flush`. All three buckets share the same clock, so the fractions
`decode/total`, `copy/total`, `other/total` are valid even though cntvct
ticks are not 1:1 with CPU cycles. A `BENCHR0` line was added to the
windowed harness so these cumulative counters are logged every 300 frames
alongside `BENCHWIN`. `grep -c CB_R0_PROF hwsprites.cpp` = 11 (1 globals
block + 2 in `decode_row` + 3 in `draw_sprite_fast` + 3 in
`replay_colour_span` + 2 in `replay_shadow_span`), matching the five
functions' entry/exit counts exactly.

## R0a — static analysis (objdump, S4 binary)

`objdump -d` + `c++filt`, functions located and bounds-checked against the
full `hwsprites::` symbol table (an earlier naive line-range slice bled into
neighbouring functions — `hwsprites::replay()` sits physically between
`replay_colour_span` and `replay_shadow_span` — corrected before scanning).

**1. `bl`/`blr` calls inside the hot paths — YES.**

`decode_row` calls a helper twice at row-entry, *not* per nibble
(`sched()`/`wsum()` fold to the same code address under identical-code
folding):

```
5d440: bl 5d1d0 <hwsprites::sched(unsigned int)>
5d444: mov x22, x0
5d448: ldr w1, [x19, #32]
5d44c: mov x0, x20
5d450: bl 5d1d0 <hwsprites::sched(unsigned int)>
```

`draw_sprite_fast` calls `decode_row` and `replay` via real `bl` (the
compiler did not inline either); the source's single `while` loop is
unrolled 3x, so this is 3 real call pairs per sprite:

```
5e890: bl 5d3f0 <hwsprites::decode_row(hwsprites::SpriteRec const&, int, int)>
5e8b0: bl 5dde0 <hwsprites::replay(hwsprites::SpriteRec const&, unsigned short*, int, int, int, int)>
5e918: bl 5d3f0 <hwsprites::decode_row(...)>
5e938: bl 5dde0 <hwsprites::replay(...)>
5e9bc: bl 5d3f0 <hwsprites::decode_row(...)>
5e9dc: bl 5dde0 <hwsprites::replay(...)>
```

**2. `sdiv`/`udiv` — NONE.** Zero occurrences of either mnemonic in
`decode_row`, `replay_colour_span`, `replay_shadow_span`, or
`draw_sprite_fast`. The division-free design (closed-form `(d*zoom)>>9`
instead of an accumulator) is confirmed at the instruction level.

**3. `mul`/`madd`/`smull` inside the per-nibble decode loop — YES.** 9
occurrences of `mul x_, x_, x13` inside `decode_row`'s unrolled per-nibble
body (one COLOUR-word unroll of k=0..7 plus the MIXED-word path), each
immediately following a 16-bit mask and feeding a store — this is
`fill = (uint64_t)v * 0x0001000100010001ULL` computed fresh per nibble:

```
5d6ac: orr w7, w7, w1, lsr #28
5d6b0: and x7, x7, #0xffff
5d6b4: mul x7, x7, x13          ; fill = v * 0x0001000100010001
5d6b8: str x7, [x14, x11]       ; std::memcpy(line+pos, &fill, 8) -> inlined store
...
5d6d0: sbfiz x6, x7, #1, #32
5d6d4: mul x5, x5, x13
5d6d8: str x5, [x14, x6]
```

(`replay_colour_span`/`replay_shadow_span` also contain 1 and 2
mul-family instructions respectively, but these sit outside any per-pixel
loop — they compute the `base + d*scrn_width` row address once per
destination row, not once per pixel.)

**4. Loads from a record-relative base inside the unrolled body — YES.**
`x19` holds the address of `const SpriteRec& r` (set in the prologue,
`mov x19, x1`). `r.color` (offset `#52`) is reloaded from memory on
successive nibble iterations of the same unrolled word instead of being
kept live in a register, even though it is invariant for the whole row:

```
5d698: ldr w7, [x19, #52]   ; reload r.color, nibble k
5d6e4: ldr w5, [x19, #52]   ; reload r.color, nibble k+1
5d70c: ldr w4, [x19, #52]   ; reload r.color, nibble k+2
5d730: ldr w5, [x19, #52]   ; reload r.color, nibble k+3
```

**5. `bl memcpy` — YES, exactly one, in `replay_colour_span`'s scalar
D-path**, called once per destination row for a variable-length span (the
fixed 8-byte per-nibble memcpy in `decode_row` is a compile-time-constant
size and was inlined to a plain store instead — see item 3 above, no
`bl memcpy` there):

```
5dc08: mov x0, x3
5dc0c: mov x2, x23
5dc10: mov x1, x24
5dc14: add w22, w22, #0x1
5dc18: bl 22dd0 <memcpy@plt>
5dc1c: add x3, x0, x25
5dc20: cmp w19, w22
5dc24: b.ne 5dc08 <hwsprites::replay_colour_span(...)+0xf8>   ; loop over d in [da,db)
```

`replay_shadow_span` has **zero** `bl`/`blr` and zero `memcpy` — its
read-modify-write loop is fully inlined and call-free.

## Runs

Three 9000-frame `-60 -t 4` attract captures, `SDL_VIDEODRIVER=offscreen`,
`CB_FRAME_CAP=9000`, run **sequentially** (never concurrently), governor
pinned+verified `performance` immediately before the sequence started,
temperature sampled every 20 s:

| run | binary | args | frames reached | RC | throttled before/after | temp before -> after |
|---|---|---|---|---|---|---|
| ship | r0_ship | (none) | 9000 | 0 | 0x0 / 0x0 | 54.2C -> 68.2C |
| s4glow | r0_s4 | `-spritereplay 0` | 9000 | 0 | 0x0 / 0x0 | 67.7C -> 71.4C |
| s4noglow | r0_s4 | `-spritereplay 0 -noglow` | 9000 | 0 | 0x0 / 0x0 | 71.4C -> 73.1C |

`throttled=0x0` at every checkpoint; peak 73.1C, well under thermal
throttling. `GOVERNOR_CHECK: performance` logged immediately before run 1;
`scaling_governor`/`scaling_cur_freq` re-logged at the start of every run,
reading `performance` / `1000000` all three times, so clock was fixed at
1.0 GHz for the whole sequence (an earlier run attempt was discarded and
redone after discovering the governor had reverted to `ondemand`,
presumably following the build-phase reboot — the box has a real
600-1000 MHz range, not a single fixed point, so that would have confounded
the light/moderate/heavy banding below; the re-run's numbers land within a
few percent of the discarded ondemand-governor run, but the pinned-governor
numbers are the ones reported here).

Cumulative sprite time over the identical 9000-frame sequence
(`BENCHFINAL cum_spr_ns`):

| build | cum sprite-ns | vs shipped |
|---|---|---|
| shipped | 13,901,970,479 | 1.000 |
| S4 glow-ON (`-spritereplay 0`) | 19,375,577,155 | 1.394 |
| S4 glow-OFF (`-noglow`) | 18,765,547,594 | 1.350 |

30 usable 300-frame windows per run (window 0 discarded, no prior cumulative
to diff against), aligned by frame index across all three runs (identical
attract-mode content by construction) and banded into terciles by shipped
ns/frame.

## Results

**Per-band summary:**

| band | n windows | shipped ns/frame | S4-glow ns/frame | deficit ns/frame | ratio | glow cost ns/frame |
|---|---|---|---|---|---|---|
| light | 10 | 624,982 | 912,473 | 287,491 | 1.460 | 34,275 |
| moderate | 10 | 1,169,892 | 1,690,136 | 520,244 | 1.445 | 47,418 |
| heavy | 10 | 2,839,117 | 3,855,917 | 1,016,800 | 1.358 | 121,650 |

**S4 phase split** (fraction of `draw_sprite_fast`'s total cntvct ticks):

| band | decode/total | copy/total | other/total (glue) |
|---|---|---|---|
| light | 0.5537 | 0.1535 | 0.2928 |
| moderate | 0.6002 | 0.1183 | 0.2816 |
| heavy | 0.6000 | 0.1592 | 0.2408 |

**S4 per-unit costs:**

| band | decode ticks/srcrow | copy ticks/px | srcrows/frame | px/frame | sprites/frame |
|---|---|---|---|---|---|
| light | 28.44 | 0.0713 | 334.8 | 37,000 | 22.40 |
| moderate | 25.90 | 0.0722 | 740.8 | 52,372 | 41.98 |
| heavy | 37.05 | 0.0599 | 1,189.9 | 195,359 | 56.03 |

Sanity check: px/sprite averages 1,652 / 1,247 / 3,488 and srcrows/sprite
averages 14.95 / 17.65 / 21.25 across light/moderate/heavy — both plausible
for S16-class sprites (source rows shared across zoomed destination rows,
tens-to-low-hundreds of pixels wide), and both counters were available for
every window (no fallback cases).

**Deficit-per-srcrow vs deficit-per-px, across bands** (the row-vs-pixel
tracking test):

| band | deficit ns/frame | srcrows/frame | px/frame | deficit/srcrow (ns) | deficit/px (ns) |
|---|---|---|---|---|---|
| light | 287,491 | 334.8 | 37,000 | 858.63 | 7.7699 |
| moderate | 520,244 | 740.8 | 52,372 | 702.23 | 9.9336 |
| heavy | 1,016,800 | 1,189.9 | 195,359 | 854.56 | 5.2048 |

Coefficient of variation across the three bands: **deficit/srcrow = 0.0904**;
**deficit/px = 0.2531** (2.8x higher). Lower CoV = more constant = what the
deficit tracks.

## The two answers

**(1) Phase split.** DECODE is the single largest bucket (55.4-60.0% of
`draw_sprite_fast`'s time across bands), not OTHER — OTHER (glue: function
calls, schedule lookups, bounds arithmetic, the shadow-prefetch scan,
`glow_flush`) is second at 24.1-29.3%, and COPY (the per-pixel NEON/scalar
span replay) is smallest at 11.8-15.9%. So OTHER alone does not dominate.
But COPY — the only one of the three buckets whose cost is proportional to
*output pixels* — is the smallest bucket in every band: 84.1-88.2% of the
time is DECODE+OTHER, neither of which scales with output pixel count
(DECODE scales with source rows x source words per row; OTHER scales with
sprites/row-groups and fixed per-row bookkeeping).

**(2) What the deficit tracks.** Deficit-ns-per-source-row is nearly
constant across light/moderate/heavy (858.6, 702.2, 854.6 ns/row;
CoV = 0.090), while deficit-ns-per-pixel varies far more (7.77, 9.93, 5.20
ns/px; CoV = 0.253 — 2.8x the row-normalized variability, and not even
monotonic with load). The S4-vs-shipped deficit tracks **source rows per
frame**, not pixels per frame.

Bottom line (measured, not interpreted further): across the three load
bands, COPY (true per-pixel work) is 12-16% of S4's sprite time and the
regression normalizes far more consistently per source row than per pixel
— the row-count-proportional buckets (DECODE + OTHER), not the pixel-count-
proportional one (COPY), are what the S4-vs-shipped deficit tracks.
