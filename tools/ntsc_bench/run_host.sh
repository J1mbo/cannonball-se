#!/usr/bin/env bash
# Stage, build and run ntsc_bench locally (WSL, x86 host) as a cross-check
# against the ARM (run_pi.sh) results: same source, same input, so the
# checksum must match (SSE and NEON are supposed to be bit-identical).
#
# Usage (run from WSL, from the repo root):
#   tools/ntsc_bench/run_host.sh <rev|WT> <function> [bench args...]
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <rev|WT> <function> [bench args...]" >&2
    exit 1
fi

if ! command -v g++ >/dev/null 2>&1; then
    echo "No g++ found in this WSL environment - skipping host run." >&2
    exit 0
fi

REV="$1"; FN="$2"; shift 2

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE_DIR="$REPO_ROOT/tools/ntsc_bench/stage/host-$REV"
SRC_DIR="$REPO_ROOT/src/main/sdl2"
BIN="$STAGE_DIR/bench_$FN"
FILES="snes_ntsc.cpp snes_ntsc.h snes_ntsc_config.h snes_ntsc_impl.h snes_ntsc_simd.h"

# <function>=..._vec_scalar is not a real symbol: it means "build the Stage 2 vector
# kernel (snes_ntsc_blit_hires_vec) with its portable v4u32 layer forced to the
# plain-C fallback" (SNES_NTSC_FORCE_SCALAR_SIMD - see snes_ntsc_simd.h), so the
# fallback backend can be exercised and checksummed on a machine that also has real
# SIMD. The binary name keeps the _vec_scalar suffix so it doesn't collide with the
# plain vec build's cache.
FORCE_SCALAR_FLAG=""
case "$FN" in
    *_vec_scalar) FORCE_SCALAR_FLAG="-DSNES_NTSC_FORCE_SCALAR_SIMD"; FN="${FN%_vec_scalar}_vec" ;;
esac

# See run_pi.sh for why timestamps are handled this way (keeps the
# "rebuild only if stale" check from re-triggering on every invocation).
mkdir -p "$STAGE_DIR"
if [ "$REV" = "WT" ]; then
    for f in $FILES; do cp -p "$SRC_DIR/$f" "$STAGE_DIR/$f"; done
else
    for f in $FILES; do
        git -C "$REPO_ROOT" show "$REV:src/main/sdl2/$f" > "$STAGE_DIR/$f"
        touch -d @0 "$STAGE_DIR/$f"
    done
fi
cp -p "$REPO_ROOT/tools/ntsc_bench/ntsc_bench.cpp" "$STAGE_DIR/ntsc_bench.cpp"

SCALAR_FLAG=""
[ "$FN" = "snes_ntsc_blit_hires" ] && SCALAR_FLAG="-DBENCH_SCALAR"

# Auto-detect the slot-indexed API (Part A of the NTSC filter re-architecture): if the
# staged snes_ntsc.h declares snes_ntsc_update_slot, build ntsc_bench.cpp's
# -DBENCH_SLOT_API branch to match; older revisions (e.g. 086591c) don't have the symbol,
# so they build against the classic colour-indexed table unchanged.
SLOT_API_FLAG=""
grep -q "snes_ntsc_update_slot" "$STAGE_DIR/snes_ntsc.h" 2>/dev/null && SLOT_API_FLAG="-DBENCH_SLOT_API"

need_build=0
[ -x "$BIN" ] || need_build=1
for f in $FILES ntsc_bench.cpp; do
    [ "$STAGE_DIR/$f" -nt "$BIN" ] 2>/dev/null && need_build=1
done
# BENCH_CXXFLAGS: extra compiler flags appended after -march=native, e.g.
#   BENCH_CXXFLAGS=-march=x86-64 tools/ntsc_bench/run_host.sh WT snes_ntsc_blit_hires_vec
# lets a later, more specific -march override -march=native (last flag wins with GCC/
# Clang) so the vec kernel's SSE2-only v4u32 layer can be built and checksummed on a
# strictly-SSE2 target line even though the host itself is SSE4-capable. Also forces a
# rebuild whenever set/changed, since it isn't reflected in any staged file's mtime.
[ -n "${BENCH_CXXFLAGS:-}" ] && need_build=1
if [ "$need_build" = "1" ]; then
    echo "building $BIN ..."
    g++ -O3 -march=native -std=c++17 ${BENCH_CXXFLAGS:-} \
        -DBENCH_FN="$FN" -DBENCH_REV="$REV" $SCALAR_FLAG $SLOT_API_FLAG $FORCE_SCALAR_FLAG \
        "$STAGE_DIR/ntsc_bench.cpp" "$STAGE_DIR/snes_ntsc.cpp" -o "$BIN" -lpthread
else
    echo "$BIN up to date, skipping build"
fi

"$BIN" "$@"
