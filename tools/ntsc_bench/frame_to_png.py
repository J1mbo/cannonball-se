#!/usr/bin/env python3
"""
frame_to_png.py - convert CannonBall-SE frame-dump files to PNG images.

Uses only the Python 3 standard library (struct + zlib for a minimal PNG
encoder) -- no PIL, no numpy.

Dump file format is produced by the game's `-dump` option (see
src/main/sdl2/rendersurface.cpp, write_dump_frame()) and documented again in
frame_stats.py. This tool only needs the trailing "colour" buffer: the
16-bit values the Blargg NTSC blitter is actually handed, one per pixel,
which encode a 5-5-5 RGB colour plus a "shadow" bit:

    bit 15      : shadow flag (rendered here at half brightness)
    bits 14-10  : red   (5 bits)
    bits 9-5    : green (5 bits)
    bits 4-0    : blue  (5 bits)

Usage:
    python3 frame_to_png.py <frame.bin> <out.png> [--downscale]
    python3 frame_to_png.py --batch <frames_dir> <out_dir> [--downscale]

--downscale halves both dimensions (e.g. 640x448 -> 320x224) by averaging
each non-overlapping 2x2 block of pixels (averaged on the final 8-bit RGB,
i.e. after the shadow-brightness adjustment).
"""
import sys
import os
import glob
import struct
import zlib
import argparse

MAGIC = b"CBSEFRM1"
HEADER_BYTES = 64

_LUT = None


def _build_lut():
    """Build a 65536-entry lookup table: raw S16-hardware colour value (5-5-5
    RGB + shadow bit) -> 3-byte (R,G,B) bytes object. Built once, lazily."""
    global _LUT
    if _LUT is not None:
        return _LUT
    lut = [b''] * 65536
    for v in range(65536):
        r5 = (v >> 10) & 0x1F
        g5 = (v >> 5) & 0x1F
        b5 = v & 0x1F
        # 5-bit -> 8-bit, evenly spread across 0..255
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g5 << 3) | (g5 >> 2)
        b8 = (b5 << 3) | (b5 >> 2)
        if v & 0x8000:  # shadow bit -> half brightness
            r8 >>= 1
            g8 >>= 1
            b8 >>= 1
        lut[v] = bytes((r8, g8, b8))
    _LUT = lut
    return lut


def read_colour_buffer(path):
    """Read a dump file and return (width, height, frame_number, phase,
    colour) where colour is a tuple of width*height uint16 values."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < HEADER_BYTES or data[0:8] != MAGIC:
        raise ValueError(f"{path}: not a CBSEFRM1 dump file")
    hdr = struct.unpack_from('<16I', data, 0)
    width, height, frame_number, phase, hires, wide, palette_entries = hdr[2:9]
    pixel_count = width * height
    expected_size = HEADER_BYTES + pixel_count * 2 + palette_entries * 2 + pixel_count * 2
    if len(data) != expected_size:
        raise ValueError(f"{path}: size mismatch (got {len(data)}, expected {expected_size})")
    colour_off = HEADER_BYTES + pixel_count * 2 + palette_entries * 2
    colour = struct.unpack_from(f'<{pixel_count}H', data, colour_off)
    return width, height, frame_number, phase, colour


def colour_to_rgb8(colour):
    """Flat sequence of raw 16-bit colour values -> flat RGB888 bytes."""
    lut = _build_lut()
    return b''.join(map(lut.__getitem__, colour))


def downscale_2x2(rgb, width, height):
    """Average each non-overlapping 2x2 block of an RGB888 byte buffer."""
    dw, dh = width // 2, height // 2
    out = bytearray(dw * dh * 3)
    src_stride = width * 3
    dst_stride = dw * 3
    for y in range(dh):
        row0 = (2 * y) * src_stride
        row1 = (2 * y + 1) * src_stride
        orow = y * dst_stride
        for x in range(dw):
            s0 = row0 + x * 6
            s1 = row1 + x * 6
            d = orow + x * 3
            out[d] = (rgb[s0] + rgb[s0 + 3] + rgb[s1] + rgb[s1 + 3]) // 4
            out[d + 1] = (rgb[s0 + 1] + rgb[s0 + 4] + rgb[s1 + 1] + rgb[s1 + 4]) // 4
            out[d + 2] = (rgb[s0 + 2] + rgb[s0 + 5] + rgb[s1 + 2] + rgb[s1 + 5]) // 4
    return bytes(out), dw, dh


def write_png(path, width, height, rgb):
    """Minimal PNG encoder: 8-bit truecolour (colour type 2), one IDAT chunk,
    filter type None on every scanline."""
    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw.extend(rgb[y * stride:(y + 1) * stride])
    idat = zlib.compress(bytes(raw), 9)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', idat))
        f.write(chunk(b'IEND', b''))


def convert_one(in_path, out_path, downscale):
    width, height, frame_number, phase, colour = read_colour_buffer(in_path)
    rgb = colour_to_rgb8(colour)
    if downscale:
        rgb, width, height = downscale_2x2(rgb, width, height)
    write_png(out_path, width, height, rgb)
    return width, height, frame_number, phase


def main():
    ap = argparse.ArgumentParser(description="Convert CannonBall-SE frame dumps (.bin) to PNG")
    ap.add_argument('--batch', action='store_true',
                     help="treat <input> as a directory of frame_*.bin files and "
                          "<output> as the directory to write .png files into")
    ap.add_argument('--downscale', action='store_true',
                     help="halve both dimensions, averaging 2x2 blocks (e.g. 640x448 -> 320x224)")
    ap.add_argument('input')
    ap.add_argument('output')
    args = ap.parse_args()

    if args.batch:
        os.makedirs(args.output, exist_ok=True)
        paths = sorted(glob.glob(os.path.join(args.input, 'frame_*.bin')))
        if not paths:
            print(f"No frame_*.bin files found in {args.input}", file=sys.stderr)
            return 1
        for p in paths:
            base = os.path.splitext(os.path.basename(p))[0]
            out_path = os.path.join(args.output, base + '.png')
            w, h, fn, phase = convert_one(p, out_path, args.downscale)
            print(f"{os.path.basename(p)} -> {out_path}  ({w}x{h}, frame {fn}, phase {phase})")
    else:
        w, h, fn, phase = convert_one(args.input, args.output, args.downscale)
        print(f"{args.input} -> {args.output}  ({w}x{h}, frame {fn}, phase {phase})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
