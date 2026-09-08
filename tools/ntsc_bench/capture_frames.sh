#!/usr/bin/env bash
# Regenerate the 24-frame NTSC benchmark corpus from your own OutRun ROMs.
#
# The corpus is NOT distributed: the frames are rendered OutRun graphics and
# therefore SEGA's copyright, exactly like the ROMs themselves. This script
# reproduces it locally instead. All it needs is a build of the game and a
# legally-owned revision B ROM set.
#
# Usage (from the repo root):
#   tools/ntsc_bench/capture_frames.sh [outdir]
#
# outdir defaults to tools/ntsc_bench/frames.
#
# Environment overrides:
#   GAME       path to the cannonball-se binary   (default: build/cannonball-se)
#   ROMS       path to the ROM directory          (default: roms/)
#   RES        path to the res directory          (default: res/)
#   KEEP_ALL   1 = keep all 150 captured frames instead of the 24 selected
#
# How the corpus is defined
# -------------------------
# A 150-frame attract-mode capture at an interval of 61 rendered frames, i.e.
# frames 61, 122, ... 9150. 61 is deliberately coprime with the NTSC filter's
# 3-phase cycle, so the captured set covers all three phases evenly rather than
# sampling one of them repeatedly.
#
# 24 of those 150 are then selected - the set listed in frames/manifest.txt,
# chosen to span the attract sequence and all three phases. This script keeps
# exactly those and deletes the rest.
#
# Determinism
# -----------
# -perftest -t 1 puts the engine in its deterministic single-threaded mode (the
# same mode CB_FRAME_CKSUM relies on), so the same ROM set and the same config
# produce the same frames every time, on any machine. The capture config below
# is written fresh on every run rather than read from your own config.xml, so a
# local settings change cannot silently alter the corpus.
#
# Verify your capture with:
#   cd <outdir> && sha256sum -c frames.sha256
# using the reference list committed alongside manifest.txt, and cross-check the
# aggregate with the bench itself:
#   tools/ntsc_bench/run_host.sh WT snes_ntsc_blit_hires_vec --input <outdir>
# which must print checksum=0xe12371d9049c3d61.
#
# The reference hashes are pinned to a commit
# -------------------------------------------
# The corpus is the game's rendered output, so any deliberate change to what the
# engine draws re-baselines it. frames/frames.sha256 was generated at 180c093 and
# verified identical on x86-64 and 32-bit ARMv7. It is NOT valid for arbitrary
# revisions: capturing at 281ad89, for instance, gives the older corpus whose
# aggregate was checksum=0xe3557fa06310379d, because 7100ec0 ("restore exact ROM
# sprite sizes for the default hiresprites==0 path") intentionally changed sprite
# rendering afterwards. If you change engine output on purpose, re-run this script
# and commit the new frames.sha256 in the same change.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTDIR="${1:-$REPO_ROOT/tools/ntsc_bench/frames}"
MANIFEST="$REPO_ROOT/tools/ntsc_bench/frames/manifest.txt"

GAME="${GAME:-$REPO_ROOT/build/cannonball-se}"
ROMS="${ROMS:-$REPO_ROOT/roms/}"
RES="${RES:-$REPO_ROOT/res/}"

INTERVAL=61
COUNT=150

die() { echo "capture_frames: $*" >&2; exit 1; }

[ -x "$GAME" ]      || die "no game binary at '$GAME' (build it first, or set GAME=)"
[ -d "$ROMS" ]      || die "no ROM directory at '$ROMS' (set ROMS=)"
[ -f "$MANIFEST" ]  || die "no manifest at '$MANIFEST'"

# The ROM set must actually be present; the game soft-fails to a partial start
# otherwise and would produce a capture that is not the corpus.
ls "$ROMS"/epr-10380b.133 >/dev/null 2>&1 || \
    die "'$ROMS' does not look like a revision B set (epr-10380b.133 missing) - see roms/roms.txt"

mkdir -p "$OUTDIR"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CFG="$WORK/capture.xml"

# The capture configuration. Only the settings that change what the dump
# contains are pinned; everything else takes the compiled-in default.
#
#   hires 1 / widescreen 0  -> the corpus geometry, 640x448
#   blargg 1                -> the dump is written from inside the Blargg path
#                              (rendersurface.cpp), so it must be enabled or
#                              nothing is captured at all
#   fps 1                   -> 60fps, so frame numbering matches the corpus
#   sound.enable 0          -> no audio device is opened; keeps the capture
#                              runnable headless and over ssh. Audio does not
#                              feed back into rendering in deterministic mode.
#   menu.enabled 0          -> boot straight into attract mode
cat > "$CFG" <<XML
<?xml version="1.0" encoding="utf-8"?>
<data>
	<rompath>$ROMS</rompath>
	<respath>$RES</respath>
	<savepath>$WORK/</savepath>
	<crc32>1</crc32>
</data>
<video>
	<mode>0</mode>
	<window><scale>1</scale></window>
	<fps>1</fps>
	<widescreen>0</widescreen>
	<hires>1</hires>
	<hiresprites>0</hiresprites>
	<vsync>0</vsync>
	<shader_mode>0</shader_mode>
	<shadow_mask>0</shadow_mask>
	<blargg>1</blargg>
	<saturation>30</saturation>
	<contrast>0</contrast>
	<brightness>0</brightness>
	<sharpness>0</sharpness>
	<resolution>0</resolution>
	<gamma>0</gamma>
	<hue>-2</hue>
</video>
<sound>
	<enable>0</enable>
	<advertise>1</advertise>
	<preview>1</preview>
	<fix_samples>1</fix_samples>
	<rate>44100</rate>
</sound>
<engine>
	<time>0</time>
	<traffic>1</traffic>
	<japanese_tracks>0</japanese_tracks>
	<prototype>0</prototype>
	<levelobjects>1</levelobjects>
	<randomgen>1</randomgen>
	<fix_bugs>1</fix_bugs>
	<fix_timer>1</fix_timer>
	<new_attract>0</new_attract>
	<freeplay>1</freeplay>
</engine>
<menu>
	<enabled>0</enabled>
</menu>
XML

echo "capture_frames: $COUNT frames every $INTERVAL rendered frames -> $OUTDIR"
echo "capture_frames: game=$GAME"
echo "capture_frames: roms=$ROMS"

# engine.randomgen 1 pins the RNG seed to the original game's, so the attract
# sequence is the same every run - without it the traffic layout drifts and the
# capture is not reproducible.
( cd "$REPO_ROOT" && "$GAME" -cfgfile "$CFG" -perftest -t 1 \
        -dump "$OUTDIR" "$INTERVAL" "$COUNT" ) || die "capture run failed"

written=$(ls "$OUTDIR"/frame_*.bin 2>/dev/null | wc -l)
[ "$written" -ge "$COUNT" ] || die "expected $COUNT frames, got $written"

if [ "${KEEP_ALL:-0}" != "1" ]; then
    # Keep only the frames named in the manifest.
    keep="$WORK/keep.txt"
    cut -d, -f1 "$MANIFEST" | sed '/^$/d' | sort > "$keep"
    missing=0
    while read -r f; do
        [ -f "$OUTDIR/$f" ] || { echo "capture_frames: MISSING $f" >&2; missing=1; }
    done < "$keep"
    [ "$missing" = "0" ] || die "capture did not produce every frame the manifest names"

    for f in "$OUTDIR"/frame_*.bin; do
        grep -qx "$(basename "$f")" "$keep" || rm -f "$f"
    done
fi

kept=$(ls "$OUTDIR"/frame_*.bin 2>/dev/null | wc -l)
echo "capture_frames: kept $kept frames in $OUTDIR"

REF="$REPO_ROOT/tools/ntsc_bench/frames/frames.sha256"

# Never overwrite the committed reference list: write this capture's hashes
# beside it as frames.sha256.local, then verify against the reference.
( cd "$OUTDIR" && sha256sum frame_*.bin > frames.sha256.local )
echo "capture_frames: hashes written to $OUTDIR/frames.sha256.local"

if [ -f "$REF" ]; then
    if ( cd "$OUTDIR" && sha256sum -c --quiet "$REF" ) 2>/dev/null; then
        echo "capture_frames: OK - all $kept frames match the reference list"
    else
        echo "capture_frames: MISMATCH against $REF" >&2
        echo "capture_frames: the reference is pinned to a commit (see the header of" >&2
        echo "capture_frames: this script). Either you are on a revision whose engine" >&2
        echo "capture_frames: output differs, or your ROM set is not revision B." >&2
        exit 1
    fi
else
    echo "capture_frames: no reference list at $REF - nothing to verify against"
fi
