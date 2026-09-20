# The System 16 "glowy edge" quirk: minimal reproduction in a span renderer

Status: branch `ntsc-rearch`. Settles the `PIXEL_ACCURACY` question
raised in `../README.md` (formerly `PROBLEM_STATEMENT.md` section 11). Reference is the upstream
`PIXEL_ACCURACY` macro (upstream `djyt/cannonball` `hwsprites.cpp` lines 155-170); the
shipped fork carries the same macro at `hwsprites.cpp:182-209` but its unguarded
`#define PIXEL_ACCURACY 0` at line 58 means it has never executed in any build.
Every equivalence claim below was checked by script
(a standalone verification script, not checked in): 30.2 million single-row cases and 200,000 random
two-sprite pairs, zero mismatches where equivalence is claimed, and a concrete
mismatch wherever it is denied.

Notation: `P[x]` is the destination halfword, `x1`/`x2` the clip window, `lo` the
leftmost *visible* output pixel of a span. Types per output pixel: gap (nibble 0
or 15), shadow (nibble 10 with the sprite's shadow flag set), colour (anything
else). All reasoning is in output-pixel space: the reference draws output pixels
one at a time in a fixed order, and magnification only determines the type
sequence, so a proof over arbitrary type sequences covers every zoom.

## 1. Minimal writes: one clear per run, at its left neighbour

Consider a sprite row drawn left to right, and a maximal run of consecutive
output pixels `[a, b]` that it writes as colour. The reference executes, for
`x = a, a+1, ..., b` in that order: `P[x-1] &= 0xFFF` (if `x > x1`), then
`P[x] = pix | color`.

For every `x` in `(a, b]`, the clear targets `P[x-1]`, which was written by the
immediately preceding step of this same loop as `pix | color`. That value is
below `0x1000` (`color <= 0xFF0`, `pix <= 14`), so bit 12 is already zero, and
nothing executes between that write and this clear (the accumulator loop at
lines 278-285 only advances `x` and `xacc`). The clear is therefore a no-op:
same value in, same value out, and no other agent can observe the intermediate
state because the renderer is single-threaded on this buffer. Only the clear at
`x = a` can change anything, and it targets `P[a-1]`, a pixel that is by
definition not colour in this row.

**Rule.** For each maximal, non-empty, visible run of colour output pixels with
leftmost pixel `lo`: if `lo > x1`, clear bit 12 of `P[lo-1]`. Nothing else.

Conditions under which it holds, each load-bearing:

* A colour write always produces a value with bit 12 clear. This is the whole
  argument; if `color` could ever carry bit 12 the reduction fails.
* The pixels of a run are written consecutively with no intervening operation
  on the buffer. True of the reference; an implementation that interleaved two
  rows would still be safe (different addresses) but one that wrote a run in a
  non-sequential order would have to re-derive the argument.
* Values never reach `0x2000`, so the reference's `&= 0xFFF; += 0x1000` shadow
  form equals `|= 0x1000` and the algebra `(v | 0x1000) & 0xFFF == v & 0xFFF`
  holds. Background layers store values below `0x1000` (see `../README.md`, threading model, formerly `PIPELINE.md`;
  the harness asserts it).
* `lo` is the first pixel actually drawn, after clipping. If the run starts left
  of `x1` the first drawn pixel is `x1` itself and the reference's `x > x1`
  guard suppresses the clear; the rule's `lo > x1` says the same thing. The
  guard also prevents `P[-1]`, which in a flat buffer is the last pixel of the
  previous row.

Magnification does not change the rule, it only says what "first" means. A run
of source colour pixels `[i, j]` becomes output pixels `[N(i), N(j+1))`; its
first pixel is the first replica of source pixel `i`, and clears at replicas
2..r and at every replica of `i+1..j` are the no-op case. Two things follow for
the shrink regime (`r = 0` allowed): a colour source pixel with no replica
contributes nothing, so a colour span of zero length between two shadow or gap
spans must be skipped *before* the rule is applied (it is not a run and the
reference draws nothing for it); and two colour source pixels separated by a
zero-replica colour pixel form one output run with one clear.

At the boundary between a shadow run and a colour run of the same sprite row,
drawn left to right, the shadow pixel at `lo-1` is set by the shadow step and
then cleared by the colour run's single surviving clear. Net effect
`P[lo-1] = pre & 0xFFF`. This is not "the shadow run is one pixel shorter":
a shorter shadow run would leave an earlier sprite's bit 12 in place, whereas
the quirk removes it. Under magnification it is the *last output replica* of
the last shadow source pixel that is lost, so the visible boundary shifts by one
output pixel, consistent with the "one pixel clock" explanation. When `lo-1` is
a gap pixel or lies outside the sprite's extent, the clear lands on whatever an
earlier sprite left there, which is the glow proper: a car drawn over its own
shadow sprite loses the shadow one pixel left of every opaque run start.

## 2. Direction: the same clear, but the shadow boundary flips

Drawing right to left, the run `[a, b]` is written `b, b-1, ..., a`. At `x = b`
the clear targets `P[b-1]`, not yet drawn; at `x = b-1` the colour write
overwrites `P[b-1]` in full, killing that clear; and so on down the run. Every
clear except the last one, at `x = a`, is dead because a full write to the same
address follows it. The survivor is again `P[a-1]`, the left neighbour of the
*leftmost* pixel, which in this direction is drawn last. So the rule's
address is direction-independent: mirroring a sprite to left-to-right and
applying the rule in screen space places the clear correctly.

What changes is the order of the clear relative to the shadow step at `a-1`, if
that pixel is a shadow pixel of the same sprite row:

* Left to right: shadow sets bit 12 at `a-1`, then the colour run clears it.
  Final `P[a-1] = pre & 0xFFF`.
* Right to left: the colour run clears `a-1` first, then the shadow step sets
  it. Final `P[a-1] = pre | 0x1000`.

Concrete example, both directions, screen order `gap, shadow, colour, colour`
at `x = 2..5`, background `0x0103` at `x = 3`: the reference leaves
`P[3] = 0x0103` when `xdelta = +1` and `P[3] = 0x1103` when `xdelta = -1`
(script Test 1b). No other configuration is order-sensitive: the only writes a
row makes to `a-1` are a shadow OR (this case), a colour write (impossible,
the run is maximal), or another run's clear (only if it is the same run). The
right-hand boundary is symmetric in both directions because no clear ever
targets the pixel right of a run. Every pairwise interaction was enumerated in
script Test 1 (all type sequences up to length 7, all bit-12 patterns, both
directions, three clip windows): the rule with the correction below matches the
reference in all 30,219,264 cases.

**Verdict.** Mirror normalisation stays valid with one amendment: for a sprite
drawn right to left, suppress the clear when the screen pixel at `lo-1` is a
shadow pixel of the same sprite row. The mirrored decode, the span list and the
replay are otherwise untouched; the direction survives as one bit per sprite
consulted in the span walk, where the previous non-empty span's type is already
at hand. Routing mixed shadow-and-colour right-to-left sprites to the reference
would also be correct, but there is no reason to pay for it.

## 3. Deferral: safe per row or per sprite, wrong per frame

A deferred clear commutes with a later colour write to the same address
(`colour & 0xFFF == colour`), and with the text layer, which is drawn after the
sprites (`video.cpp:201-202`) and stores plain values below `0x1000`
(`hwtiles.cpp:452-455`). It does not commute with a later shadow OR:

* inline: `(pre & 0xFFF) | 0x1000`, bit 12 set;
* deferred: `(pre | 0x1000) & 0xFFF`, bit 12 clear.

Two-sprite counterexample (script Test 2, first failing pair): sprite A, drawn
first, opaque from `x = 7` on some row, so its clear targets `x = 6`. Sprite B,
drawn later with its shadow flag set, draws a shadow pixel at `x = 6` on the
same row. Background `P[6] = 0x0106`. Inline: A clears (no change), B sets,
final `0x1106`. End-of-frame deferral: B sets, then the pass clears, final
`0x0106`. The pixel that should sit under B's shadow is unshadowed. The
configuration is the ordinary one of a nearer object's shadow falling on a
farther object's left edge; 6.7% of random pairs hit it. There is no rescue
short of also deferring the shadow ORs in the same ordered log, which is just
replaying the original sequence, or keeping a per-pixel last-operation mask,
which is the 36-72 KB mask `DESIGN_FABLE.md` section 5 already rejected on
eviction grounds.

Granularities:

* **End of row** and **end of sprite**: safe. A sprite writes each destination
  address at most once except in the two cases already handled (dead clears
  inside a right-to-left run, which the rule never emits, and the shadow
  boundary, where the rule either orders the clear after the OR or suppresses
  it). Rows are disjoint addresses, so any flush point up to the end of the
  sprite, including flushing a partial list whenever it fills, gives the
  reference result. Script Test 2: 0 mismatches in 200,000 pairs for
  end-of-sprite deferral.
* **End of the sprite list / end of frame**: unsafe, as above. With a single
  priority pass (`render(pixels, 8)`) the two are the same point.

Cost of the safe form. Entries are one 32-bit offset (or pointer) per visible
colour span per destination row; the hi-res estimate from `DESIGN_FABLE.md`
(300 K output pixels, ~100 per row, 1.5 colour spans per row) gives ~4,500 per
frame. A 64-entry ring flushed when full or at sprite end is 256 bytes and never
leaves L1, so the array traffic is two L1-hit slots per entry. The pass itself
is `ldr` offset, `ldrh`, `bic #0x1000`, `strh`: four slots, two of them memory,
with the pixel load hitting L1 or L2 because the same row was stored to
microseconds earlier. Total ~6 slots per entry, ~27 K slots per frame, plus at
most an L2 hit per entry on large sprites: **0.03-0.1 ms per frame** on either
core, 2-8% of the ~1.2-1.5 ms renderer estimate.

Does it beat inline? Inline before the run's stores is `ldrh`, `bic`, `strh`
against a line that usually misses: an exposed 135-180 cycle load stall per
span-row, 0.6-0.8 ms per frame if every one is exposed. On the A7 that stall is
partly money already spent, because its 4-entry store buffer would stall on the
same line's fill as soon as the run's stores arrive; on the A53, whose store
buffer merges and drains without stalling, it is a net loss. The deferral moves
the load to a point where the fill has completed, for two extra L1 slots per
entry. So: deferral to end of sprite is the better inline, not an alternative
to it; deferral to end of frame is incorrect and is not costed.

## 4. Recommended implementation

In the span renderer of `DESIGN_FABLE.md` (decode a source row once into
`[lo, hi)` records of type gap/colour/shadow; replay onto the `r_v` destination
rows; colour spans store-only; shadow spans read-modify-write), the quirk is a
property of a colour record, decided once per source row and applied once per
destination row:

1. **Decode** (per source row, per sprite): when emitting a colour record, set a
   `clear_left` flag if the record is non-empty and the preceding non-empty
   record is not a shadow record of a right-to-left sprite. The decode already
   tracks `prev` type for record coalescing; this is one compare and one `orr`
   into the record word per colour span, zero per pixel.
2. **Replay** (per colour span per destination row): after clipping, if
   `clear_left` and `x(lo) > x1`, push `dst + lo - 1` onto the per-sprite ring.
   One compare, one store. The colour copy is unchanged and still never reads
   the destination.
3. **Flush** at sprite end or when the ring fills: the four-slot loop of
   section 3. Spans are walked left to right, so for left-to-right sprites the
   shadow OR at `lo-1` has always been issued before the clear is pushed; the
   flush therefore executes after it, matching the reference order.

Per span-row: ~6-7 issue slots, one probable L1/L2 hit. Per frame: ~4,500
entries, 0.03-0.1 ms. The reference `PIXEL_ACCURACY` path costs a load, an
`and`, a store and a branch on every opaque output pixel, ~1 M extra slots and
up to 300 K exposed misses per frame; that is the cost the project remembered,
and it is roughly 60x the cost of the span form.

Design decisions this does and does not change:

* The colour path stays a pure store stream. The quirk adds no per-pixel read
  and no dependence on the pixel to the left; the constraint asserted in the
  earlier brief does not exist. No vector kernel is affected.
* Mirror normalisation stays; `xdelta` survives as one bit consumed by the
  decode's `clear_left` test.
* The replay's overrun assertion (`DESIGN_FABLE.md` section 6.5) must admit
  `x1 <= x(lo)-1`, one pixel left of the sprite's own extent, while the colour
  copy's end-overlap trick must continue never to touch `lo-1` (it cannot,
  `lo-1` is a gap or shadow pixel, and a leading overlap would be a bug with
  or without the quirk).
* Zero-length colour records from shrink zooms must be dropped before step 1
  or 2, which the replay's `hi <= lo` skip already does if the flag test is
  placed after it.
* Two optional exact filters, each removing work rather than adding it: a
  frame-level `shadow_seen` flag (bit 12 is clear everywhere until the first
  shadow OR, so earlier clears are provable no-ops) and a per-destination-row
  bitmap set by shadow replays and tested per colour span-row (28-56 bytes, one
  `tst` per span). Neither is needed to make the budget; instrument first.
* The ring plus flag fits in the register budgets of both cores as a pointer
  and a count; on the A7 the flush loop is separate code with its own
  allocation, like the replay.

Enabling the quirk should therefore be the default, with the CMake option
restored to mean what it says, and the "glow constrains every vector design"
line in the brief retired.

## 5. Verification plan

Everything below is against the reference with `PIXEL_ACCURACY 1`, which is the
only definition of correct.

1. **Single-row rule.** Keep `glow_check.py` Test 1 as a unit test: every type
   sequence up to length 7, every bit-12 pattern over a 9-pixel row, both
   directions, three start positions, clip windows `(0,W)`, `(1,W)`, `(0,W-2)`.
   Zero mismatches is the pass condition; this proves sections 1 and 2 in
   output space, independent of zoom.
2. **Zoom independence.** A second single-row test in *source* space: random
   nibble rows including 0, 15 and 10 under both shadow modes, every zoom in the
   three shipping tables, both `flip` and both `xdelta`, rendered by the
   reference accumulator loop and by decode-plus-rule. Covers zero-length
   spans, replica boundaries and the terminator, which Test 1 abstracts away.
3. **Ordering.** Test 2 as a unit test: random pairs and triples of rows,
   inline versus end-of-sprite deferral must match; end-of-frame deferral is
   kept in the test as a known-failing control so the suite proves it is
   still detecting the case, not merely passing.
4. **Whole-frame bit-exactness.** The golden-frame harness of
   `DESIGN_FABLE.md` section 6.4 with the reference built at
   `PIXEL_ACCURACY 1`: recorded sprite RAM from real play in lo-res, hi-res and
   hiresprites, backgrounds pre-filled with bit 12 set in patterns, whole-buffer
   `memcmp` per frame. Assert every buffer value is below `0x2000` before and
   after, which is the precondition of the `|=` rewrite.
5. **Fuzzing aimed at the awkward cases.** Records with `xpos` placing `lo` at
   `x1`, `x1+1`, `x2-1` and at 32-pixel line boundaries; wide-screen clip mode
   with `x1 > 0`; right-to-left sprites whose art has nibble 10 immediately
   before colour in source order (so that after mirroring it is immediately
   left of a run on screen); shrink zooms with colour pixels of zero replica
   between shadow pixels; and lists of 2-5 overlapping sprites in both
   shadow-first and colour-first orders. Coverage counters must show non-zero
   counts of clears applied, clears suppressed by the right-to-left rule, and
   clears that changed a value.
6. **Observability in real play.** Count per frame the clears that changed a
   value. If it is zero across recorded play the feature is invisible and the
   whole question is moot; if not, those frames go into the golden set.
7. **Cost attribution.** Per-frame counts of entries, flushes and, via
   `perf_event_open`, L1D refills attributable to the flush loop, with the NTSC
   filter running on the neighbouring cores. The expected result is a delta
   under 0.1 ms; a larger one means the loads are missing and the flush point
   should move earlier (end of row) or gain a `pld` at push time.
