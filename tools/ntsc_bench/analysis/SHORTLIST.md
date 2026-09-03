# SIMD strategy shortlist for the hires NTSC filter

Status: analysis only, nothing implemented. Date: 2026-09-03.

Companion documents in this folder:

- `BRIEF.md`: facts handed to the independent reviewers (hardware, call contract, measurements, goal).
- `ALGORITHM.md`: de-macroed description of `snes_ntsc_init` and `snes_ntsc_blit_hires`, verified against a preprocessor expansion.
- `IDEAS_A.md` (memory-first), `IDEAS_B.md` (parallelism-axis-first): five proposals each, written independently and without sight of the existing SIMD code or of each other.
- `REVIEW.md`: independent audit, merge and ranking of the ten proposals, with a dated addendum after the page-spread experiment.

The three Fable reviewers saw only `tools/ntsc_bench/scalar_ref/` (the filter from commit 086591c with every SIMD region stripped, proven unchanged by checksum) plus the brief. They were not shown `snes_ntsc_blit_hires_fast`, `_fast2`, or any of this conversation's conclusions.

## 1. What the scalar filter is

`snes_ntsc_blit_hires` is a plain 14-tap FIR filter on a 6-input, 7-output resampling grid. Each output pixel sums 12 packed 32-bit words, one from each of the 12 most recent input pixels' 512-byte table entries, then applies a 5-op clamp and unpacks to RGBA. Adjacent output pixels read adjacent words within each entry's 14-word block. Table words are per-tap integer roundings of a function linear in the colour's YIQ, followed by an error-redistribution pass, so they cannot be recomputed on the fly bit-exactly. Rows are independent; the only state between chunks is the sliding window of 12 entry pointers. Full detail in `ALGORITHM.md`, including quirks: the last two input pixels of a row are never read, saturation clamps to 254, and the composite preset carries `merge_fields = 1` which the game clears.

## 2. Where the scalar time goes (measured on sl-d303, 640 x 448, one thread)

| Distinct colours N | 1 | 16 | 64 | 256 | 1024 | 4096 | 65536 |
|---|---|---|---|---|---|---|---|
| ms/frame, values random over 0..65535 | 10.98 | 13.01 | 19.03 | 22.20 | 47.79 | 83.95 | 93.53 |
| ms/frame, values contiguous 0..N-1 | | | 13.27 | 20.29 | 37.86 | | |

At 335,552 outputs per frame and 1 GHz, N = 1 is the pure instruction cost of the compiled scalar code: about 33 cycles per output. The benchmark case (N = 256, random) is about 66 cycles. The reviewer's decomposition, which the contiguous-palette experiment supports: roughly 33 cycles issue, 16 cycles micro-TLB refills (the A53 L1 data TLB holds about 10 pages and 256 random entries live on 256 pages), 16 cycles L1 misses served from L2. The two stall terms are of equal size and need two different levers. Transparent huge pages are not available on the target kernel, so page compaction has to be done in user space.

For the goal of 2x on this case the implementation needs both a cheaper kernel (below 33 cycles) and a smaller stall (below 33 cycles).

## 3. Approaches considered

Ten proposals merged into eight distinct approaches (`REVIEW.md` section 3):

| Id | Approach | Verdict |
|---|---|---|
| AO | Adjacent outputs in lanes, contiguous 16-byte loads, table unchanged | shortlisted, build first |
| RI | Same-phase row interleave (rows r, r+3, r+6 processed chunk by chunk) | shortlisted, cheap |
| CT | Compact phase-major slot table for the colours actually on screen | shortlisted, page lever and cliff insurance |
| RK | Run-length kernels (flat chunk, pair) cached per colour | shortlisted, real frames only |
| HP | Huge pages on the master table | dead: no THP on the target kernel |
| P8 | int8 planar table with 16-bit lanes | not pursued: no faster than AO, conditional exactness |
| TG | TBL-instruction gather over per-band tiny tables | not pursued: nothing on general content, ARM-only, 15+ days |
| RL | Rows in lanes (multi-line, lanes = rows r, r+1, r+2 with phases 0,1,2) | not pursued, see section 5 |

## 4. Shortlist, in build order

### 4.1 AO: adjacent output pixels in lanes

Four consecutive output pixels per 128-bit register. Each input pixel contributes 4 or 5 unaligned 16-byte loads from its 14-word kernel block, added into 4-lane accumulators; block edges are handled with `ext` against a zero register. Loop unrolled over 4 chunks (24 inputs, 28 outputs, 7 accumulators) so all offsets are immediates. Clamp and unpack are the scalar bit sequence applied per lane, so the 254 saturation case is reproduced. Bit-exact by construction: the same 32-bit words are summed and wrap-around addition is order-independent. Loads per output fall from 12 to about 3.6; vector ops about 8 per output. Ports to plain SSE2 (`_mm_loadu_si128`, `_mm_add_epi32`, `_mm_slli_si128`) and RVV (`vle32`, `vadd`, `vslideup`).

Expected: 1.7 to 2.5x alone on the benchmark. Effort 5 to 7 days including edge handling, the x86 macro layer and a bit-exact harness.

Kill experiment, one day: hand-write the unrolled 28-output body with intrinsics, no edge handling, on a 16-colour row and a 256-colour row repeated 448 times; count cycles per output and compare sums with scalar. Go if the 16-colour case is at or under 22 cycles and the 256-colour case at or under 34. Amber (16-colour fine, 256-colour 34 to 45): the kernel is right and memory-bound, proceed only together with RI and CT. No-go if the 16-colour case exceeds 28: the 2-cycles-per-128-bit-op model is wrong and no vector kernel reaches 2x.

### 4.2 RI: same-phase row interleave

Pure loop-order change, no table change: process rows r, r+3 and r+6, which share a burst phase and therefore read the same 168-byte sub-block of each entry, chunk by chunk so that lines fetched for one row are reused by the next two while still in L1. Folds into AO's register allocation at chunk granularity.

Expected: 1.2 to 1.3x on top of AO on the reviewer's estimate. Caveat from the coordinator: on the spatially random benchmark, rows share colours only by chance, so the gain there may be small; on real frames, which are vertically coherent (sky, road), it should be much larger. The kill experiment must therefore be run on a vertically coherent input as well as the random one.

Kill experiment, two hours: reorder the scalar row loop into the triplet order and rerun the sweep at N = 64, 256, 1024, plus the mixed pattern. Go if N = 256 drops 8 or more cycles per output or the mixed pattern drops proportionally more.

### 4.3 CT: compact phase-major slot table

Copy the three 168-byte phase blocks of each distinct colour seen into a compact phase-major table indexed by a small slot number, with a 64K-entry remap from input value to slot (or, better, have the renderer's existing palette pass emit slot indices directly, which removes the remap load and its L1 footprint). 256 colours then occupy 11 pages instead of 256, and a row with fewer than about 190 colours fits in 8 pages, which removes the micro-TLB term entirely. Exact: words are copied, never recomputed.

Expected: about 1.2x on scalar at 256 colours, 1.4x on rows with 64 or fewer colours, roughly 1.1x marginal on AO plus RI on the benchmark, and the only defence against the large-palette cliff (N = 1024 falls from 144 to an estimated 50 to 70 cycles per output). Optionally, with lazy per-colour kernel generation, the 32 MiB master table can be dropped for a table of about 1.3 MB. Effort 3 to 4 days including two-thread slot allocation and a per-frame cap on new colours.

Kill experiment, half a day, scalar code only: copy the 256 live entries into a phase-major 129 KB buffer, pre-remap the synthetic frame to slots 0..255, run N = 256 and N = 1024; predicted 52 to 55 and 85 to 100 cycles per output. Then add the remap load inside the row loop and confirm at least 8 cycles per output of gain survive.

### 4.4 RK: run-length kernels, real frames only

Cache per colour a pre-summed flat-chunk kernel and a pair kernel (for pixel-doubled content), and emit them when the sliding window is uniform. Nothing on the random benchmark; 1.3 to 2x on real frames on top of the stack, giving an estimated 3.5 to 6x total on sky- and road-heavy frames. Exact by associativity. Requires AO's accumulators.

Kill experiment, two hours: capture 10 to 20 in-game 16-bit input frames from several stages and count uniform 6-pixel chunks, 17-pixel same-colour supports, and identical even/odd pixel pairs at the blitter's alignment. Go if flat chunks are 30 percent or more, or uniform pairs 80 percent or more (in which case build the pair kernel first).

### 4.5 Expected stack on the benchmark

| Stack | 256-colour, one thread | Full frame ms (est.) |
|---|---|---|
| scalar today | 1.0x | 22.2 |
| AO | 1.7 to 2.5x | 9 to 13 |
| AO + RI | 2.0 to 2.9x | 8 to 11 |
| AO + RI + CT | 2.2 to 3.1x | 7 to 10 |
| plus RK on real frames | 3.5 to 6x | |

Two threads take the half-frame figure to roughly 4 to 5 ms, inside the 8 ms budget with margin.

## 5. Why multi-row (rows in lanes) is not shortlisted

The lanes = rows idea (three rows r, r+1, r+2 with phases 0, 1, 2 in three lanes, table re-laid so the three phases of a tap are adjacent) was analysed in full in `IDEAS_B.md` section 6 and audited in `REVIEW.md`. It has the same vector-op count as AO, but its loads are only vector-wide when the three rows hold the same colour at the same column. On any detailed row (road texture changes every line) it degenerates to 12 single-lane loads per output, the scalar count, and those serialise on their destination register on an in-order core. It needs a 12-byte tap stride (or a 42 MiB padded table), and SSE2 has no lane insert. The vertical locality it buys is obtained for free by RI with no table change. It only becomes attractive on RISC-V, which has indexed vector loads. On this core, rows are a cache-locality axis, not a lane axis.

## 6. Open questions that change the plan

1. Can the renderer's palette conversion pass (`rgb_blargg[]` lookup) emit CT slot indices directly instead of 16-bit colour values? This removes CT's remap load and its 16 KB of L1 pressure and is now the biggest single simplification available.
2. Can 16-bit input frames plus phase be dumped from the game for 10 to 20 frames? Needed for RK's go/no-go, for realistic benchmark input (the current `--pattern mixed` is a weak proxy), and for the bit-exact harness. Are any hires layers pixel-doubled (even/odd pairs identical)? A yes makes the pair kernel a near-unconditional 1.5 to 1.8x on real frames.
3. Is shrinking the table from 32 MiB to about 1.3 MB (lazy generation in CT) wanted for its own sake? With about 100 MB free it is a footprint decision, not a speed one.
4. Wide-screen input width (808) for the unroll remainder, and confirmation that writes one pixel past the row end are tolerable on the last row of a half (the reviewer's preferred AO variant needs none).
5. The bottom-half render thread is currently seeded with the same phase as the top half rather than `(phase + 224) mod 3`; the new kernel's harness should compare like with like, and the game may want the continuous phase.

## 7. Recommended next steps

1. Add a captured-frame input mode to `ntsc_bench` (`--input frame.bin`) and a dump hook in the renderer, so every experiment below runs on real content as well as synthetic.
2. Run the four kill experiments in the order AO, RI, CT, RK (about three days in total). Each is independent of the others.
3. Decide on open questions 1 and 2, which change the CT and RK designs.
4. Only then implement AO, with the existing `_fast` checksums as the bit-exactness gate.

## 8. Answers from the graphics audit and frame capture (2026-09-03)

### 8.1 Open question 1: slot indices (answered by GRAPHICS_SETUP.md)

The emulator already produces slot indices. The `pixels` buffer handed to the renderer holds
`palette_index (0..4095) | 0x1000 for shadow`, i.e. a value in 0..8191, composed by the tile,
text, road and sprite layers from palette RAM indices (only hwsprites sets the shadow flag;
there is no highlight variant in this title, hence 64k rather than 96k colours). The per-frame
`rgb_blargg[pixels[i]]` loop (rendersurface.cpp ~984-993) exists only to re-encode that slot
into a 16-bit colour so that the filter's direct `table + n*512` addressing can find it.

Consequences for CT:
- The kernel table becomes `table[8192]` indexed by the slot (4 MiB at 512 B per entry, or
  1.3 MB per phase plane if phase-major). No remap load, no 64K map, no per-frame conversion
  pass at all: the blitter reads `pixels` directly. The 32 MiB table disappears.
- Regeneration hooks into `RenderBase::convert_palette()` (renderbase.cpp ~95-113), which is
  already called once per palette RAM write and already touches exactly the two affected rows
  (normal and shadow). Worst observed per-frame volume from game logic is 128 rows (sky-cycle
  easter egg, every other frame) or 96 rows (end-of-stage fade, every frame); zero during
  normal driving; larger one-off bursts at stage load.
- The one design point: palette writes come from the game-logic thread while the render
  threads read the table, unlocked, today. A kernel regeneration per write widens that window,
  so writes should be queued as dirty slots and regenerated at the frame boundary before the
  render threads start (a few microseconds per row, well under a millisecond per frame even
  in the worst case).
- CT therefore moves from "footprint and cliff insurance" to a near-free structural change
  that also deletes the conversion pass, and its build order relative to AO can be reversed:
  do CT first, then AO on top of the slot-indexed table.

The audit also settled the pixel-doubling question structurally: tiles, text and the road
background are always written as identical 2x2 blocks in hires mode; only the road foreground
(true 640-wide horizontally, interpolated vertically) and sprites can differ between even and
odd columns. Frame statistics below quantify it.

### 8.2 Open question 2: real frames (answered by the capture)

Facility: the game now accepts `-dump <dir> [interval] [count]` (main.cpp / main.hpp /
rendersurface.cpp, uncommitted). Every interval-th rendered frame it writes the emulator's
slot buffer, the rgb_blargg table and the converted filter input plus the burst phase, then
quits. Tools: `tools/ntsc_bench/frame_stats.py` (validates and prints statistics) and
`frame_to_png.py` (PNG previews). A 150-frame capture (frames 61..9150, interval 61 so all
three phases occur, `-perftest -t 1` under WSL) lives in WSL at
/home/pearc/cannonball-se-dump/frames_long; 24 selected frames with PNGs are in
`tools/ntsc_bench/frames/`.

Content: every frame is the attract-mode AI drive with the title overlay (PUSH START, FREE
PLAY, copyright text, the Ferrari), with the backdrop cycling through cruising, traffic, a
crash, a road fork, a logo flyby and a snowy stage. Road, sky, scenery, traffic and sprites
are all present, so the statistics are close to gameplay; the in-game HUD is absent. To get
player-driven frames, run the WSL binary with `-dump` and press Start (freeplay is on).

Statistics over the 150 frames (frame_stats.py):

| Metric | Average | Range |
|---|---|---|
| Distinct colour values per frame | 100 | 65 to 144 |
| Distinct slot indices per frame | 181 | |
| 6-pixel chunks uniform at the blitter alignment | 65% | 47% to 93% |
| Identical even/odd pixel pairs | 91% | 85% to 98% |
| Pixel equal to the pixel directly above | 81% | |
| Shadow pixels | 1% | |

Implications for the shortlist:

1. Working set on real frames is about 100 colours x 512 B = 50 KB with the current table,
   or about 100 x 168 B = 17 KB per row with a phase-major slot table: L1-resident. The
   256-colour benchmark is pessimistic. Reading the sweep at N = 100 random gives scalar at
   roughly 20 ms today; the N = 64 contiguous point (13.3 ms) suggests the slot-indexed CT
   table alone is worth about 1.5x on real frames before any SIMD work.
2. Pair kernel (RK): 91% of even/odd pairs identical means the pair kernel applies almost
   everywhere. The reviewer's 1.5 to 1.8x for it on real frames is now near-unconditional.
3. Flat-chunk kernel (RK): 65% of 6-pixel chunks are uniform, so most of the kernel work
   can be replaced by a cached pre-summed kernel once the transient has passed. The shipped
   `_fast` already exploits this at 24-pixel granularity (19.8 ms random vs 12.2 ms mixed
   pattern); 6-pixel granularity should capture more.
4. Row interleave (RI): 81% vertical repetition means the interleave will find genuine L1
   reuse on real frames, unlike on the random benchmark.
5. The benchmark needs an `--input frame.bin` mode so that every kill experiment runs on
   these frames as well as on synthetic input. That is the next harness step.

Revised build order for real-frame performance: CT (slot-indexed table, deletes the
conversion pass) -> AO -> RK with the pair kernel first -> RI. The reviewer's 3.5 to 6x
estimate for the full stack on flat-heavy frames was made before these statistics and now
looks conservative on the RK share; it has not been re-estimated here.
