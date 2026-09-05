# Golden frame checksums — the bit-exact gate for every optimisation

Every road/tile/sprite optimisation must reproduce these exactly. They are a rolling
FNV-1a over the finished 16-bit S16 frame buffer (`config.s16_width * s16_height` pixels)
accumulated across a fixed number of frames, computed by the `CB_FRAME_CKSUM` test build
(a compile-time aid, not shipped; see `video.cpp` and `main.cpp`).

Determinism: attract mode under `-perftest -t 1` is frame-locked and single-threaded, so
the same frame count gives the same checksum run to run (verified: three identical runs).

Reference: branch `sprite-a53`, working tree before any renderer optimisation, x86-64
GCC 13.3 in WSL. Captured 2026-09-04.

## Build and run

```
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DCB_FRAME_CKSUM"
cmake --build build -j16
CB_MAX_FRAMES=600 SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  ./build/cannonball-se -perftest -t 1 -cfgfile /tmp/cfg/<mode>.xml
```

## Golden values, 600 frames

| mode | hires | widescreen | checksum |
|---|---|---|---|
| hires_nw (shipping default) | 1 | 0 | `0x2cf51cf81d7c7a3e` |
| lores_nw | 0 | 0 | `0x5d36c3da5664fbc3` |
| hires_wide | 1 | 1 | `0x1b1db120b0903ef9` |
| lores_wide | 0 | 1 | `0x0e379d6377445308` |

Longer run, shipping default, 1800 frames: `0x8061434744eef158`.

## Rule

After any change to `hwroad.cpp`, `hwtiles.cpp` or the composition in `video.cpp`,
rebuild the `CB_FRAME_CKSUM` binary and re-run all four modes. Every checksum must match
the table above. A single mismatch means the change is not bit-exact and does not ship
until the difference is understood and either fixed or explicitly accepted by the
maintainer. The per-function `_ref` tests in the implementation spec are the finer gate;
this whole-frame gate is the backstop that catches composition and edge-case errors the
unit tests miss.

Caveat: these cover attract mode only. Before finalising, capture an equivalent baseline
over a recorded gameplay input so that player-driven content (dense traffic, the road
foreground control modes that attract mode may not exercise) is also gated.
