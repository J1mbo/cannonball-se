#!/usr/bin/env python3
"""
frame_stats.py - verify and summarise CannonBall-SE frame-dump files.

These files are produced by the game's `-dump <dir> [interval] [count]`
command-line option (see src/main/sdl2/rendersurface.cpp, the
write_dump_frame() helper, for the exact format and the call site).

File format (little-endian, one file per frame, "<dir>/frame_NNNNNN.bin"):

    header (64 bytes, 16 x uint32_t):
        char     magic[8]        = "CBSEFRM1"
        uint32_t width
        uint32_t height
        uint32_t frame_number      (rendered frames since startup; 1-based)
        uint32_t phase              (Blargg burst phase in effect, 0..2)
        uint32_t hires              (config.video.hires)
        uint32_t wide               (config.video.widescreen)
        uint32_t palette_entries    (= S16_PALETTE_ENTRIES*2)
        uint32_t reserved[7]        (zero)
    uint16_t raw[width*height]       -- S16 palette indices, as produced by the emulator
    uint16_t table[palette_entries]  -- rgb_blargg, exactly as it stood for this frame
    uint16_t colour[width*height]    -- raw[] converted through table[]: exactly what
                                         the Blargg blitter is handed for this frame

For every frame file this script:
  - checks the magic number and the overall file size,
  - checks that colour[i] == table[raw[i]] for every pixel i,
  - reports, per frame: frame number, phase, count of distinct raw indices,
    count of distinct colour values, max colour value, whether/how many
    colour values have bit 15 set (the S16 shadow bit),
  - reports three "flatness" statistics measured on the colour buffer
    (i.e. what the NTSC blitter actually receives), all as fractions in [0,1]:
      * flat6    : fraction of 6-pixel chunks that are uniform (all 6 pixels
                   equal), where chunks are aligned the way the Blargg blitter
                   consumes them -- starting at pixel 2 of each row, chunks
                   covering pixels [6c+2 .. 6c+7] for c = 0 .. 105 (640-wide
                   hires frames have 106 such non-overlapping chunks per row,
                   covering columns 2..637);
      * evenodd  : fraction of horizontally-adjacent pixel pairs (2k, 2k+1)
                   that are identical;
      * vert     : fraction of pixels equal to the pixel directly above them
                   (row y vs row y-1).
Finally it prints one summary line with the averages of the above across all
frames processed, plus the total number of verification failures (should be 0).

Usage:
    python3 frame_stats.py [frames_dir]

frames_dir defaults to a "frames" directory next to this script.
Uses numpy if available (much faster); falls back to pure-Python/struct
otherwise (correct, but slow on large frames/many files).
"""

import sys
import os
import glob
import struct

try:
    import numpy as np
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

MAGIC = b"CBSEFRM1"
HEADER_BYTES = 64
HEADER_WORDS = HEADER_BYTES // 4  # 16 x uint32_t


def read_frame(path):
    with open(path, 'rb') as f:
        data = f.read()

    if len(data) < HEADER_BYTES:
        raise ValueError(f"{path}: file too small ({len(data)} bytes, need at least {HEADER_BYTES})")

    magic = data[0:8]
    if magic != MAGIC:
        raise ValueError(f"{path}: bad magic {magic!r} (expected {MAGIC!r})")

    if HAVE_NUMPY:
        hdr = np.frombuffer(data, dtype='<u4', count=HEADER_WORDS, offset=0)
    else:
        hdr = struct.unpack_from('<16I', data, 0)

    width           = int(hdr[2])
    height          = int(hdr[3])
    frame_number    = int(hdr[4])
    phase           = int(hdr[5])
    hires           = int(hdr[6])
    wide            = int(hdr[7])
    palette_entries = int(hdr[8])
    reserved        = [int(x) for x in hdr[9:16]]

    pixel_count = width * height
    expected_size = HEADER_BYTES + pixel_count * 2 + palette_entries * 2 + pixel_count * 2
    if len(data) != expected_size:
        raise ValueError(
            f"{path}: size mismatch: file is {len(data)} bytes, expected {expected_size} "
            f"(width={width} height={height} palette_entries={palette_entries})")

    if any(reserved):
        raise ValueError(f"{path}: reserved header words are not all zero: {reserved}")

    off = HEADER_BYTES
    if HAVE_NUMPY:
        raw = np.frombuffer(data, dtype='<u2', count=pixel_count, offset=off).reshape(height, width)
        off += pixel_count * 2
        table = np.frombuffer(data, dtype='<u2', count=palette_entries, offset=off)
        off += palette_entries * 2
        colour = np.frombuffer(data, dtype='<u2', count=pixel_count, offset=off).reshape(height, width)
    else:
        raw = list(struct.unpack_from(f'<{pixel_count}H', data, off))
        off += pixel_count * 2
        table = list(struct.unpack_from(f'<{palette_entries}H', data, off))
        off += palette_entries * 2
        colour = list(struct.unpack_from(f'<{pixel_count}H', data, off))

    return {
        'path': path, 'width': width, 'height': height,
        'frame_number': frame_number, 'phase': phase,
        'hires': hires, 'wide': wide, 'palette_entries': palette_entries,
        'raw': raw, 'table': table, 'colour': colour,
    }


def analyse_numpy(fr):
    width, height = fr['width'], fr['height']
    raw, table, colour = fr['raw'], fr['table'], fr['colour']

    # colour[i] == table[raw[i]] for every pixel
    expected_colour = table[raw.reshape(-1).astype(np.int64)].reshape(height, width)
    mismatches = int(np.count_nonzero(expected_colour != colour))

    distinct_raw = int(np.unique(raw).size)
    distinct_colour = int(np.unique(colour).size)
    max_colour = int(colour.max())
    shadow_count = int(np.count_nonzero((colour & 0x8000) != 0))
    total_pixels = width * height

    # 6-pixel chunk uniformity, aligned to the blitter: chunks start at
    # pixel 2 of each row, covering [6c+2 .. 6c+7] for c = 0 .. (n_chunks-1).
    n_chunks_per_row = (width - 2) // 6
    chunk_uniform = 0
    chunk_total = n_chunks_per_row * height
    if n_chunks_per_row > 0:
        usable_width = n_chunks_per_row * 6
        block = colour[:, 2:2 + usable_width].reshape(height, n_chunks_per_row, 6)
        uniform = np.all(block == block[:, :, 0:1], axis=2)
        chunk_uniform = int(np.count_nonzero(uniform))

    # identical even/odd horizontal pixel pairs (2k, 2k+1)
    n_pairs_per_row = width // 2
    even = colour[:, 0:n_pairs_per_row * 2:2]
    odd = colour[:, 1:n_pairs_per_row * 2:2]
    pair_equal = int(np.count_nonzero(even == odd))
    pair_total = even.size

    # pixels equal to the pixel directly above
    above_equal = int(np.count_nonzero(colour[1:, :] == colour[:-1, :]))
    above_total = colour[1:, :].size

    return dict(
        mismatches=mismatches, total_pixels=total_pixels,
        distinct_raw=distinct_raw, distinct_colour=distinct_colour,
        max_colour=max_colour, shadow_count=shadow_count,
        chunk_uniform=chunk_uniform, chunk_total=chunk_total,
        pair_equal=pair_equal, pair_total=pair_total,
        above_equal=above_equal, above_total=above_total,
    )


def analyse_pure_python(fr):
    width, height = fr['width'], fr['height']
    raw, table, colour = fr['raw'], fr['table'], fr['colour']
    total_pixels = width * height

    mismatches = 0
    for i in range(total_pixels):
        if table[raw[i]] != colour[i]:
            mismatches += 1

    distinct_raw = len(set(raw))
    distinct_colour = len(set(colour))
    max_colour = max(colour)
    shadow_count = sum(1 for v in colour if v & 0x8000)

    n_chunks_per_row = (width - 2) // 6
    chunk_uniform = 0
    chunk_total = n_chunks_per_row * height
    for y in range(height):
        row_off = y * width
        for c in range(n_chunks_per_row):
            start = row_off + 6 * c + 2
            v0 = colour[start]
            if all(colour[start + k] == v0 for k in range(1, 6)):
                chunk_uniform += 1

    n_pairs_per_row = width // 2
    pair_equal = 0
    pair_total = n_pairs_per_row * height
    for y in range(height):
        row_off = y * width
        for k in range(n_pairs_per_row):
            if colour[row_off + 2 * k] == colour[row_off + 2 * k + 1]:
                pair_equal += 1

    above_equal = 0
    above_total = (height - 1) * width
    for y in range(1, height):
        row_off = y * width
        prow_off = (y - 1) * width
        for x in range(width):
            if colour[row_off + x] == colour[prow_off + x]:
                above_equal += 1

    return dict(
        mismatches=mismatches, total_pixels=total_pixels,
        distinct_raw=distinct_raw, distinct_colour=distinct_colour,
        max_colour=max_colour, shadow_count=shadow_count,
        chunk_uniform=chunk_uniform, chunk_total=chunk_total,
        pair_equal=pair_equal, pair_total=pair_total,
        above_equal=above_equal, above_total=above_total,
    )


def frac(n, d):
    return (n / d) if d else 0.0


def main():
    if len(sys.argv) > 1:
        frames_dir = sys.argv[1]
    else:
        frames_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'frames')

    paths = sorted(glob.glob(os.path.join(frames_dir, 'frame_*.bin')))
    if not paths:
        print(f"No frame_*.bin files found in {frames_dir}", file=sys.stderr)
        return 1

    print(f"Reading {len(paths)} frame(s) from {frames_dir}  "
          f"(numpy: {'yes' if HAVE_NUMPY else 'no, using pure Python'})\n")

    n_ok = 0
    n_fail = 0
    sums = dict(distinct_raw=0, distinct_colour=0, max_colour=0,
                shadow_fraction=0.0, chunk_fraction=0.0,
                pair_fraction=0.0, above_fraction=0.0)

    for path in paths:
        try:
            fr = read_frame(path)
        except ValueError as e:
            print(f"{os.path.basename(path)}: FAIL ({e})")
            n_fail += 1
            continue

        a = analyse_numpy(fr) if HAVE_NUMPY else analyse_pure_python(fr)

        ok = (a['mismatches'] == 0)
        n_ok += 1 if ok else 0
        n_fail += 0 if ok else 1

        shadow_fraction = frac(a['shadow_count'], a['total_pixels'])
        chunk_fraction = frac(a['chunk_uniform'], a['chunk_total'])
        pair_fraction = frac(a['pair_equal'], a['pair_total'])
        above_fraction = frac(a['above_equal'], a['above_total'])

        sums['distinct_raw'] += a['distinct_raw']
        sums['distinct_colour'] += a['distinct_colour']
        sums['max_colour'] += a['max_colour']
        sums['shadow_fraction'] += shadow_fraction
        sums['chunk_fraction'] += chunk_fraction
        sums['pair_fraction'] += pair_fraction
        sums['above_fraction'] += above_fraction

        status = "OK" if ok else f"FAIL ({a['mismatches']} mismatches)"
        print(f"frame {fr['frame_number']:6d}  phase={fr['phase']}  "
              f"{fr['width']}x{fr['height']}  "
              f"distinct_raw={a['distinct_raw']:5d}  distinct_colour={a['distinct_colour']:5d}  "
              f"max_colour=0x{a['max_colour']:04x}  "
              f"shadow={a['shadow_count']:7d} ({shadow_fraction*100:5.2f}%)  "
              f"flat6={chunk_fraction*100:6.2f}%  evenodd={pair_fraction*100:6.2f}%  "
              f"vert={above_fraction*100:6.2f}%  verify={status}")

    n_total = n_ok + n_fail
    print()
    if n_ok > 0:
        print(f"SUMMARY: {n_total} frame(s), {n_ok} verified OK, {n_fail} FAILED  |  "
              f"avg distinct_raw={sums['distinct_raw']/n_ok:.1f}  "
              f"avg distinct_colour={sums['distinct_colour']/n_ok:.1f}  "
              f"avg max_colour=0x{int(sums['max_colour']/n_ok):04x}  "
              f"avg shadow={sums['shadow_fraction']/n_ok*100:.2f}%  "
              f"avg flat6={sums['chunk_fraction']/n_ok*100:.2f}%  "
              f"avg evenodd={sums['pair_fraction']/n_ok*100:.2f}%  "
              f"avg vert={sums['above_fraction']/n_ok*100:.2f}%")
    else:
        print(f"SUMMARY: {n_total} frame(s), 0 verified OK, {n_fail} FAILED")

    return 0 if n_fail == 0 else 2


if __name__ == '__main__':
    sys.exit(main())
