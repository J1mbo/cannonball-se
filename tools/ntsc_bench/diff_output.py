#!/usr/bin/env python3
"""diff_output.py - pixel-diff two ntsc_bench --dump-output dumps.

Usage:
    diff_output.py A.bin B.bin [--cols N]

Compares two "CBSEOUT1" raw-pixel dumps (see ntsc_bench.cpp's --dump-output
header comment for the exact format: a 32-byte header - magic, out_width,
height, frame_count, phases_per_frame, 2x reserved - followed by
frame_count*phases_per_frame images of height rows x out_width x 4 bytes,
tightly packed, row pitch == out_width*4) pixel by pixel.

The two dumps may have different out_width (e.g. the SIMD "_fast" blitter
produces 752 output columns per input row, the scalar reference 749) - use
--cols N to limit the comparison to the first N columns of each row, which
is what makes the two comparable (both compute the same leftmost content;
they only disagree in the trailing few columns, which use a different
end-of-line formula and legitimately differ). Without --cols, the tool
defaults to the smaller of the two out_widths.

Only the Python standard library is required. If numpy is importable, it is
used to vectorize the comparison (much faster on ~100 MB dumps); otherwise a
pure-stdlib fallback compares each row's bytes in bulk (fast when rows
match, which is the common case) and only drops to a per-pixel Python loop
inside rows that actually differ.
"""
import argparse
import struct
import sys
from collections import Counter, defaultdict

MAGIC = b"CBSEOUT1"
HEADER_SIZE = 32
BYTES_PER_PIXEL = 4

try:
    import numpy as np
except ImportError:
    np = None


class Dump:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            hdr = f.read(HEADER_SIZE)
        if len(hdr) != HEADER_SIZE:
            raise SystemExit(f"{path}: file too small for a {HEADER_SIZE}-byte header")
        magic = hdr[0:8]
        if magic != MAGIC:
            raise SystemExit(f"{path}: bad magic {magic!r} (expected {MAGIC!r})")
        (self.out_width, self.height, self.frame_count,
         self.phases_per_frame, _res0, _res1) = struct.unpack_from("<IIIIII", hdr, 8)
        self.image_count = self.frame_count * self.phases_per_frame
        self.row_bytes = self.out_width * BYTES_PER_PIXEL
        self.image_bytes = self.row_bytes * self.height
        expected_size = HEADER_SIZE + self.image_bytes * self.image_count
        import os
        actual_size = os.path.getsize(path)
        if actual_size != expected_size:
            raise SystemExit(
                f"{path}: size mismatch: file is {actual_size} bytes, expected "
                f"{expected_size} (out_width={self.out_width} height={self.height} "
                f"frame_count={self.frame_count} phases_per_frame={self.phases_per_frame})")

    def open_data(self):
        f = open(self.path, "rb")
        f.seek(HEADER_SIZE)
        return f


def frame_phase_of(image_index, phases_per_frame):
    return image_index // phases_per_frame, image_index % phases_per_frame


def run_length_summary(cols):
    """Given a sorted list/iterable of distinct differing column indices
    (within one row), return the list of consecutive run lengths."""
    runs = []
    run_start = None
    prev = None
    for c in cols:
        if prev is None or c != prev + 1:
            if run_start is not None:
                runs.append(prev - run_start + 1)
            run_start = c
        prev = c
    if run_start is not None:
        runs.append(prev - run_start + 1)
    return runs


class Stats:
    def __init__(self, first_n=20):
        self.total_pixels_compared = 0
        self.total_diffs = 0
        self.per_frame_phase = Counter()   # (frame, phase) -> diff count
        self.first_diffs = []              # (frame, phase, row, col, a_hex, b_hex)
        self.first_n = first_n
        self.col_mod28 = Counter()
        self.col_mod7 = Counter()
        self.rows_affected = set()
        self.run_lengths = Counter()       # run length -> occurrence count
        self.images_with_diffs = 0

    def record_row_diffs(self, frame, phase, row, diff_cols, a_row, b_row):
        if not diff_cols:
            return
        self.total_diffs += len(diff_cols)
        self.per_frame_phase[(frame, phase)] += len(diff_cols)
        self.rows_affected.add(row)
        for c in diff_cols:
            self.col_mod28[c % 28] += 1
            self.col_mod7[c % 7] += 1
            if len(self.first_diffs) < self.first_n:
                a_val = int.from_bytes(a_row[c * 4:c * 4 + 4], "little")
                b_val = int.from_bytes(b_row[c * 4:c * 4 + 4], "little")
                self.first_diffs.append((frame, phase, row, c, a_val, b_val))
        for run in run_length_summary(diff_cols):
            self.run_lengths[run] += 1


def compare_numpy(da, db, cols, stats):
    fa = da.open_data()
    fb = db.open_data()
    n_images = da.image_count
    ppf = da.phases_per_frame
    stats.total_pixels_compared = n_images * da.height * cols
    for img in range(n_images):
        frame, phase = frame_phase_of(img, ppf)
        bufa = fa.read(da.image_bytes)
        bufb = fb.read(db.image_bytes)
        arr_a = np.frombuffer(bufa, dtype=np.uint8).reshape(da.height, da.out_width, 4)
        arr_b = np.frombuffer(bufb, dtype=np.uint8).reshape(db.height, db.out_width, 4)
        sub_a = arr_a[:, :cols, :]
        sub_b = arr_b[:, :cols, :]
        row_diff_mask = np.any(sub_a != sub_b, axis=2)  # (height, cols) bool
        if not row_diff_mask.any():
            continue
        stats.images_with_diffs += 1
        rows_with_diffs = np.nonzero(row_diff_mask.any(axis=1))[0]
        for row in rows_with_diffs:
            diff_cols = np.nonzero(row_diff_mask[row])[0].tolist()
            a_row_bytes = sub_a[row].tobytes()
            b_row_bytes = sub_b[row].tobytes()
            stats.record_row_diffs(frame, phase, int(row), diff_cols, a_row_bytes, b_row_bytes)
    fa.close()
    fb.close()


def compare_stdlib(da, db, cols, stats):
    fa = da.open_data()
    fb = db.open_data()
    n_images = da.image_count
    ppf = da.phases_per_frame
    cmp_bytes = cols * BYTES_PER_PIXEL
    stats.total_pixels_compared = n_images * da.height * cols
    for img in range(n_images):
        frame, phase = frame_phase_of(img, ppf)
        bufa = fa.read(da.image_bytes)
        bufb = fb.read(db.image_bytes)
        image_has_diff = False
        for row in range(da.height):
            off_a = row * da.row_bytes
            off_b = row * db.row_bytes
            row_a = bufa[off_a:off_a + cmp_bytes]
            row_b = bufb[off_b:off_b + cmp_bytes]
            if row_a == row_b:
                continue
            image_has_diff = True
            diff_cols = [c for c in range(cols)
                         if row_a[c * 4:c * 4 + 4] != row_b[c * 4:c * 4 + 4]]
            stats.record_row_diffs(frame, phase, row, diff_cols, row_a, row_b)
        if image_has_diff:
            stats.images_with_diffs += 1
    fa.close()
    fb.close()


def main():
    ap = argparse.ArgumentParser(description="Pixel-diff two ntsc_bench --dump-output dumps.")
    ap.add_argument("a", help="first dump (e.g. host _fast)")
    ap.add_argument("b", help="second dump (e.g. host scalar)")
    ap.add_argument("--cols", type=int, default=None,
                    help="compare only the first N columns of each row "
                         "(default: the smaller of the two out_widths)")
    args = ap.parse_args()

    da = Dump(args.a)
    db = Dump(args.b)

    if da.height != db.height:
        raise SystemExit(f"height mismatch: {args.a} has height={da.height}, "
                          f"{args.b} has height={db.height} - not comparable")
    if da.frame_count != db.frame_count or da.phases_per_frame != db.phases_per_frame:
        raise SystemExit(
            f"frame/phase count mismatch: {args.a} has frame_count={da.frame_count} "
            f"phases_per_frame={da.phases_per_frame}, {args.b} has "
            f"frame_count={db.frame_count} phases_per_frame={db.phases_per_frame} - "
            "not comparable (dumps must cover the same frames/phases)")

    default_cols = min(da.out_width, db.out_width)
    cols = args.cols if args.cols is not None else default_cols
    if cols > da.out_width or cols > db.out_width:
        raise SystemExit(f"--cols {cols} exceeds an out_width (A={da.out_width}, B={db.out_width})")

    print(f"A: {args.a}  out_width={da.out_width} height={da.height} "
          f"frame_count={da.frame_count} phases_per_frame={da.phases_per_frame}")
    print(f"B: {args.b}  out_width={db.out_width} height={db.height} "
          f"frame_count={db.frame_count} phases_per_frame={db.phases_per_frame}")
    print(f"comparing columns 0..{cols - 1} (of {da.out_width} / {db.out_width}), "
          f"engine={'numpy' if np is not None else 'stdlib'}")
    print()

    stats = Stats(first_n=20)
    if np is not None:
        compare_numpy(da, db, cols, stats)
    else:
        compare_stdlib(da, db, cols, stats)

    n_images = da.image_count
    print("=== Totals ===")
    print(f"images compared: {n_images}  ({da.frame_count} frames x {da.phases_per_frame} phases)")
    print(f"images with at least one diff: {stats.images_with_diffs}")
    print(f"pixels compared: {stats.total_pixels_compared}")
    print(f"pixel differences: {stats.total_diffs}")
    if stats.total_pixels_compared:
        pct = 100.0 * stats.total_diffs / stats.total_pixels_compared
        print(f"difference rate: {pct:.6f}%")
    print()

    if not stats.total_diffs:
        print("No differences found in the compared region.")
        return

    print("=== Per frame/phase diff counts (nonzero only) ===")
    for (frame, phase), count in sorted(stats.per_frame_phase.items()):
        print(f"  frame={frame:3d} phase={phase}  diffs={count}")
    print()

    print(f"=== First {min(len(stats.first_diffs), stats.first_n)} differences "
          f"(frame, phase, row, col, A, B) ===")
    for (frame, phase, row, col, a_val, b_val) in stats.first_diffs:
        print(f"  frame={frame:3d} phase={phase} row={row:3d} col={col:4d} "
              f"A=0x{a_val:08x} B=0x{b_val:08x}")
    print()

    print("=== Differing-column histogram mod 28 (the _fast unroll period: "
          "4 chunks x 7 outputs) ===")
    for m in range(28):
        c = stats.col_mod28.get(m, 0)
        if c:
            print(f"  col%28={m:2d}: {c}")
    print()

    print("=== Differing-column histogram mod 7 ===")
    for m in range(7):
        c = stats.col_mod7.get(m, 0)
        if c:
            print(f"  col%7={m}: {c}")
    print()

    rows_sorted = sorted(stats.rows_affected)
    print(f"=== Rows affected ({len(rows_sorted)} distinct rows) ===")
    if len(rows_sorted) <= 40:
        print(f"  {rows_sorted}")
    else:
        print(f"  first 20: {rows_sorted[:20]}")
        print(f"  last 20:  {rows_sorted[-20:]}")
    print()

    print("=== Run-length summary (consecutive differing columns within a row) ===")
    total_runs = sum(stats.run_lengths.values())
    print(f"  total runs: {total_runs}")
    for length in sorted(stats.run_lengths):
        print(f"  length {length:4d}: {stats.run_lengths[length]} run(s)")


if __name__ == "__main__":
    main()
