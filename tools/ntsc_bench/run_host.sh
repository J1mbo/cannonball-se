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
FILES="snes_ntsc.cpp snes_ntsc.h snes_ntsc_config.h snes_ntsc_impl.h"

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

need_build=0
[ -x "$BIN" ] || need_build=1
for f in $FILES ntsc_bench.cpp; do
    [ "$STAGE_DIR/$f" -nt "$BIN" ] 2>/dev/null && need_build=1
done
if [ "$need_build" = "1" ]; then
    echo "building $BIN ..."
    g++ -O3 -march=native -std=c++17 -DBENCH_FN="$FN" -DBENCH_REV="$REV" $SCALAR_FLAG \
        "$STAGE_DIR/ntsc_bench.cpp" "$STAGE_DIR/snes_ntsc.cpp" -o "$BIN" -lpthread
else
    echo "$BIN up to date, skipping build"
fi

"$BIN" "$@"
