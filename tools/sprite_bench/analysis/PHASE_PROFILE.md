# Where the prepare-thread frame actually goes: measured phase profile

Date: 2026-09-04. Machine `sl-d303`, Raspberry Pi Zero 2 W, 4x Cortex-A53 at
1.0 GHz, governor `performance`, `vcgencmd get_throttled` = 0x0 before and after
every run, so wall-clock is trustworthy. Hi-res 640x448, widescreen off, Blargg
NTSC filter on, full CRT shader (`shader_mode 2`), audio on, attract mode,
headless via KMSDRM. Instrumentation: eight clock reads per frame around the
individual calls in `Video::prepare_frame()`.

## The split at 60 fps, four threads (real operation, 0% frames dropped)

| call | ms/frame | share of prepare | share of 16.7 ms |
|---|---|---|---|
| `update_tile_values` | 0.001 | 0.0% | 0.0% |
| road background | 0.68 | 20% | 4.1% |
| tile background | 0.43 | 13% | 2.6% |
| tile foreground | 0.20 | 6% | 1.2% |
| **road foreground** | **0.88** | **26%** | **5.3%** |
| sprites | 0.93 | 27% | 5.6% |
| text | 0.31 | 9% | 1.9% |
| **total** | **3.44** | | **20.6%** |

Road and tiles sums to 2.19 ms, which matches the 2.20 ms bundled figure
measured earlier, so the split is consistent.

## Road is the target, not tiles

| layer group | ms/frame | share of the 2.19 ms |
|---|---|---|
| **road (background + foreground)** | **1.56** | **71%** |
| tiles (background + foreground + values) | 0.63 | 29% |

The road layer alone costs **1.7 times the sprite pass**. Tiles, which the static
analysis flagged for issuing four separate 16-bit stores per source nibble in
hi-res, are only 0.63 ms in total: even halving them returns 0.3 ms. Road is
where the time is.

`update_tile_values` costs 1 microsecond per frame, so the hoisting opportunity
identified in it is worthless. Ignore it.

## Contention hits the streaming fill hardest

Comparing four-thread operation against single-thread, both at `-60`:

| call | 1 thread | 4 threads + GPU | inflation |
|---|---|---|---|
| **road background** | **0.265** | **0.682** | **+157%** |
| tile background | 0.34 | 0.43 | +27% |
| tile foreground | 0.182 | 0.203 | +11% |
| road foreground | 0.70 | 0.88 | +26% |
| sprites | 0.72 | 0.93 | +29% |

Everything else sits in a +11% to +29% band. **The road background alone
inflates by a factor of 2.6.** That is the phase the road analysis describes as
a `std::fill_n` of a repeated word, a pure streaming write with no per-pixel
decisions. It is the one phase whose cost is set by memory bandwidth rather than
by instruction issue, and it is precisely the one that collapses when three other
consumers are on the bus.

This is the clearest evidence yet for the hypothesis that system memory or the
shared L2 is a limiting factor at 60 fps. It is not that everything is
bandwidth-bound; it is that the one bandwidth-bound phase pays a 2.6x penalty
while the compute-bound phases pay under 30%.

**Caveat.** At `-t 1` the game runs at 26 fps with 50% of frames dropped, so it
is at a different point in the attract sequence and the amount of visible sky,
which is what the road background fills, may differ. The other four phases were
measured across the same pair of runs and moved only 11-29%, which makes a pure
content explanation for a 157% move unlikely, but a same-scene comparison would
settle it.

Under maximum load (`-perftest`, 81 fps, everything flat out) the picture
sharpens further: road foreground rises to 1.33-1.42 ms and tile background to
0.64-0.82 ms, against 0.88 and 0.43 at a paced 60 fps.

## What this implies

**1. The road background is overdraw, and that is the cheapest ms on the table.**
It writes a full-width fill that the tile layers, the road foreground, the
sprites and the text then partly overwrite. Since its cost is bandwidth, not
logic, no amount of loop tuning will help; the only lever is writing fewer
pixels. Composing the layers so the fill covers only pixels that survive, or
merging it into the pass that draws over it, would recover most of 0.68 ms. That
is the band-composition idea from the earlier design work, and it now has a
measured justification rather than a speculative one.

**2. The road foreground, at 0.88 ms, is the largest single compute item.** The
static analysis puts it at about 2.5 memory operations per pixel: two texture
loads, a priority-table load, a colour-table load and one 32-bit store per pixel
pair, giving a load/store floor near 0.72 ms. Measured 0.88 ms against a 0.72 ms
floor means it is already close to its memory-op limit, so the win there must
come from removing memory operations, not from better arithmetic. The candidates
are the 256 KB texture table that is eight times L1D, and the priority table
lookup.

**3. Sprites remain worth doing but are no longer the obvious first target.** At
0.93 ms they are comparable to the road foreground, and the measured 2x kernel
improvement is worth about 0.47 ms. Road as a whole offers more.

**4. Ranked by absolute time recoverable**, best current estimates:

| target | cost now | plausible saving | basis |
|---|---|---|---|
| road background overdraw | 0.68 ms | up to ~0.5 ms | eliminate writes that are overwritten |
| sprite kernel rewrite | 0.93 ms | ~0.47 ms | measured 11.6 to 5.7 cycles/pixel |
| road foreground | 0.88 ms | ~0.15 ms | already near its memory-op floor |
| tile layers | 0.63 ms | ~0.3 ms | wider stores, fewer branches |

## Reproducing

```
# build (Pi 3 A+ needs -j1; -j4 exhausts its 512 MB)
rsync -a --delete --exclude build --exclude .git --exclude tools <repo>/ pi3mule:~/cb-instr/
ssh pi3mule 'cd ~/cb-instr && cmake --build build -j1'
rsync -a pi3mule:~/cb-instr/build/cannonball-se sl-d303:~/cbrun/
ssh sl-d303 'cd ~/cbrun && ./cannonball-se -60 -t 4'   # also -t 1, -perftest
```
