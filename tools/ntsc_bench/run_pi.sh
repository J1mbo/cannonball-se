#!/usr/bin/env bash
# Stage, ship, build and run ntsc_bench on the ARM test box (sl-d303).
#
# Usage (run from WSL, from the repo root):
#   tools/ntsc_bench/run_pi.sh <rev|WT> <function> [bench args...]
#
# <rev>      a git commit-ish (e.g. 086591c), or WT for the working tree.
# <function> the blitter under test, e.g. snes_ntsc_blit_hires_fast,
#            snes_ntsc_blit_hires_fast2, or snes_ntsc_blit_hires (scalar;
#            -DBENCH_SCALAR is added automatically for this one).
#
# Example:
#   tools/ntsc_bench/run_pi.sh WT snes_ntsc_blit_hires_fast2 --colours 65536 --threads 2
#
# --input handling: when the bench args include --input, the captured frames
# in tools/ntsc_bench/frames/ are rsync'd once to sl-d303:~/ntsc_bench/frames/
# (shared across revisions, not copied into every per-REV build dir), and a
# --input value naming a local path under tools/ntsc_bench/frames (relative
# to the repo root, or absolute) is rewritten to the equivalent path on the
# Pi before the run. E.g.:
#   tools/ntsc_bench/run_pi.sh 086591c snes_ntsc_blit_hires_fast --input tools/ntsc_bench/frames --seconds 5
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <rev|WT> <function> [bench args...]" >&2
    exit 1
fi

REV="$1"; FN="$2"; shift 2

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE_DIR="$REPO_ROOT/tools/ntsc_bench/stage/$REV"
SRC_DIR="$REPO_ROOT/src/main/sdl2"
PI_HOST="sl-d303"
PI_DIR="ntsc_bench/$REV"
BIN="bench_$FN"
FILES="snes_ntsc.cpp snes_ntsc.h snes_ntsc_config.h snes_ntsc_impl.h"

# ---- 1. stage the four filter files + the bench source --------------------
# Timestamps matter here: the Pi-side build is skipped when the binary is
# newer than its sources, so staging must not manufacture a fresh "now"
# mtime on every invocation. WT files keep their real mtime (cp -p) so an
# unedited working tree doesn't force a rebuild; a git rev's content is
# immutable, so it gets a fixed epoch mtime instead.
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
echo "==> staged $REV -> $STAGE_DIR"

# ---- 2. ship ----------------------------------------------------------------
ssh "$PI_HOST" "mkdir -p $PI_DIR"
rsync -az "$STAGE_DIR"/ "$PI_HOST:$PI_DIR/"
echo "==> shipped to $PI_HOST:$PI_DIR"

# ---- 2b. --input: ship captured frames once, translate local frame paths --
# Frames are shared across revisions/builds, so they live at a fixed
# ~/ntsc_bench/frames on the Pi - a sibling of the per-rev build dir
# $PI_DIR, not inside it - rather than being copied again for every REV.
# Only shipped when the bench args actually use --input; rsync is
# incremental, so repeating this once frames are already present is cheap.
FRAMES_LOCAL_DIR="$(realpath -m "$REPO_ROOT/tools/ntsc_bench/frames")"
PI_FRAMES_DIR="ntsc_bench/frames"
HAS_INPUT=0
for a in "$@"; do [ "$a" = "--input" ] && HAS_INPUT=1; done

if [ "$HAS_INPUT" = "1" ]; then
    ssh "$PI_HOST" "mkdir -p $PI_FRAMES_DIR"
    rsync -az "$FRAMES_LOCAL_DIR"/*.bin "$PI_HOST:$PI_FRAMES_DIR/"
    echo "==> shipped captured frames to $PI_HOST:$PI_FRAMES_DIR/"

    # Translate a --input value (a single path, or a comma-separated list of
    # them) that names a local path under tools/ntsc_bench/frames - e.g. the
    # directory itself, or a specific frame_*.bin file in it, given either
    # relative to the repo root or as an absolute path - into the equivalent
    # path on the Pi, relative to $PI_DIR (whose sibling $PI_FRAMES_DIR is
    # "../frames" once the remote shell has cd'd into $PI_DIR). Any part that
    # isn't under the local frames dir (e.g. a path already meaningful on the
    # remote side) is passed through unchanged.
    ARGS=("$@")
    prev=""
    for i in "${!ARGS[@]}"; do
        if [ "$prev" = "--input" ]; then
            spec="${ARGS[$i]}"
            new_spec=""
            IFS=',' read -ra PARTS <<< "$spec"
            for p in "${PARTS[@]}"; do
                case "$p" in
                    /*) abs_p="$p" ;;
                    *)  abs_p="$REPO_ROOT/$p" ;;
                esac
                abs_p="$(realpath -m "$abs_p")"
                case "$abs_p" in
                    "$FRAMES_LOCAL_DIR"/*) p="../frames${abs_p#$FRAMES_LOCAL_DIR}" ;;
                    "$FRAMES_LOCAL_DIR")   p="../frames" ;;
                esac
                new_spec="${new_spec:+$new_spec,}$p"
            done
            ARGS[$i]="$new_spec"
        fi
        prev="${ARGS[$i]}"
    done
    set -- "${ARGS[@]}"
    echo "==> --input translated to: $*"
fi

# ---- 3/4. build (only if stale) + run, pinned + governor bracket -----------
SCALAR_FLAG=""
[ "$FN" = "snes_ntsc_blit_hires" ] && SCALAR_FLAG="-DBENCH_SCALAR"

# Auto-detect the slot-indexed API (Part A of the NTSC filter re-architecture): if the
# staged snes_ntsc.h declares snes_ntsc_update_slot, build ntsc_bench.cpp's
# -DBENCH_SLOT_API branch to match; older revisions (e.g. 086591c) don't have the symbol,
# so they build against the classic colour-indexed table unchanged.
SLOT_API_FLAG=""
grep -q "snes_ntsc_update_slot" "$STAGE_DIR/snes_ntsc.h" 2>/dev/null && SLOT_API_FLAG="-DBENCH_SLOT_API"

# Pick cores ending at 3 (away from resident services), one per thread, e.g.
# --threads 2 -> "2,3", to actually exercise multi-core scaling.
THREADS=1
prev=""
for a in "$@"; do
    [ "$prev" = "--threads" ] && THREADS="$a"
    prev="$a"
done
case "$THREADS" in ''|*[!0-9]*) THREADS=1;; esac
[ "$THREADS" -ge 1 ] || THREADS=1
CORES=3
i=1
while [ "$i" -lt "$THREADS" ]; do
    CORES="$((3-i)),$CORES"
    i=$((i+1))
done

BENCH_ARGS_STR=""
for a in "$@"; do BENCH_ARGS_STR="$BENCH_ARGS_STR $(printf '%q' "$a")"; done

ssh "$PI_HOST" bash -s -- <<EOF
set -e
cd "$PI_DIR"
need_build=0
[ -x "$BIN" ] || need_build=1
for f in ntsc_bench.cpp snes_ntsc.cpp snes_ntsc.h snes_ntsc_config.h snes_ntsc_impl.h; do
    [ "\$f" -nt "$BIN" ] 2>/dev/null && need_build=1
done
if [ "\$need_build" = "1" ]; then
    echo "building $BIN ..."
    time g++ -O3 -mcpu=cortex-a53 -std=c++17 -DBENCH_FN=$FN -DBENCH_REV=$REV $SCALAR_FLAG $SLOT_API_FLAG \\
        ntsc_bench.cpp snes_ntsc.cpp -o $BIN -lpthread
else
    echo "$BIN up to date, skipping build"
fi
echo "--- vcgencmd get_throttled (before) ---"
vcgencmd get_throttled
sudo sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > \$g; done'
echo "--- running: taskset -c $CORES ./$BIN$BENCH_ARGS_STR ---"
taskset -c $CORES ./$BIN$BENCH_ARGS_STR
echo "--- vcgencmd get_throttled (after) ---"
vcgencmd get_throttled
sudo sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo ondemand > \$g; done'
EOF
