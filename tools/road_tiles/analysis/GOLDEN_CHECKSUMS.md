# Golden frame checksums — the bit-exact gate for every optimisation

Every road/tile/sprite optimisation must reproduce these exactly. They are a rolling
FNV-1a over the finished 16-bit S16 frame buffer (`config.s16_width * s16_height` pixels)
accumulated across a fixed number of frames, computed by the `CB_FRAME_CKSUM` test build
(a compile-time aid, not shipped; see `video.cpp` and `main.cpp`).

Determinism: attract mode under `-perftest -t 1` is frame-locked and single-threaded, so
the same frame count gives the same checksum run to run. Re-verified at the baseline below
with three identical consecutive runs.

Reference: `master` at the "Optional fix for traffic braking on a stale car ahead speed"
commit, x86-64, GCC 15.2, WSL.

## Build and run

```
cmake -S . -B build-cksum -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DCB_FRAME_CKSUM"
cmake --build build-cksum -j$(nproc)
CB_MAX_FRAMES=600 SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  ./build-cksum/cannonball-se -perftest -t 1 -cfgfile /tmp/cfg/<mode>.xml
```

Run it from the repository root so `res/` and `roms/` resolve.

## The four mode config files

Previously unrecorded, which meant the recipe had to be reconstructed on every use. Each
mode is the shipped `res/config.xml` with only `<hires>` and `<widescreen>` altered:

```sh
mkdir -p /tmp/cfg
for m in hires_nw:1:0 lores_nw:0:0 hires_wide:1:1 lores_wide:0:1; do
  IFS=: read n h w <<<"$m"
  sed -e "s#<hires>[01]</hires>#<hires>$h</hires>#" \
      -e "s#<widescreen>[01]</widescreen>#<widescreen>$w</widescreen>#" \
      res/config.xml > /tmp/cfg/$n.xml
done
```

Check the four files actually differ before trusting a result. If `-cfgfile` names a file
that does not exist the binary silently falls back to its defaults, and all four modes then
return the *same* checksum — a failure mode that looks like a plausible result. Note also
that a WSL instance which idles out discards `/tmp`, so config files written there may have
vanished by the next invocation; keep them somewhere persistent for a long session.

## Golden values, 600 frames

| mode | hires | widescreen | checksum |
|---|---|---|---|
| hires_nw (shipping default) | 1 | 0 | `0x05d98764ff87acd5` |
| lores_nw | 0 | 0 | `0x6630b4b099cf1157` |
| hires_wide | 1 | 1 | `0x432cb013834a6d87` |
| lores_wide | 0 | 1 | `0x20674347e186b8e3` |

Longer run, shipping default, 1800 frames: `0x14f2ae11922ea305`.

## Provenance of this baseline

The previous table dated from the commit that introduced the gate and no longer matched the
tree. Rather than overwrite it blind, the drift was bisected. Findings:

- The original four values reproduce **exactly** at `fcf9d05` under GCC 15.2, so the recipe
  was always correct and the toolchain is not a variable. The drift was real code drift.
- The NTSC filter rework, the road/tile NEON work and both branch merges are **bit-exact**:
  `32fb472` and `b991a80` both still reproduce the original values.
- The first commit to move the pixels is **`f045a26` (Sprite S1: LINEBUF core)**. Its
  predecessor `ce38d8c` is clean. The change was intentional — the sprite scheduling rework
  altered output by design — but it shipped described as bit-exact, which it was not.
- Further intended sprite changes moved them again later, notably `bd06b6e` (exact ROM
  sprite sizes) and `3889f79` (hi-res black line and horizontal offset fixed at source).

Every move therefore traces to deliberate sprite work, which is why these values are
blessed as the new baseline rather than recorded as unexplained drift.

## Notes for a future bisect

- `CB_MAX_FRAMES` does **not** exist on the `ntsc-rearch` line — twelve commits between
  `281ad89` and `fc680bb`, plus the merge `d41637e`, have no exit block in `main.cpp`. Those
  commits cannot be gated without cherry-picking the aid in first.
- `dc67ee2` builds and has the aid but never reaches the frame cap under `-perftest -t 1`;
  it spins multi-threaded until killed. Wrap the run in `timeout` or a bisect will wedge.

## Rule

After any change to `hwroad.cpp`, `hwtiles.cpp`, the sprite kernels, or the composition in
`video.cpp`, rebuild the `CB_FRAME_CKSUM` binary and re-run all four modes. Every checksum
must match the table above. A single mismatch means the change is not bit-exact and does not
ship until the difference is understood and either fixed or explicitly accepted by the
maintainer — and if accepted, this table is re-baselined in the same commit, with the reason
recorded in the section above. The per-function `_ref` tests in the implementation spec are
the finer gate; this whole-frame gate is the backstop that catches composition and edge-case
errors the unit tests miss.

Caveat: these cover attract mode only. Before finalising, capture an equivalent baseline
over a recorded gameplay input so that player-driven content (dense traffic, the road
foreground control modes that attract mode may not exercise) is also gated.
