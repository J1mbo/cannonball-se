# Merge plan: `ntsc-rearch` and `sprite-a53` back to `master`

Planning only. No merge is performed by this document. Written 2026-09-05.

## 1. Topology

Both feature branches diverge from the **same** master commit `086591c`, and neither
has been merged into the other:

```
086591c (master, origin/master)
   |
   +-- ntsc-rearch   (11 commits)  NTSC composite filter rework
   +-- sprite-a53     (9 commits)  road/tile renderer optimisation + F7 fix
```

`master` has not moved since either branched, so both are simple three-way merges
against an unchanged base. There is no rebase/replay complication between them.

## 2. Conflict surface

The renderer cores are **disjoint**:

| Branch | Owns (no overlap) |
|---|---|
| ntsc-rearch | `sdl2/snes_ntsc.{cpp,h}`, `sdl2/snes_ntsc_simd.h`, `sdl2/rendersurface.{cpp,hpp}`, `sdl2/renderbase.{cpp,hpp}`, `main.hpp` |
| sprite-a53 | `hwvideo/hwroad.{cpp,hpp}`, `hwvideo/hwtiles.{cpp,hpp}`, `hwvideo/hwarch.hpp`, `sdl2/input.cpp` |

Only **four hub files** are touched by both, and mostly in different regions:

| File | ntsc-rearch region | sprite-a53 region | Conflict risk |
|---|---|---|---|
| `video.hpp` | +2 member lines (~L50) | +30 member lines (~L39) | **Low-medium** — two pure additions into the same `public:` block; adjacent, may flag but trivial to resolve (keep both) |
| `video.cpp` | `render_frame` (~L210) | `prepare_frame` (~L191) + includes | **Low** — different functions |
| `main.cpp` | includes, globals, `main_loop` (~L572-621), `parse_command_line` (-dump), `main` | `main_loop` FPS/report block (~L633) | **Medium** — both edit `main_loop`, in different sub-regions; the highest-risk file |
| `CMakeLists.txt` | +`snes_ntsc_simd.h` to `src_sdl` (~L140) | +`hwarch.hpp` to `src_hwvideo` (~L114) | **Low** — different list sections |

Expected outcome: mostly auto-merge; a handful of "keep both additions" resolutions
in `video.hpp` and `main.cpp`. No logic conflicts, because the two features do not
touch the same code paths.

## 3. What is production vs scaffolding

The important structural fact: **scaffolding is interleaved inside production commits**,
not isolated in its own commits. For example the sprite-a53 `S0` commit adds both the
production `hwarch.hpp` ISA macros and the test-only `_ref` twins; the `S3+S5` commit
adds both the production overwrite mask and the debug `check_foreground_rows`. So the
"without scaffolding" variant **cannot** be produced by dropping whole commits — it
requires a surgical cleanup pass. The compile-time guards make that mechanical.

### sprite-a53

| Item | Files | Kind | Guard / cost |
|---|---|---|---|
| F7 hi-res-sprites hotkey fix | `input.cpp` | **production (bug fix)** | always |
| S1 tile visible-window scan | `hwtiles.cpp` | **production** | always |
| S6 run-length road foreground | `hwroad.{cpp,hpp}` | **production** | always |
| S3 overwrite mask + S5 tile clip | `hwroad.*`, `hwtiles.*`, `video.*` | **production** | always |
| S7 NEON tile-row kernel | `hwtiles.cpp` | **production** | `CB_HAVE_NEON`, scalar fallback |
| `hwarch.hpp` ISA macros | `hwarch.hpp` | **production** (needed by S7) | always |
| `cb_dczva_probe`/`cb_zva_lines` | `hwarch.hpp` | **dead** (DC ZVA killed) | never called — **strip regardless of variant** |
| Per-layer phase timing (`prof_ns_*`, 8 `clk::now`/frame) | `video.{cpp,hpp}`, `main.cpp` | **measurement** | **unguarded — always compiled**; small but real |
| `check_foreground_rows` + `CB_RENDER_CHECKS` assert | `hwroad.*`, `video.cpp` | scaffolding | body compiled unconditionally, called only under `CB_RENDER_CHECKS` |
| `_ref` twins + `RoadTilesTestAccess` | `hwroad.*`, `hwtiles.*` | scaffolding | `CB_RENDER_TEST` (off in game) |
| `CB_FRAME_CKSUM` frame checksum | `video.*`, `main.cpp` | scaffolding | `CB_FRAME_CKSUM` (off in game) |
| Standalone driver | `tools/road_tiles/test/` (tracked) | scaffolding | separate build |
| Design docs | `tools/road_tiles/analysis/`, `tools/sprite_bench/` | docs | **untracked — not committed yet** |

### ntsc-rearch

| Item | Files | Kind |
|---|---|---|
| NTSC filter rework (slot table, vector kernel, run ladder) | `snes_ntsc.{cpp,h}`, `snes_ntsc_simd.h`, `rendersurface.*`, `renderbase.*` | **production** |
| SSE4 ROTATE_OUT lane-order fix | `snes_ntsc.cpp` | **production (bug fix in shipped code)** |
| Bottom-half phase-discontinuity fix | `rendersurface.cpp` | **production (bug fix)** |
| Review fixes (MSVC restrict, aligned table, width guard, safe re-init) | various sdl2 | **production hardening** |
| `flush_ntsc_palette` | `video.cpp`, `main.cpp` | **production** |
| `-dump` frame capture | `main.{cpp,hpp}`, `rendersurface.cpp` | dev flag — **unguarded runtime option** (14 refs); ship or strip is a choice |
| Windows preset + `.gitignore` | `CMakePresets.json`, build cfg | keep |
| ntsc_bench harness + analysis + scalar_ref | `tools/ntsc_bench/` (tracked) | scaffolding/docs |
| Reviewer's guide | `tools/ntsc_bench/analysis/` | docs |

## 4. Two merge variants

### Variant A — merge WITH scaffolding

Merge both branches essentially as they are. Every guard is off in shipping builds, so
the only actual runtime cost carried is the **unguarded phase timing** (and `-dump`,
which is inert unless the flag is passed).

- Pros: full reproducibility travels with the code; the checksum gate, `_ref` twins,
  driver and benches remain available for any future regression run; smallest merge
  effort; history preserved.
- Cons: `tools/ntsc_bench/` and `tools/road_tiles/test/` live in the tree permanently;
  guarded-but-present test code in the renderers; the phase-timing overhead ships unless
  guarded; the dead `cb_dczva` helpers remain unless stripped.
- Minimum hygiene even in this variant: **strip the dead `cb_dczva` helpers**, and
  **guard the phase timing** behind a `-DPROFILE_LAYERS` option (default off) so it is
  not an unconditional cost.

### Variant B — merge WITHOUT scaffolding

Produce a clean production tree, scaffolding removed. Because scaffolding is interleaved,
this is a **cleanup pass**, not commit selection. Remove:

- all `#ifdef CB_FRAME_CKSUM`, `#ifdef CB_RENDER_CHECKS`, `#ifdef CB_RENDER_TEST` blocks
  and the `_ref` twins, `RoadTilesTestAccess`, `check_foreground_rows`, `foreground_row_mask`'s
  debug-only consumer (keep the mask itself — S5 needs it);
- the `CB_FRAME_CKSUM`/`CB_MAX_FRAMES` code in `main.cpp` and `video.*`;
- the phase-timing `prof_ns_*` and the `clk::now` reads (or keep behind a build option);
- the dead `cb_dczva` helpers;
- `tools/road_tiles/test/`, `tools/ntsc_bench/`, and the analysis dirs (or move them to a
  long-lived `dev-tools` branch);
- decide `-dump`: keep as a documented dev flag or remove.

- Pros: lean production diff; no dead code; no measurement overhead; reviewer sees only
  shipping logic.
- Cons: loses in-tree reproducibility; re-verifying later means re-creating the harness;
  the cleanup edits are hand-work that must themselves be checksum-gated so they change
  nothing observable.

### Recommended shape: A for master, B's intent via guards

Keep master on **Variant A but fully guarded** — i.e. do the two hygiene fixes (strip
dead `cb_dczva`, put phase timing behind `-DPROFILE_LAYERS=OFF`), leave the rest of the
scaffolding compile-guarded and the benches in `tools/`. This ships a clean binary (zero
scaffolding cost) while keeping every test asset one build-flag away. If a pristine
production tree is a hard requirement (e.g. for an upstream PR), cut Variant B as a
separate squashed branch from the merged result, gated by the same checksums.

## 5. Recommended order and mechanics

1. **Merge `ntsc-rearch` first.** It is the more mature branch (review-fix commits, a
   reviewer's guide, more iteration) and its `-dump` facility predates and is used by the
   sprite study. Merging it first means its hub-file changes land on master cleanly (no
   conflicts, since master is untouched).
2. **Then merge `sprite-a53`.** This is where the four hub-file conflicts surface, now
   resolved against a master that already carries the NTSC hub edits. All are
   "keep both additions".
3. Use real merge commits (`--no-ff`) so each feature is a reviewable unit and revertible,
   rather than squashing away the staged history that documents the verification.
4. Before either merge, **commit the untracked design docs** (sprite-a53's
   `tools/road_tiles/analysis/` and `tools/sprite_bench/` are currently uncommitted) or
   deliberately exclude them — otherwise they are lost.

## 6. Verification gates (both variants)

- The NTSC work has its own bit-exactness references (FNV checksums in
  `tools/ntsc_bench`, real-frame + synthetic). Re-run after the merge.
- The road/tile work has the four golden frame checksums
  (`tools/road_tiles/analysis/GOLDEN_CHECKSUMS.md`) and the driver unit suite. Re-run
  after the merge, and **again after any Variant B cleanup edit** — the cleanup must not
  change a single checksum.
- Build matrix to green before calling it done: Linux AArch64 (NEON), Linux x86-64,
  Windows MSVC (x64), and a non-NEON ARM (scalar fallback). Both features have scalar
  fallbacks that only the last two exercise.
- Because the two features are disjoint, a merged build should reproduce **both** sets of
  reference checksums unchanged; any drift means a hub-file merge resolved wrong.

## 7. Decisions for the maintainer

1. Master carries scaffolding (Variant A, guarded) or stays pristine (Variant B)?
2. Phase timing: strip, or keep behind `-DPROFILE_LAYERS`?
3. `-dump`: ship as a documented dev flag, or remove?
4. `tools/ntsc_bench` and `tools/road_tiles/test`: keep in-tree, or relocate to a
   `dev-tools` branch?
5. Commit the untracked analysis docs, or drop them?
6. Merge commits vs squash for the public history?

Items to fix regardless of variant: strip the dead `cb_dczva` helpers; confirm the
`PIXEL_ACCURACY`/glow decision from the sprite study is settled before it ships (separate
from these branches, noted here so it is not forgotten).
