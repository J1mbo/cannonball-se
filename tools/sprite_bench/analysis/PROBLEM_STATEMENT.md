# Sprite renderer: clean-sheet design brief for Cortex-A7 and Cortex-A53

Status: 2026-09-04, branch `ntsc-rearch`. Design and strategy only. No renderer code is
to be written yet.

**The task is not to optimise the current `hwsprites.cpp` incrementally.** It is to go
back to the upstream renderer, treat it as the functional specification, and answer:
*what should this routine look like* to run as fast as possible on Cortex-A7 (Pi 2 v1.1,
32-bit) and Cortex-A53 (Pi 2 v1.2, Pi 3, Pi Zero 2 W, 64-bit), given the memory system
those machines actually have and the other work running concurrently.

The fork's current renderer is prior art, not a starting point. Six years of layered
edits have produced 1003 lines, twelve unrolled kernels and a per-row dispatch switch,
none of it measured in isolation on either target. Some of its ideas will survive a
clean-sheet design; the point of this exercise is to find out which, by deriving the
structure from the workload rather than from the edit history.

Companion documents in this directory:

| Document | What it is |
|---|---|
| `PIPELINE.md` | Threading, sprite RAM/ROM formats, buffers, build flags, instrumentation, with file:line refs |
| `COMPARISON.md` | Upstream vs fork walk-through; commit-by-commit history with verbatim performance claims |
| `FABLE_ASSESSMENT.md` | Independent cost model and ranked bottlenecks for the *current* code (isolated Fable agent) |
| `AMIGA_PORT_NOTES.md` | What is publicly known about Chris White's OutRun: Amiga Edition sprite scaler |
| `SCHEDULE_VERIFICATION.md` | Brute-force proof of the scaling closed form, plus run-length distributions and periods |
| `MICROARCH_A7_A53.md` | Sourced Cortex-A7 and Cortex-A53 facts for this loop shape, with gaps flagged |
| `DESIGN_FABLE.md` | Clean-sheet design proposal from an isolated agent given only the specification and the two cores. **Superseded on one point: its line buffer is not needed, see `DESIGN_REVIEW_STAGING.md`** |
| `DESIGN_GLOW.md` | The System 16 glow quirk: minimal rule, direction analysis, deferral safety, recommended implementation and cost |
| `DESIGN_REVIEW_STAGING.md` | Review of whether the design's line buffer earns its place against direct and per-word-staged alternatives |

Upstream reference source (djyt/cannonball master, `hwsprites.cpp`, 323 lines) is at the
session scratchpad path given in section 12.

---

## 1. What the routine has to do

This is the contract. It is derived from upstream and from the OutRun sprite hardware,
and it holds regardless of implementation.

**Input per frame.** A list of up to 122 sprite records, each giving: a source bitmap
address in a 1 MB ROM, a pitch (words per source row), a height, a zoom factor, a screen
position, a horizontal mirror flag, a palette base, and a shadow-enable flag. The list is
terminated by an end marker and is read in list order; later sprites overwrite earlier
ones.

Correction, 2026-09-04: **the height field counts destination scanlines, not source
rows** (upstream line 206 feeding the loop bound at line 244, doubled for hi-res at line
254). An earlier revision of this brief said source rows and was wrong. The number of
output rows is therefore known before drawing starts and the last source row's vertical
run is simply truncated.

Also resolved: upstream keeps the running source-word index in sprite RAM word 7 as
scratch (lines 231, 275, 300), which raises the question of whether the emulated 68000
side reads it back. It does not need to. The fork has repurposed that word to carry a
precomputed height and the game works, so a new design is free to hold that index in a
register.

**Source format.** 4 bits per pixel, 8 pixels per 32-bit word, first pixel in the top
nibble. Nibble 0 is transparent. Nibble 15 is transparent *and*, when it appears in the
second-to-last position of a word, terminates the source row. Nibble 10 is a shadow
pixel when the sprite's shadow flag is set, and an ordinary colour otherwise.

**Output.** A 16-bit palette-slot buffer. An opaque pixel writes `nibble | colour_base`.
A shadow pixel does not write a colour: it ORs bit 12 into whatever the road, tile and
earlier sprite passes already left there, selecting the darkened version of that slot.
Transparent pixels leave the destination untouched.

**Geometry.** The sprite is a uniform nearest-neighbour scale of the source rectangle.
Both axes use the same accumulator recurrence with the same zoom value, and the engine
provably writes the same number to the horizontal and vertical zoom fields
(`osprites.cpp:648-649` and `697-698`), so `hzoom == vzoom` always. Mirroring reverses
the source in x only. Clipping is to the screen rectangle.

That is the whole job: **a uniform nearest-neighbour magnifier/minifier from 4bpp
source to 16-bit destination, with two transparent codes, a read-modify-write shadow
rule, an optional x mirror, and edge clipping.** Everything else in the current file is
implementation.

### 1.1 The scaling law, in closed form

The accumulator in upstream (`xacc`/`yacc` start at 0, add `zoom` per output step,
subtract 0x200 on carry) is equivalent to:

- Destination offset at which source pixel `i` starts: `N(i) = ceil(i * 512 / zoom)`
- Source index sampled at destination offset `d`: `src(d) = (d * zoom) >> 9`
- Run length of source pixel `i`: `r(i) = N(i+1) - N(i)`

Two consequences that the current implementation does not exploit:

1. **The horizontal schedule is identical for every row of a sprite.** `xacc` is reset
   to 0 at the start of each row and `zoom` is constant, so the source-to-destination
   mapping repeats unchanged for all `height` rows. It can be computed once per sprite,
   or looked up from a table built once at startup, instead of being re-derived per
   pixel per row.
2. **The vertical schedule is the same sequence as the horizontal one**, because the
   zoom is the same. One schedule serves both axes.

**This closed form is now verified.** See `SCHEDULE_VERIFICATION.md`: all of it holds
exactly, checked by brute force for every integer zoom from 1 to 0x7FF, a strict superset
of every value the tables can produce, over tens of millions of samples with no
mismatches. Both accumulator loops reduce to the single function `f(n) = (n * zoom) >> 9`,
so the schedule can be evaluated directly with a multiply and a shift and **no schedule
table is needed at all**. Two secondary results:

- The run-length sequence has a period equal to the odd part of the zoom, up to 489 in
  the hi-res set and 949 in lo-res. That is longer than any real sprite, so the idea of a
  small per-zoom schedule table keyed on a short period is dead. It is also moot, given
  the closed form.
- A run length of **zero is legitimate** whenever `zoom > 0x200`: a source pixel can
  contribute no output pixels at all. Any fast path must treat that as a normal case.

### 1.2 The zoom regimes, from the actual tables

Extracted from `src/main/engine/data/ozoom_lookup.hpp` (256 rows each; smaller zoom
value means larger on screen, so `zoom < 0x200` magnifies and `zoom > 0x200` shrinks):

| Configuration | Table | Halved by renderer? | Effective zoom range | Regime |
|---|---|---|---|---|
| lo-res | `ZOOM_LOOKUP` | no | 0x100 .. 0x3F0 | mixed: 128 magnify / 1 unity / 127 shrink table rows |
| **hi-res, `hiresprites` off (the shipping default)** | `ZOOM_LOOKUP` | yes | **0x80 .. 0x1F8** | **magnification only, 1.01x to 4x. Never shrinks** |
| hi-res, `hiresprites` on | `ZOOM_LOOKUP_HIRES` | yes | 0x80 .. 0x3F0 | mixed, 193 of 256 rows magnify |

The default shipping configuration is hi-res with `hiresprites` off. **In that
configuration every sprite is magnified**, every source pixel produces between one and
four output pixels on each axis, and no source pixel is ever skipped. The inner loop is
therefore pure run-length replication, not a general resampler. A design that treats the
magnifying case as the main path and the shrinking case as a secondary one is aligned
with what actually ships.

Note also that the minimum effective zoom is 0x80, comfortably above upstream's
"clamp below 0x40" guard, so that guard is dead code for sprites scaled through these
tables. Check `oanimseq.cpp:149`, which sources a zoom index from ROM, before relying on
this.

### 1.3 What the workload actually is

Measured run-length distributions, aggregated with equal weight over the distinct zoom
values in each set (`SCHEDULE_VERIFICATION.md`):

| Run length | Hi-res default (0x80..0x1F8) | Lo-res (0x100..0x3F0) |
|---|---|---|
| 0 | — | 12.3% |
| 1 | 24.7% | 62.1% |
| 2 | 37.3% | 25.6% |
| 3 | 24.9% | — |
| 4 | 13.2% | — |
| Mean per axis | **2.27** | 1.13 |

Because the same law governs both axes, one source pixel becomes an axis-aligned
rectangle of constant colour, `r_h` by `r_v`, each between one and four in the hi-res
default. **On average one decoded source pixel produces about 5.1 output pixels there,
and about 1.3 in lo-res.**

That is the single most important fact for the design. In the configuration that ships,
this routine is not a per-pixel plotter; it is a filler of small constant-colour
rectangles averaging roughly two by two. Nibble decoding, transparency testing and
schedule evaluation happen once per source pixel and are amortised over about five
output pixels. The cost that is not amortised is the stores. A design should be judged
first on how few store operations and how few destination cache lines it touches per
source pixel, and only second on its arithmetic.

Caveat: these shares weight every distinct zoom value equally. Real frames weight them by
how far away the objects are, which needs the per-frame zoom histogram in section 10.2.
The qualitative conclusion is robust because the entire hi-res range magnifies, but the
mean of 2.27 could shift.

---

## 2. Target CPUs

**Scope change, 2026-09-04 (maintainer direction).** Targeted development for the
Cortex-A7 is dropped. The Pi 2 v1.1 is a small share of the installed base and even the
Pi 2 v1.2 is an A53. **Tune for the A53.** The existing renderer, probably simplified,
stays as the scalar fallback for Pi 1 and Pi 2 class machines, so the A7 must keep
working but need not be fast. The A7 column below is retained because it still bounds
what the fallback may assume, but no A7 measurement is planned and no A7 machine is
being stood up.

Two cores matter. Both are in-order, both stall in place on a dependent load, and both
have one load/store pipe, so **the design goal on either is the same: fewer memory
operations, fewer unpredictable branches, and a small working set.** They differ enough
in register file and NEON capability that a single source must be written carefully.

Sourced detail, gaps and citations are in `MICROARCH_A7_A53.md`.

| | Cortex-A7 (Pi 2 v1.1) | Cortex-A53 (Pi 2 v1.2, Pi 3, Zero 2 W) |
|---|---|---|
| ISA as built | ARMv7-A, **32-bit**, **13 usable general registers** | ARMv8-A, **64-bit** (`-march=native` gives `cortex-a53+crc`), 31 general registers |
| Clock | 900 MHz stock (README notes 1.1 GHz overclock for 60 fps) | 1.0 GHz (Zero 2 W) to 1.4 GHz (Pi 3B+) |
| Pipeline | in-order, 8-stage, *partial* dual-issue | in-order, 8-stage, partial dual-issue |
| **Memory ops** | **one per cycle; a load and a store cannot co-issue** | **same** |
| L1 | 32 KB I + 32 KB D, 64 B lines | 32 KB I + 32 KB D, 64 B lines |
| L2 | 512 KB shared by 4 cores; hit latency not published | 512 KB shared; hit latency 15-17 cycles measured on silicon (ARM's own 8-cycle figure used an unrepresentative 2 MB L2) |
| Wide stores | `STRD` issues in 1 cycle, about double the throughput of two `STR` | `STP` not separately documented; the 128-bit load model suggests the same pattern |
| Store buffer | 4 x 64-bit slots, merges writes within a 64-bit aligned window | merges stores to the same 128-bit aligned address into one burst |
| Write-allocate escape | bus unit detects a **full line being written** and skips the linefill | not documented |
| NEON table lookup | `vtbl1..4_u8`: table up to 32 bytes but the **result is always 8 bytes** | `vqtbl1q_u8`: 16-byte table, **16-byte result**. A different capability, not just faster |
| NEON registers | 32 D-regs aliased in pairs as 16 Q-regs | 32 full 128-bit V-regs, no aliasing |
| Branch history | 256-entry pattern history table | 3072-entry |
| Indirect branch targets | **8-entry BTAC** | 256-entry array, but measured tracking is nearer two targets per branch |
| Branch mispredict | not published | 7 cycles measured |
| Memory | LPDDR2, 32-bit bus shared with the GPU; ~170 cycles to DRAM; roughly 1-2 GB/s achievable | same class |
| Pages | 4 KB, no transparent huge pages, ~10-entry data micro-TLB | same |
| Data prefetcher | not documented for either core; `PLD`/`PRFM` are hints with no defined timing. **Measure, do not assume** | same |

Three consequences follow directly and should shape every proposal.

**Memory operations are the binding constraint, on both cores.** One per cycle, and a
load cannot pair with a store. So a design is judged by counted memory operations, not by
instruction count. Combined with section 1.3, the target metric is memory operations per
*source* pixel, given that each one becomes about five output pixels in the shipping
configuration. A read-modify-write costs at least two issue slots that cannot overlap.

**Branchy dispatch is a bad bet on A7 specifically.** An eight-entry indirect branch
target cache cannot hold a twelve-way or sixteen-way `switch`, and the current renderer
executes exactly such a switch once per output row. On A53 the array is large enough in
principle but measured behaviour tracks far fewer targets per site. Branch-free
formulations are favoured on both, and strongly on A7.

**Overlapping wide stores look cheap on both.** Neither TRM states a penalty, and both
store buffers merge writes within an aligned window, so "store the widest run then
advance by the actual run length" should cost one issue slot per store with no
overlap-specific hazard. This wants confirming by measurement, since it is reasoned from
the merge mechanism rather than from a published cycle figure.

Register pressure is the sharpest difference. A design that keeps three or four
destination row pointers plus a source pointer, colour, accumulator and loop counters
live at once fits comfortably on A53 and spills on A7. Any per-sprite precomputed
schedule must also be small enough to stay in L1 alongside everything else.

**Two further targets constrain portability**, though they are not performance targets:
the Pi B+ and Pi Zero (W) are ARMv6 with no NEON at all and are 30 fps only, and Windows
x86-64 must build under MSVC. So any vector path needs a scalar fallback, and the
existing portable layer in `src/main/sdl2/snes_ntsc_simd.h` is the pattern to follow. It
already detects NEON via `__ARM_NEON`, which is true on both ARMv7 and ARMv8, so a NEON
path written with only the intrinsics common to both will compile for A7 and A53 alike.

Performance counters are available from user space on the A53 test box without the
`perf` tool: `perf_event_open` with `exclude_kernel` works (`perf_event_paranoid` is 2,
PMU `armv8_cortex_a53`, six programmable counters plus the cycle counter). Verified raw
event codes: 0x01 L1I refill, 0x03 L1D refill, 0x04 L1D access, 0x05 L1D TLB refill,
0x08 instructions, 0x10 branch mispredicts, 0x11 cycles, 0x12 branches, 0x13 memory
accesses, 0x16/0x17/0x18 L2 access/refill/writeback, 0x19 bus access, 0x60/0x61 bus
loads/stores, 0xE7 cycles stalled on loads, 0xE8 cycles stalled on stores, 0xE1
interlock stalls, 0xC7 store-buffer-full. Events 0x23/0x24 are not implemented. A
40-line `perf_event_open` wrapper is all the harness needs.

**Test machines.** The A53 box `sl-d303` (Pi Zero 2 W) is available now, with its
unrelated services stopped. The maintainer will stand up a Pi 2 v1.1 for A7 and a Pi 3
for A53; **neither is available yet**, so nothing can be measured on A7 and the Pi 3's
higher clock cannot yet be separated from the Zero 2 W's. Until they exist, A7 must be
reasoned about rather than measured, and the design must avoid choices known to punish
it: register pressure above thirteen live values, indirect branches, and any reliance on
a 16-byte table lookup that A7 cannot perform.

---

## 3. The environment the routine runs in

### 3.1 Concurrency

On Linux with four threads the frame is a three-stage pipeline:

| Thread | Work | What it does to the memory system |
|---|---|---|
| main | input, audio, GPU texture upload of frame n-2 and GLSL scaling | streams a full RGB surface to the GPU across the shared bus |
| prepare | game logic, then road, tiles, **sprites**, text into the frame-n slot buffer | the thread this brief is about |
| render 0/1 | NTSC composite filter over frame n-1, half each | streams the other slot buffer plus a 15.8 MB kernel table through the shared L2 |

The sprite pass therefore never runs on an idle machine. It competes for a 512 KB L2
with a filter whose table alone is thirty times that size, and for a memory bus that the
GPU is also using. **Any measurement taken on an otherwise idle core will overstate the
benefit of anything that trades memory traffic for compute, and understate the benefit
of anything that reduces cache footprint.** The bench must reproduce this contention.

Budget: 16.7 ms per frame for the whole prepare thread at 60 fps. If the average frame
rate falls below 50 the game demotes itself to 30 fps (`main.cpp:684-696`), which is the
failure this work exists to prevent. The split of that budget between game logic, road,
tiles, sprites and text on either target is **unknown and must be measured first** — it
is possible that sprites are not the largest term.

### 3.2 Destination buffer

| Mode | Buffer | Bytes | Sprite output pixels vs lo-res |
|---|---|---|---|
| lo-res | 320x224 | 143 KB | 1x |
| lo-res widescreen | 404x224 | 181 KB | ~1.3x |
| **hi-res (default)** | 640x448 | 573 KB | ~4x |
| hi-res widescreen | 808x448 | 724 KB | ~5x |

In hi-res the destination alone exceeds the shared L2 while the NTSC thread streams a
second buffer of the same size through it. Sprite stores in hi-res will largely be
write-allocate misses. Each buffer carries a 128-byte front pad and two spare rows
(`video.cpp:65-69`); the current unclipped kernels rely on that slack for small
overruns, and any design using wide or overlapping stores must state exactly what
overrun it needs and where.

Sprites are drawn after road and tiles and before text, directly into the shared buffer,
with no intermediate layer. Every caller passes `set_x_clip(false)`, so the clip window
is always the full screen width and "clipping" only ever means "this sprite crosses a
screen edge".

---

## 4. What upstream does, and what it costs

Upstream is one loop, one `draw_pixel()` macro, two copies of the inner loop for the
mirror case, and per-pixel branches for clipping, transparency and shadow. Per output
pixel it runs the accumulator test, a clip range test, a transparency test, and either a
16-bit store or a load-or-store shadow update. It keeps the running ROM address in
sprite RAM (`ramBuff[data+7]`), which forces a reload after every pixel store because
the compiler must assume the `uint16_t*` destination may alias it. The fork's first
useful change was to hold that address in a local.

Order of magnitude on either target: roughly ten instructions and two to three
data-dependent branches per output pixel, so eight to twelve cycles once mispredicts are
included, before any memory stall. At ~300k sprite output pixels in a hi-res frame that
is 2.5 to 4 ms of a 16.7 ms budget, plus a memory term that only appears under
contention. These are estimates, not measurements; producing the real numbers is the
first task in section 10.

The costs worth attacking, stated as properties rather than as patches:

1. **Work is proportional to output pixels, not source pixels.** In the default hi-res
   configuration each source pixel is replicated one to four times per axis, so up to
   sixteen output pixels do the full per-pixel decode, test and store sequence for one
   source nibble that was already decoded.
2. **The schedule is recomputed per pixel** even though it is identical for every row of
   the sprite and derivable in closed form.
3. **One 16-bit store per output pixel** on a core with a single load/store pipe. A
   four-pixel run is four stores where one 64-bit store would do.
4. **Two to three unpredictable branches per output pixel**, on cores that flush eight
   or more cycles per miss and cannot overlap anything across one.
5. **The shadow rule forces a read-modify-write**, which is the only reason the pass
   reads its destination at all.

---

## 5. Prior art in this fork, and why none of it is settled

Full detail in `COMPARISON.md`. Summary of the techniques tried since February 2025:
an unsigned range test that folds "not 0 and not 15" into one comparison (survives); a
merged mirror loop with prefetch (reverted within days); inline functions per shadow and
mirror case (reverted); a return to a MAME-shaped loop; an eight-way split by shadow,
direction and mirror; an engine-computed "this sprite crosses an edge" bit feeding a
sixteen-way `switch`; writing up to four destination rows from one decoded source row;
and finally a lazily built mirrored copy of the whole 1 MB ROM plus a 256 KB per-row
shadow-presence table.

Three things to carry into the design work:

- **Every performance claim in that history is a whole-game frame rate on a Pi 2 v1.1**,
  which is the A7. None was an isolated sprite measurement, none was on A53, and none
  used counters. The claims are 20%, then ~30% against MAME, then "significantly". They
  may all be true and they may all be measuring the same thing twice.
- **The "jump table" is a `switch` executed once per output row**, on a key of
  (rows-in-this-group, crosses-an-edge, has-shadow). Two of those three bits are
  constant for the whole sprite. On an in-order core an indirect branch whose target
  changes every few rows is a poor trade against the branches it removes, and the twelve
  kernel bodies cost L1I against the tile, road and engine code that runs in the same
  frame. This is the single most important thing to settle by measurement, because it is
  the technique the maintainer has invested most in.
- **The multi-row write is the one idea that attacks the real structure**, because it
  exploits the fact that vertical magnification replicates rows. It is currently capped
  at three rows and contains a defect: the three-row kernels write rows y..y+2 but
  advance y by two, so the third row is drawn again on the next iteration. Output is
  unaffected; the stores are wasted, on exactly the large close-up sprites that cost
  most.

Known defects, verified, to be fixed before any baseline is taken so that the baseline
is honest: the unguarded `#define PIXEL_ACCURACY 0` at `hwsprites.cpp:58` silently
overrides the CMake option, so the "glowy edge" shadow behaviour the history repeatedly
claims to ship has never been compiled; the redundant third row above; and two
`std::chrono` reads per sprite plus three static counter arrays live in the hot path and
read by nothing.

---

## 6. The design space

This section frames the axes. It is deliberately not a proposal; the agents' job is to
work out which combination is right and why, for both cores.

**A. Where the scale schedule lives.** Per output pixel (upstream and the fork); once
per sprite as a run-length array; or once at startup as a table indexed by zoom, since
there are only 128 distinct zoom values in `ZOOM_LOOKUP` and 159 in
`ZOOM_LOOKUP_HIRES`. What does each cost in L1 footprint, and what does it save?

**B. What the inner loop iterates over.** Output pixels (today) or source pixels. If
source pixels, a run of length 1..4 can be emitted with a single wide store, optionally
using the overlapping-store trick: always store the widest run, then advance the pointer
by the actual run length so the next store overwrites the excess. That is branch-free
and one store per source pixel, but it needs a stated overrun bound and it interacts
badly with transparency, which must not write at all. Where does that trade land?

**C. How transparency is handled.** A branch per source pixel; a mask; or a
destination-blend that loads the destination, selects, and stores back. The blend turns
N stores into one load and one store per vector, which is a large win on a single
load/store pipe, but it doubles the traffic on lines that were going to be fully
overwritten anyway.

**D. How the shadow rule is handled.** It is the only reason the pass reads its
destination. Shadow pixels are rare but expensive. Is there a formulation that keeps the
semantics without a dependent load on the common path, and is a per-sprite or per-row
"contains no shadow" test worth its table?

**E. How mirroring is handled.** A second copy of the ROM (today, 1 MB); mirroring on
the fly with a reverse and a nibble swap (about five instructions per source word,
amortised over up to 32 output pixels); or a mirrored schedule that walks the source
backwards. Which wins when L2 is the contested resource?

**F. How edge-crossing sprites are handled.** A separate clipped kernel (today); a
closed-form skip to the first visible source pixel and stop at the last, so the clipped
path disappears; or clamping the schedule at the edges.

**G. Vector or scalar, and which vector.** A NEON path must compile for ARMv7 and
ARMv8, which rules out 128-bit table lookups. Is there a formulation using only the
common subset that beats a good scalar loop on both, given the 64-bit datapath means a
128-bit operation occupies two issue slots on each core?

**H. Dispatch.** Per-row indirect branch on a composite key; per-sprite kernel
selection, since the mirror, shadow-enable, edge-crossing and magnify/shrink regime are
all constant for a sprite; or one branch-free kernel that handles everything. What is
the L1I cost of each on a 32 KB instruction cache shared with the rest of the frame?

**I. Where the work happens.** The engine builds the sprite records on the same thread
immediately before the renderer consumes them. Anything computable once per sprite could
move there, but its cost counts against the same budget. Candidates: the run-length
schedule, the regime classification, the first and last visible source pixel, and a
y-sorted list so the destination is walked top to bottom.

**J. Destination locality.** In hi-res the buffer does not fit L2. Options include
software prefetch at a tuned distance, pairing the stores of adjacent identical rows,
and composing all layers band by band so that road, tiles, sprites and text touch a
32-row strip that stays resident. The last is a cross-layer change and only worth it if
measurement shows stores dominate.

---

## 7. Constraints

Fixed:

- **Correctness reference.** Build a faithful scalar model of upstream's algorithm as
  the golden reference, confirm what it does and does not agree with in today's shipped
  output for each mode, and enumerate every difference deliberately rather than
  inheriting it. Gate every optimised variant against that model, byte for byte, on
  captured frames, in all four mode combinations. Where a difference from today's output
  is intended, it must be a stated decision, not a side effect.
- The slot-buffer format (16-bit palette slot, bit 12 shadow) is consumed by the NTSC
  filter's slot-indexed table and by the RGB path. Do not change it.
- Must build for ARMv6 (scalar), ARMv7 NEON, ARMv8 NEON, and x86-64 under GCC, Clang and
  MSVC. Vector paths behind the portable layer with a scalar fallback.
- No new threads. All four cores are already committed. No huge pages.
- Resident memory not above today's 2.3 MB of sprite tables, and preferably below it.

Open, and fair game to change:

- The sprite record layout and the division of work between the engine and the renderer.
- The in-memory ROM encoding. `sprites[]` is already a re-encoding of the ROM built at
  startup; it could be re-encoded differently, if the resident and per-frame touched
  footprints both stay within budget.
- The mirrored and shadow-info side tables.
- The order in which layers are composed, if per-pixel results are identical.

Explicitly a maintainer decision, not an optimisation, and out of scope for the design:
whether sprites need true 2x sampling in hi-res at all. Hi-res costs four to five times
the sprite pixels and pushes the destination out of L2. Rendering sprites once and
doubling, or having the NTSC filter consume a lo-res sprite layer, would be far cheaper
and is not bit-exact. State the cost, do not assume the answer.

---

## 8. The Amiga comparison

The premise that Chris White ported CannonBall to an Amiga 1200 with a 60 MHz 68040 and
reached 60 fps does not match any published source. What he shipped is "OutRun: Amiga
Edition", a ground-up 68k assembly engine explicitly not based on CannonBall, targeting a
68030 at 50 MHz minimum and a 68040 at 25 MHz recommended, running at a selectable 25 or
30 fps, with owner reports on real hardware clustering between 12 and 25 fps. Details and
sources in `AMIGA_PORT_NOTES.md`. If the maintainer has first-hand information that
differs, correct that document.

The yardstick still holds and is still humbling. A 25 to 50 MHz 680x0 with no cache
hierarchy worth the name produces full-screen scaled sprites at 25 to 30 fps. A 1 GHz
A53 has twenty to forty times the cycles per frame at 60 fps. If a hi-res frame needs
300k sprite pixels, ten cycles each is 3 ms and three cycles each is 1 ms.

The one technique documented, second-hand from summaries of his sprite-scaling video, is
that the scaler is 34 specialised routines rather than one general one, split on grow
versus shrink, clipping, mirroring, shadow and unscaled, chosen so bounds checks can be
eliminated where provably unnecessary. That is specialisation **per sprite**, on the
regime, which is axis H above and is not what the fork does (it specialises per row, on
a key that is mostly constant per sprite). The tutorial source is a zip attached to the
itch.io devlog for that chapter; it is not linked from the static page and was not
downloaded. If the maintainer fetches it into `tools/sprite_bench/analysis/amiga/`, a
follow-up agent can extract the dispatch structure and inner-loop shapes.

---

## 9. What is wanted from the design agents

A design, argued from the workload and the two microarchitectures, for what this routine
should be. Not a list of patches to the current file.

Deliver:

1. **The recommended structure**, in enough detail to implement: data layout, what is
   precomputed and when, the dispatch scheme, the shape of the inner loop, and how
   mirroring, shadow, clipping and the two zoom regimes are handled. Pseudocode or
   annotated C for the main path, plus the register budget on A7 (14 usable registers)
   and A53.
2. **A cycle estimate per output pixel and per source pixel** on each core, built from
   the instruction mix, stated assumptions, and where the estimate is weakest.
3. **Which parts are shared and which diverge between A7 and A53.** If a single
   implementation is right for both, justify it. If not, say exactly where they part and
   what the maintenance cost is.
4. **What it would take to get it wrong**: the failure modes, the exactness risks, and
   the specific properties that must be proven (exhaustively where the state space
   allows, such as the schedule closed form over all zoom values).
5. **A staged build order** where each stage is independently measurable and revertible,
   with the kill rule for each, following the pattern the NTSC filter work used.
6. **An honest account of what the current fork already does well**, so that the
   maintainer's existing investment is either carried forward with a reason or retired
   with a reason.

Specific questions to answer or challenge:

- Q1 Is the closed-form schedule the right foundation, and where should it be
  materialised: per pixel, per sprite, or in a startup table indexed by zoom?
- Q2 Should the inner loop iterate over source pixels and emit runs, rather than over
  output pixels? What is the run-length distribution in the default configuration, and
  does the overlapping-store trick survive the transparency and overrun constraints?
- Q3 Given that the default hi-res configuration never shrinks, is a magnify-only main
  path with a separate shrink path the right split, and what does that do to code size?
- Q4 Is per-sprite specialisation better than per-row dispatch on these cores, and how
  many variants can a 32 KB L1I afford when tiles, road and the engine share it?
- Q5 What is the right treatment of the shadow read-modify-write, which is the only
  reason the pass reads its destination?
- Q6 Does a mirrored ROM copy earn 1 MB of a contested 512 KB L2, against mirroring on
  the fly?
- Q7 Is there a NEON formulation, restricted to the intrinsics common to ARMv7 and
  ARMv8, that beats a good scalar run-length loop on both cores? Be specific about the
  64-bit datapath and about A7's weaker NEON.
- Q8 What should the engine precompute, given its cost lands in the same budget?
- Q9 In hi-res, how much of the cost is destination misses rather than issue, and which
  of prefetch, store pairing or band composition addresses it best?
- Q10 What in the surrounding pipeline makes this pass slower than it needs to be:
  buffer alignment, row stride, layer order, the double-buffer swap, or the cache state
  the prepare thread inherits from the game tick?

Ground rules: target these two cores specifically. "Use SIMD" and "use a jump table" are
not answers. Give the mechanism, the reason it helps on an in-order core with one
load/store pipe and a contested L2, the expected effect as an order of magnitude, and
the way to falsify it.

---

## 10. Measurement plan

Nothing in sections 4 to 6 is measured. This plan produces the numbers that decide the
design, and mirrors `tools/ntsc_bench`.

### 10.1 Capture

Per captured frame, written around the sprite call in `Video::prepare_frame()`: the mode
flags and dimensions; the sprite list the renderer reads (4 KB); the slot buffer before
sprites and after sprites (143 to 724 KB each). Once per run, the converted `sprites[]`
ROM image so the bench needs no ROM loader. Capture attract mode across several stages
and at least one player-driven sequence in each of the four mode combinations, because
attract mode may not exercise edge crossing and close-up sprites the way play does.

### 10.2 Bench harness

`tools/sprite_bench/`, built the same way as the NTSC bench: compiles the renderer under
test with stub globals so the shipped code is what is measured; replays captured frames
onto the recorded background; checksums the result and compares against the recorded
output; prints a RESULT line with ms per frame, checksum and counters. Counters via
`perf_event_open` in two groups, normalised per frame and per output pixel. Contention
modes: idle, a synthetic L2 thrasher on another core, and the real NTSC bench running on
two other cores. **The third is the one that decides anything.**

Statistics needed from an instrumented build, because they determine which regimes
matter: sprites per frame and their size distribution; output pixels; the zoom histogram
after halving; the run-length distribution; the fraction of source words that are wholly
transparent; the mirrored, shadowed and edge-crossing fractions; distinct destination
cache lines and pages touched; and bytes of ROM touched.

### 10.3 Whole-game numbers

A `-perftest` breakdown printed periodically, splitting the prepare thread into game
tick, road and tiles, sprites, and text, one clock pair per phase per frame. This
replaces the dead per-sprite instrumentation and answers whether sprites are even the
dominant term. Running the game on the A53 box needs the SDL2 and GL development
packages and a display path; that is stage 0 work. WSL gives functional checks only.

### 10.4 Experiments, in order, each with a kill rule

| ID | Question | Read | Keep if |
|---|---|---|---|
| E0 | Baseline, after fixing the three known defects | all counters, cycles per output pixel, phase split | reference checksums recorded |
| E1 | Is the closed-form schedule exactly equivalent to the accumulator loop? | exhaustive comparison over every zoom value in both tables and every start phase | **DONE, 2026-09-04. Proven exact; see `SCHEDULE_VERIFICATION.md`** |
| E2 | Memory or issue? Idle vs thrasher vs real NTSC load | cycles, load-stall, store-stall, L2 refill | decides whether locality work or issue work leads |
| E3 | Per-row switch vs per-sprite selection vs one generic kernel | cycles, branch mispredicts, L1I refills, instructions per pixel | best cycles under NTSC load |
| E4 | Source-pixel run loop vs output-pixel loop | stores per frame, cycles | fewer cycles at identical output |
| E5 | Mirrored ROM copy vs on-the-fly mirroring | L2 refills, cycles under load | keep the cheaper under load |
| E6 | Prefetch distance on the destination | store-stall cycles | improves under load without regressing idle |
| E7 | Vector inner loop, prototyped on the main path first | cycles per pixel, branch mispredicts | at least 2x on that path and bit-exact |
| E8 | Band composition across layers | L2 refills, store stalls | only if E2 still shows stores dominant |
| E9 | Hi-res versus lo-res cost on the same frames | output pixels, cycles | informs the maintainer's hi-res decision |

Each stage lands as its own commit with the RESULT lines in the message, as the NTSC work
did.

---

## 11. Immediate next steps

1. Fix the three defects in section 5 so the baseline is honest. **The `PIXEL_ACCURACY`
   (System 16 glow) question is now settled; see `DESIGN_GLOW.md`.** An earlier revision
   of this brief asserted that restoring the glow would constrain every vector design,
   because it writes to the pixel left of the current one. **That claim is retired.** A
   colour write always leaves bit 12 clear, so the clear at `x-1` is a proven no-op
   everywhere inside a run of opaque pixels. The quirk reduces to one clear per maximal
   visible colour run, at `lo-1`, when `lo > x1`. Verified by enumerating every type
   sequence up to length seven, every bit-12 pattern, both directions and three clip
   windows: 30.2 million cases, no mismatches against the reference.

   Consequences: the colour path stays a pure store stream, no vector kernel is
   constrained, and the CMake option should be restored to mean what it says with the
   glow enabled by default. Cost is roughly 0.03 to 0.1 ms per frame against about 60x
   that for the reference's per-pixel read-modify-write. Two details carry into any
   implementation. Deferring the clears is safe per row or per sprite but **wrong per
   frame**, because a later sprite's shadow at the same address does not commute with the
   clear. And mirror normalisation stays valid only with one amendment: for a sprite
   drawn right to left, suppress the clear when the pixel at `lo-1` is a shadow pixel of
   the same sprite row, which is the one case where the two directions genuinely differ.
2. ~~Verify the closed-form schedule exhaustively.~~ Done 2026-09-04, proven exact.
3. Build the capture and the bench, and take E0 and E2. Until the phase split exists,
   nobody knows whether sprites are the right target at all.
4. Find or rule out a Cortex-A7 test machine.

---

## 11b. Scope widened to road and tiles (2026-09-04)

In-situ measurement on a properly powered Pi Zero 2 W changed the priorities. At 60 fps
with no dropped frames, the prepare thread splits as road and tiles 2.20 ms, sprites
0.95 ms, text 0.30 ms. **Road and tiles costs 2.3 times the sprite pass** and is hurt
more by concurrency: at an identical workload, four-thread operation inflates it by 52%
against 30% for sprites.

So the sprite renderer is no longer obviously the right first target. Profiling of the
road and tile layers is underway: the timing has been split five ways
(`update_tile_values`, road background, tile background, tile foreground, road
foreground) and the two renderers are being read for a cost model, in
`ROAD_ANALYSIS.md` and `TILES_ANALYSIS.md`. The techniques that won for sprites should
transfer, since all three layers write the same 16-bit buffer and the road layer in
particular fills every pixel of every row, which is the case wide stores suit best.

## 11a. Settled design decisions

**CORRECTION (2026-09-05): keep the line buffer (LINEBUF), not fan-out.** The
predicted fan-out win below came from the slot-counting model in
`DESIGN_REVIEW_STAGING.md`. It was overturned by measurement on real A53:
`MEASURED_A53_PI3.md` found the line-buffer replay (LINEBUF) is faster than
per-source-pixel fan-out (FANOUT) in every condition (6.08 vs 7.91 cyc/px idle
L1-resident, still ahead under load), because both touch the same destination lines
so the memory term is common and LINEBUF's win is a pure compute gap. `DESIGN_REVIEW_
BANDWIDTH.md` (2026-09-05) upholds LINEBUF against the in-situ memory findings and
retires the slot model as a predictor. The paragraph immediately below is kept for
history but its recommendation is superseded; the overrun-safety analysis in it is
still valid and applies to the LINEBUF line-buffer fill.

**Staging (`DESIGN_REVIEW_STAGING.md`, 2026-09-04) — SUPERSEDED, see correction above.**
The scratch line buffer proposed in
`DESIGN_FABLE.md` is not needed and should be dropped. Decode each source row once and
fan each wide store out to all `r_v` destination rows through immediate offsets from one
base pointer, making run ends exact with a one-pixel lookahead. Memory slots per output
pixel at the mean fall from 0.85 to 0.55, and issue cycles per output pixel from 2.7 to
1.9 on A7 and 2.1 to 1.25 on A53. The separate case for vertical run 1 then comes free as
a template instance rather than needing its own path.

The line buffer's stated justification, that it is the only way to make the overrunning
wide store safe, is wrong. Independently verified here: with a one-pixel lookahead giving
the last pixel of a run an exact-width store, the second-to-last pixel's excess stays
inside the run exactly when `r_i + r_(i+1) >= MAXRUN`. That holds for every magnifying
class, tight with zero slack at zoom 0xCD for the four-wide class and 0x156 for the
two-wide class, and it holds for every value in both zoom tables as actually used.

**One reachable edge case, not covered by that inequality.** For `zoom >= 0x402` two
adjacent runs can both be zero, and then the excess of a one-wide store lands in the
following span. This is reachable: 32 raw `ZOOM_LOOKUP_HIRES` values exceed 0x402, and
they are used unhalved whenever `hiresprites` is on while `hires` is off. That
combination is guarded in the menu (`menu.cpp:1150-1156` refuses it and clears the flag
when hi-res is turned off) but **not** by the F7 hotkey (`input.cpp:344`), which toggles
`hiresprites` unconditionally, nor by the config file, which reads the two settings
independently. The implementation must therefore skip the store entirely when a run's
final pixel has run length zero, rather than relying on the inequality. Worth fixing the
unguarded hotkey separately.

## 12. File map

| Path | Role |
|---|---|
| `src/main/hwvideo/hwsprites.cpp`, `.hpp` | the current renderer, 1003 + 46 lines |
| `src/main/engine/osprites.cpp`, `osprite.cpp` | engine-side record construction, zoom selection, clip bit, width, offset |
| `src/main/engine/data/ozoom_lookup.hpp` | the two zoom tables analysed in section 1.2 |
| `src/main/video.cpp` | buffers and the layer composition order |
| `src/main/main.cpp` | thread model, `-t`, `-perftest`, `-dump`, the 30 fps fallback |
| `src/main/sdl2/snes_ntsc_simd.h` | the portable SIMD layer pattern to reuse |
| `tools/ntsc_bench/` | harness pattern, `run_pi.sh`, captured frames, frame tools |
| `tools/sprite_bench/analysis/` | this brief and the four companion reports |
| scratchpad `.../scratchpad/upstream/hwsprites.cpp` | upstream reference source, fetched 2026-09-03 |
