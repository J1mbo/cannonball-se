# Sprite scaler accumulator: closed-form schedule verification

This is a brute-force check of whether the OutRun/X-Board hardware sprite
scaler's two accumulator loops (`hwsprites::render()`, upstream MAME-derived
source) can be replaced by closed-form index functions. The program that
performs the check lives at
`C:/Users/pearc/AppData/Local/Temp/claude/c--VS-Code-Work-cannonball-se/7bc9e1e7-00cf-4bb5-a117-22274489fbf2/scratchpad/verify_schedule.cpp`
(build: `g++ -O2 -std=c++17 -o verify_schedule verify_schedule.cpp`; run
against `src/main/engine/data/ozoom_lookup.hpp`); raw output is saved
alongside it as `verify_output.txt`.

No file under `src/` was modified. The two loops modeled are:

* **Horizontal** (source-major, `hwsprites::render()` lines ~273-290):
  `while (xacc < 0x200) { emit; xacc += zoom; } xacc -= 0x200;` per source
  pixel, `xacc` persisting across the row.
* **Vertical** (destination-major, lines ~318-321): `yacc += zoom; addr +=
  pitch*(yacc>>9); yacc &= 0x1ff;` per destination row.

The verifier parses `ZOOM_LOOKUP` and `ZOOM_LOOKUP_HIRES` out of
`ozoom_lookup.hpp` at run time (strips comments, pulls every other `0x...`
hex token, since each table row is `zoom, mask, SIZEn, UNUSED/SIZEm` and only
the first two fields are hex literals) rather than retyping the tables. It
found 128 distinct values in `ZOOM_LOOKUP` (`0x100`-`0x3F0`), 159 in
`ZOOM_LOOKUP_HIRES` (`0x100`-`0x7E0`); the hi-res-default set (`ZOOM_LOOKUP`
halved) is 128 distinct values over `0x80`-`0x1F8`, and the lo-res set is the
same 128 raw values over `0x100`-`0x3F0`. All of these, and their halves,
fall inside `[1, 0x7FF]`, so the exhaustive sweep over every integer
`zoom = 1..0x7FF` (2047 values) is a strict superset of "every zoom that
actually occurs" and was used as the single test for C1-C5, simulated with
8448 source pixels and 2048 destination rows per zoom (8x/2x the requested
minimum of 1024).

## Verdicts

**C1 — TRUE.** `N(i) == ceil(i*512/zoom)` held for all 17,295,103 samples
checked (every `zoom` in `1..0x7FF`, every `i` in `0..8448`), zero
mismatches. A short inductive proof confirms it: writing `a_i` for `xacc` at
the start of source pixel `i`, `a_i = N(i)*zoom - i*512`, and the loop's exit
condition keeps `0 <= a_i < zoom`; solving that for `N(i)` gives exactly the
ceiling form.

**C2 — TRUE.** `src(d) = max{ i : N(i) <= d } == (d*zoom) >> 9` held for all
4,192,256 samples checked. One caveat surfaced while building the harness:
with `NSRC == NDEST == 2048` a spurious "mismatch" appeared at
`zoom=0x201, d=2046` (found `2048`, expected `2049`). That was a test-rig
bug, not a real one — for `zoom > 0x200` the source index needed to cover a
destination offset can be up to `~4x` that offset (`0x7FF/512 ≈ 4`), so
`NSRC` must be sized `>= NDEST * 0x7FF/512` or the search silently clips at
the array boundary. Widening `NSRC` to `NDEST*4 + 256` removed the mismatch
for the entire sweep.

**C3 — TRUE, and the alternative rounding is confirmed wrong by
simulation.** `Y(j) == (j*zoom) >> 9` held for all 4,194,303 samples. Proof:
`S_m = addr_off_m*512 + yacc_m` satisfies `S_0 = 0` and `S_{m+1} = S_m +
zoom` by construction (`addr += pitch*(yacc>>9); yacc &= 0x1ff` is exactly a
base-512 carry split), so `S_m = m*zoom` exactly and `Y(m) =
floor(m*zoom/512)`. As instructed, the wrong candidate `floor(j*512/zoom)`
was also simulated against real `Y(j)` (lo-res zoom set, `j=1..2048`): it
agreed on only 2048 of 262,144 samples (0.8%), first disagreement at
`zoom=0x100, j=1` (`Y(1)=0` vs `floor(512/256)=2`). The two formulas are
reciprocal-shaped and only coincide at isolated points; `zoom` must be the
*numerator*, not the denominator.

**C4 — TRUE.** C2 and C3 both reduce to the same function `f(n) =
(n*zoom)>>9` of their index, so the horizontal and vertical schedules are
literally the same function. Checked directly too, independent of the
algebraic formula: `Y(j)` from the vertical simulation was compared against
`max{i : N(i)<=d}` from the *separate* horizontal simulation for all
4,192,256 samples, zero disagreements. One schedule table genuinely serves
both axes.

**C5 — TRUE, both sub-ranges.** `r(i) = N(i+1)-N(i)` lies in `{1,2,3,4}` for
every integer `zoom` in `0x80..0x1F8` (3,184,896 samples, all in range) and
in `{0,1}` for every integer `zoom` in `0x201..0x7FF` (12,967,680 samples,
all in range). `zoom == 0x200` (excluded from both ranges) is the degenerate
case `r(i) == 1` always, period 1.

## Run-length distribution (i = 0..255, aggregated over each zoom set)

Hi-res default set (`ZOOM_LOOKUP >> 1`, 128 distinct values, `0x80..0x1F8`):

| run length | count | share |
|---|---|---|
| 1 | 8084 | 24.7% |
| 2 | 12212 | 37.3% |
| 3 | 8149 | 24.9% |
| 4 | 4323 | 13.2% |

Lo-res set (`ZOOM_LOOKUP` raw, 128 distinct values, `0x100..0x3F0`):

| run length | count | share |
|---|---|---|
| 0 | 4015 | 12.3% |
| 1 | 20362 | 62.1% |
| 2 | 8391 | 25.6% |

The lo-res set spans both regimes from C5: its low end (`0x100..0x200`)
behaves like a mild version of the hi-res case (run lengths 1-2); its high
end (`> 0x200`) produces the occasional dropped source pixel (run length 0)
predicted by C5's `{0,1}` bound. No run length reaches 3 or 4 because no
lo-res entry goes below `0x100`, capping the max run length at
`ceil(512/256) = 2`.

## Period tables

Period = smallest `p >= 1` with `r(i+p) == r(i)` for all tested `i`, found
by simulating 16384 source pixels per zoom (confirmed stable to 0 unresolved
periods for the full window, i.e. every zoom in both sets settled well
inside the window) and searching `p` up to 8192.

| set | zoom count | max period | at zoom |
|---|---|---|---|
| Hi-res default (`0x80..0x1F8`) | 128 | 489 | `0x1E9` |
| Lo-res (`0x100..0x3F0`) | 128 | 949 | `0x3B5` |

Both tables show the same pattern: period = `zoom / 2^v`, the odd part of
`zoom` (e.g. `0x082=130=2·65` gives period 65; `0x080=128=2^7` gives period
1; an odd `zoom` gives period `== zoom`, which is why `0x1E9=489` and
`0x3B5=949`, both odd, set the maxima — they're simply the largest odd zoom
values in each set). This follows from the C1 proof: state `a_i` cycles with
period `zoom/gcd(zoom,512)`, and `gcd(zoom,512)` is exactly the power-of-two
part of `zoom` since `512 = 2^9`.

**Caveat on the "tiny table" premise.** A short period would let a per-zoom
schedule be tiny — that does not hold here: periods run up to 489 and 949,
longer than any real sprite (the lookup table's own comments cite widths of
4-94 pixels for the largest in-game sprites), so a table sized to the true
period would not be tiny. What *is* small, and what matters more, is the
**alphabet** of run lengths (`{1,2,3,4}` or `{0,1,2}`, per C5) — and C1-C4
mean no schedule table is needed at all: `src(d) = (d*zoom)>>9` can be
evaluated directly per index without walking an accumulator, with the short
alphabet only bounding how many "emit N copies" cases a fused kernel needs
to special-case.

## Summary

All five claims (C1-C5) are TRUE, verified exhaustively for every integer
zoom `1..0x7FF` (a superset of every zoom value that occurs via
`ZOOM_LOOKUP`/`ZOOM_LOOKUP_HIRES`, raw or halved), with tens of millions of
index comparisons and zero surviving mismatches. The one false lead was the
naive vertical formula `floor(j*512/zoom)`, which C3 asked to be checked
against simulation and which disagrees with the real loop 99%+ of the time
— the correct, *bit-exact* replacement for both accumulator loops is the
single function `f(n) = (n*zoom) >> 9`. The other behavior worth flagging
for an implementer is the run-length-zero case (`zoom > 0x200`): a source
pixel can legitimately contribute no output pixels, which any schedule-based
fast path must handle as a real, non-error case rather than a minimum of
one.
