# snes_ntsc hires path: reverse documentation of the scalar reference

Scope: `snes_ntsc_init()` and `snes_ntsc_blit_hires()` in `scalar_ref/`, with the
configuration `SNES_NTSC_IN_FORMAT = SNES_NTSC_S16`, `SNES_NTSC_OUT_DEPTH = 24`
(32-bit RGBA output), `SNES_NTSC_IN_T = unsigned short`, `SNES_NTSC_ADJ_IN(in) = in`,
composite preset, `merge_fields = 0`. All index expressions below were checked
against `snes_ntsc_blit_hires.preprocessed.cpp` and, independently, by emulating
the pointer-renaming loop and comparing it with the closed form in section 4
for all 749 outputs of a 640-pixel row.

Notation used throughout:

- `T[v][k]`: the table, `uint32_t table[0x10000][128]`; `v` is the raw 16-bit input value, `k` in 0..127.
- `b`: burst phase 0..2; `a`: alignment 0..2; `n`: tap 0..13.
- `j`: 0-based hires input pixel index within a row; `t`: 0-based output pixel index within a row.
- "field": one of the three 10-bit channel fields of a packed `uint32_t` (section 2).
- Numeric constants:

| symbol | expression | value |
|---|---|---|
| `snes_ntsc_entry_size` | | 128 words = 512 B |
| `snes_ntsc_burst_size` | 128/3 | 42 |
| `rgb_kernel_size` | 42/3 | 14 |
| `rgb_bits` / `rgb_unit` / `rgb_offset` | 7 / 1<<7 / 2*128+0.5 | 7 / 128 / 256.5 |
| `snes_ntsc_rgb_builder` | (1<<21)\|(1<<11)\|(1<<1) | 0x00200802 |
| `rgb_bias` | 256 * builder | 0x20080200 |
| `snes_ntsc_clamp_mask` | builder*3/2 | 0x00300C03 |
| `snes_ntsc_clamp_add` | builder*0x101 | 0x20280A02 |
| DISTRIBUTE_ERROR mask | (rgb_bias>>1) - builder | 0x0FE3F8FE |
| `kernel_half` / `kernel_size` | 16 / 33 | |
| `rescale_in` / `rescale_out` | | 8 / 7 |
| `snes_ntsc_black` | | 0 (input value 0, i.e. `T[0]`) |

---

## 1. Table layout

`snes_ntsc_t` is a single array `uint32_t table[65536][128]`, 32 MiB. One entry
per possible 16-bit input value; nothing else is stored in the object.

**Entry addressing.** `SNES_NTSC_S16(ktable, n)` is
`(uint32_t const*)(ktable + n * 512)`, and `ktable = (char*)table + burst * 168`
(168 = 42 words * 4 B). So the pointer used for input value `v` at burst phase `b` is

    kernel = &T[v][42*b]            // byte offset  v*512 + b*168

and every index used by the blitter is relative to that pointer, in the range 0..41.
The full 16-bit value is the index: no masking, no bit rearrangement. Bit 15
(shadow) selects a different DAC-level table during init (section 3) but is just
another address bit at blit time. Only the low 16 bits exist (`unsigned short`), so
the "highlight" mode mentioned in comments cannot occur.

**Internal structure of one 128-word entry.** `gen_kernel()` writes words
sequentially in the loop order `for b in 0..2: for a in 0..2: for n in 0..13`, i.e.

    word index k = 42*b + 14*a + n            (0 <= k <= 125)

```
word:   0 ..... 13 | 14 ..... 27 | 28 ..... 41 || 42 ..... 83 || 84 ..... 125 || 126 127
        a=0 n=0-13   a=1 n=0-13    a=2 n=0-13     burst 1         burst 2         pad
        \_________ burst phase 0 (42 words, 168 B) _________/
bytes:  0 ........................ 167          168 ... 335     336 ... 503     504-511
```

Within each 14-word alignment block:

```
n:   0  1  2  3  4  5  6 |  7  8  9 10 11 12 13
     "kernel" half        |  "kernelx" half
     read while the pixel |  read during the following chunk, via the
     is in the current    |  renamed pointer kernelx<i>
     6-pixel chunk        |
```

**Meaning of the 42 positions of one phase.** A hires input pixel `j` occupies a
slot `s = (j+4) mod 6` in a 6-pixel chunk (section 4). Its contribution to the
output is a 14-tap FIR: it adds one word to each of 14 consecutive output pixels
`t0(j) .. t0(j)+13`, where `t0(j)` is the output position at which its kernel
starts. The alignment `a = s >> 1` selects which 14-word block is used, because
slots 0/1, 2/3, 4/5 sit at different sub-positions of the 8-sample composite
period (they are the three lores pixel positions of Blargg's 3-in/7-out chunk).
Tap `n` of alignment `a` is therefore "the value this colour adds to the output
pixel `n` places to the right of the pixel's start position, when the pixel is at
lores sub-position `a`". Odd and even slots of the same pair use the *same* 14
words; the odd slot merely starts one output later.

**Which words are read.** The hires blitter reads all 42 words of the selected
phase (indices 0..41 relative to `kernel`), i.e. words 0..125 of the entry over
the three phases. Words 126 and 127 are never written by init and never read; they
are uninitialised padding (504 of 512 bytes are meaningful).

Reads per pixel are contiguous: a pixel in slot `s` has all of its 14 taps read
from the 56-byte range `[168*b + 56*(s>>1), +55]`, in increasing address order over
14 consecutive outputs.

---

## 2. Packed pixel format inside a table value

Each `uint32_t` holds three 10-bit fields with junk/headroom bits between them, in
the layout documented by the library as `xxxRRRRR RRRxxGGG GGGGGxxB BBBBBBBx`:

```
bit:  31 30 29 | 28 ........ 21 | 20 19 | 18 ........ 11 | 10 9 | 8 ......... 1 | 0
      x  ov ov | R value (8 b)  | ov ov | G value (8 b)  | ov ov| B value (8 b) | x
```

- Value bits: R = bits 21..28, G = bits 11..18, B = bits 1..8 (`PACK_RGB(r,g,b) = r<<21 | g<<11 | b<<1`).
- Headroom: two bits above each value (R: 29,30; G: 19,20; B: 9,10). Bit 0 and bit 31 are spare.
- A whole field is 10 bits: B = bits 1..10, G = 11..20, R = 21..30, so in "field units" a
  field holds 0..1023, with the value in field bits 0..7 and overflow in field bits 8..9.

**Bias trick.** Table entries are signed contributions stored as
`PACK_RGB(256+cr, 256+cg, 256+cb) - rgb_bias`, i.e. arithmetically
`cr*2^21 + cg*2^11 + cb*2 (mod 2^32)`. Negative contributions borrow from the field
above; that is fine because the final sum is what matters and it is guaranteed
positive by a bias. The bias is not stored in every tap: `correct_errors()` (section 3)
arranges that in each phase, only the 7 words `n = 0..6` of alignment 0 carry
`+rgb_bias` (256 per field), all other 35 words carry a pure signed contribution.
Every hires output sums exactly two such words (`kernel0[x]` and `kernel1[(x+6)%7]`),
so every 12-tap sum has a bias of exactly 512 per field. In field units the sum is

    S_ch = 512 + w_ch,        w_ch = signed sum of the 12 contributions (2 x colour for a flat area)

`rgb_bits = 7` ("half normal range to allow for doubled hires pixels") makes the
per-6-tap colour range 0..128 so that the 12-tap hires sum stays inside 8 bits:
flat colour gives `w = 2 * round(channel)` where the channel value is at most about
121 (DAC level 242 through the gamma curve), i.e. flat output at most 242.

**Clamp** (`SNES_NTSC_CLAMP_AND_CONVERT_SCALAR`, shift 9):

```
sub   = (io >> 9) & 0x00300C03;   // field bits 8,9 of each channel -> word bits (LSB-1, LSB)
clamp = 0x20280A02 - sub;         // per field: 0x101 (field bits 0 and 8) minus ov/2
io   |= clamp;
clamp -= sub;
io   &= clamp;
```

The two overflow bits `ov = S >> 8` (0..3) of channel `ch` land at word bits
`(LSB_ch - 1, LSB_ch)` so that the subtractions operate on disjoint 10-bit groups
(word bits 0..9, 10..19, 20..29); no borrow ever crosses a group (0x202 - 2*3 > 0).
The effect per channel, verified numerically, is:

| ov | S range | after clamp, extracted 8-bit value |
|---|---|---|
| 2 | 512..767 (w in 0..255) | `w` (pass-through) |
| 3 | 768..1023 (w in 256..511) | **254** |
| 1 | 256..511 (w in -256..-1) | 0 |
| 0 | 0..255 (w in -512..-257) | **1** |

(`ov = 3` is reachable in practice with bright colours next to dark ones; `ov = 0`
requires a -257 undershoot and is almost certainly unreachable with the composite
preset. Both are what the code does; see section 7.)

**Output conversion** (`SNES_NTSC_RGB_OUT_` / the scalar macro):

```
rgba = ((io << 3) & 0xFF000000)     // R: bits 21..28 -> 24..31
     | ((io << 5) & 0x00FF0000)     // G: bits 11..18 -> 16..23
     | ((io << 7) & 0x0000FF00)     // B: bits  1..8  ->  8..15
     | (uint32_t)Alevel;            // A: bits 0..7 (Alevel is not masked)
out  = bswap32(rgba);               // stored little-endian
```

After the byte swap the stored word is `0xAABBGGRR`; in memory the bytes are
`R, G, B, A` in increasing address order (SDL "RGBA32" / `ABGR8888` on a
little-endian host). `Alevel` is a `long`, cast to `uint32_t`; with `Alevel = 255`
the alpha byte is 0xFF.

**Worked example.** Suppose the 12-tap sum has fields R = 512+200, G = 512+100,
B = 512+300 (B overflows):

```
io    = 712<<21 + 612<<11 + 812<<1              = 0x59132658
sub   = (io >> 9) & 0x00300C03                  = 0x00200803   (ovR=2, ovG=2, ovB=3)
clamp = 0x20280A02 - 0x00200803                 = 0x200801FF
io   |= clamp                                   = 0x791B27FF   (B value bits forced to 1s)
clamp -= sub                                    = 0x1FE7F9FC
io   &= clamp                                   = 0x190321FC   (B bit 1 cleared -> 254)
rgba  = (io<<3 & FF000000)|(io<<5 & FF0000)|(io<<7 & FF00)|255 = 0xC864FEFF
store = bswap32(rgba)                           = 0xFFFE64C8   -> bytes R=200, G=100, B=254, A=255
```

---

## 3. Table generation

### 3.1 From the 16-bit value to (Y, I, Q)

```
red_index   = (v >> 10 & 0x1F) + (v >> 15) * 32
green_index = (v >>  5 & 0x1F) + (v >> 15) * 32
blue_index  = (v >>  0 & 0x1F) + (v >> 15) * 32
ir = S16_rgbVals[red_index] ...           // DAC level 0..242 (standard) or 0..156 (shadow, bit 15 set)
rr = impl.to_float[ir] ...                // gamma/contrast/brightness LUT, 256 floats
y  = 0.299 rr + 0.587 gg + 0.114 bb       // float32
i  = 0.596 rr - 0.275 gg - 0.321 bb
q  = 0.212 rr - 0.523 gg + 0.311 bb
```

with `to_float[l] = pow(l/255, gamma) * contrast + brightness`,
`gamma = 1.1333 - 0.5*setup.gamma` (1.1333), `contrast = 64*setup.contrast + 128`
(128), `brightness = 64*setup.brightness + 256.5` (256.5). So each channel is in
[256.5, 384.5] at default settings, i.e. `256.5 + (0..128)`. The luma
coefficients sum to 1 and the I and Q coefficients sum to 0, so `y = 256.5 + Y`
with `Y` in 0..~121.6, while `i`, `q` are offset-free. Saturation and hue enter
only through the decoder matrix `to_rgb` (default hue 0, saturation 1 gives
`to_rgb[0..5] = {0.956, 0.621, -0.272, -0.647, -1.105, 1.702}` for burst 0).

`gen_kernel(y, i, q)` first does `y -= 256.5` to get the offset-free `Y`.

Also computed in init for `correct_errors`: the colour's own reference RGB,
`rgb = PACK_RGB((int)(y + d0 i + d1 q), (int)(y + d2 i + d3 q), (int)(y + d4 i + d5 q))`
using burst-0 `to_rgb` and the *offset* `y`, i.e. `PACK(256 + round(R'), 256 + round(G'), 256 + round(B'))`
where `R', G', B'` are the decoded (not the original) channel values in 0..128 scale.
This same `rgb` is the target for all three phases.

### 3.2 Filters (`init_filters`)

`impl.kernel` is 7 x 66 floats: for each of `rescale_out = 7` sub-phases, 33 chroma
taps (Gaussian, `bleed` controlled, even and odd taps normalised separately) followed
by 33 luma taps (sinc with rolloff and Blackman window, `sharpness`/`resolution`
controlled, normalised to sum 1). The 7 copies are linear-interpolation blends with
weights `7/8, 6/8, ..., 1/8` implementing the 8-composite-samples-to-7-outputs rescale.
Everything here depends only on the setup, not on the colour.

`snes_ntsc_pixels[3]` (one per alignment) resolves to

| a | `offset` into `impl.kernel` | `negate` | `kernel[4]` (sample weights) |
|---|---|---|---|
| 0 | 353 (= sub-phase 5, sample 23) | +1 | 1, 1, 0.6667, 0 |
| 1 | 22 (= sub-phase 0, sample 22)  | -1 | 0.3333, 1, 1, 0.3333 |
| 2 | 154 (= sub-phase 2, sample 22) | +1 | 0, 0.6667, 1, 1 |

The four weights say how a lores pixel (8/3 composite samples wide) covers 4
consecutive composite samples; `negate` is the sign of the colour subcarrier at
the pixel's first sample (4 samples per subcarrier cycle: +I, +Q, -I, -Q).

### 3.3 One phase, one alignment, 14 taps

For burst `b`, with `(i_b, q_b)` the chroma rotated by `-120 deg * b` (see 3.5),
`d = to_rgb + 6*b`, `F = impl.fringing` (1.0), `A = impl.artifacts` (1.0), alignment
weights `w0..w3` and sign `nu`:

```
yy  = Y * F * nu
ic0 = (i_b + yy) * w0      qc1 = (q_b + yy) * w1
ic2 = (i_b - yy) * w2      qc3 = (q_b - yy) * w3
ii  = i_b * A * nu         qq  = q_b * A * nu
yc0 = (Y + ii) * w0        yc1 = (Y + qq) * w1
yc2 = (Y - ii) * w2        yc3 = (Y - qq) * w3

k = &impl.kernel[offset_a]
for n in 0..13:
    I_n = k[0]*ic0 + k[2]*ic2
    Q_n = k[1]*qc1 + k[3]*qc3
    Y_n = k[33]*yc0 + k[34]*yc1 + k[35]*yc2 + k[36]*yc3 + 256.5
    k   = (k < &impl.kernel[396]) ? k + 65 : k - 398     // next sub-phase, one sample back; wrap = 8 samples per 7 taps
    r = (int)(Y_n + d[0]*I_n + d[1]*Q_n)
    g = (int)(Y_n + d[2]*I_n + d[3]*Q_n)
    b = (int)(Y_n + d[4]*I_n + d[5]*Q_n)
    T[v][42*b + 14*a + n] = (r<<21 | g<<11 | b<<1) - 0x20080200
```

### 3.4 Mathematical structure

Everything up to the `(int)` casts is linear in `(Y, i_b, q_b)`, and `(i_b, q_b)`
is a fixed rotation of `(I, Q)`. Hence for every (b, a, n, channel) there is a
fixed coefficient triple `(cY, cI, cQ)` (a function of the setup only) such that

    channel value before packing = 256 + floor(0.5 + cY*Y + cI*I + cQ*Q)

(`(int)` truncates toward zero, but the argument is always positive, so it is a
floor; the 0.5 in `rgb_offset` turns it into round-half-up). Therefore

    T[v][42b + 14a + n]  =  Sum_ch  round(L_{b,a,n,ch}(Y,I,Q)) << shift_ch    (mod 2^32)
                            + delta_{b,a,n}(v)                                  (error correction, 3.6)
                            + 0x20080200 if (a == 0 and n <= 6)                (bias, 3.6)

where `Y, I, Q` are themselves linear in the three gamma-LUT values
`to_float[S16_rgbVals[...]]`, which are *not* linear in the 5-bit indices. So:

- The entries are integer roundings of a linear function of (Y, I, Q), **rounded
  separately per tap and per channel**, then adjusted by a small integer correction
  that depends on the rounding residuals of six taps. Linearity holds exactly only
  before the `(int)` step. `T[v]` is not additive in R, G, B and cannot be rebuilt
  bit-exactly as `T_R[r] + T_G[g] + T_B[b]` or from a smaller set of basis tables
  with integer arithmetic; a bit-exact on-the-fly computation must replay the same
  float32 expressions in the same order (see 7.9 on FMA contraction), then the
  integer correction.
- There are 3*3*14 = 126 taps x 3 channels = 378 coefficient triples in total.

### 3.5 Relationship between the three burst phases

Independent recompute, not a permutation or sign flip. After each phase, init does

    ROTATE_IQ(i, q, -0.866025, -0.5)   // (i,q) <- rotation by -120 deg, applied cumulatively in float32
    to_rgb += 6                        // decoder matrix for this phase, itself pre-rotated by -120 deg per phase in init()

The pure colour term is invariant (`d_b . (i_b, q_b) = d_0 . (i, q)`, rotation
preserves dot products) so all three phases decode the same flat colour; the
artefact terms (`yy`, `ii`, `qq`, which use `i_b, q_b` directly against a fixed
subcarrier sign pattern) differ, which is what produces dot crawl. Because the
chroma filter applies different taps to the I and Q sample positions
(`k[0],k[2]` vs `k[1],k[3]`), phase `b` of colour `c` is not equal to phase 0 of a
hue-rotated colour either. Treat the three 42-word blocks as three unrelated tables.

### 3.6 `correct_errors` (always active) and `merge_kernel_fields` (inactive)

`correct_errors(rgb, out)` runs for each phase block (`out += 42` per phase), for
`i = 0..6`:

```
error  = rgb - out[i] - out[(i+12)%14+14] - out[(i+10)%14+28] - out[i+7] - out[i+19] - out[i+31]
fourth = ((error + 2*builder) >> 2) & 0x0FE3F8FE) - (rgb_bias >> 2)    // per field: floor((e+2)/4), bias stripped
out[i+31] += fourth;  out[i+19] += fourth;  out[i+7] += fourth;
out[i]    += error - 3*fourth;
```

The six indices are exactly the six taps the *lores* blitter sums for its output
`i` when every pixel has this colour, and `rgb = PACK(256+R', 256+G', 256+B')`.
`error` per field is `256 + e` (e = small rounding residual), `fourth` per field
is `floor((e+2)/4)` with the 256 removed, so after correction:
(1) for a flat field the six lores taps sum to `rgb` exactly, and
(2) the +256 bias ends up entirely in `out[i]`, i.e. words `n = 0..6` of alignment 0.
Two taps of each lores set (`(i+12)%14+14` and `(i+10)%14+28`) are never modified.
For the hires blitter the 12 taps of output `x` are the lores set of `x` plus the
lores set of `(x-1) mod 7`, so a flat colour gives exactly `2*rgb - 2*rgb_bias`
per field plus the 512 bias, i.e. output channel `= 2*R'` (always even, max 242).

`merge_kernel_fields` replaces, for each of the 42 positions, phase `p` by the
carry-free packed average of phases `p` and `p+1 mod 3` (bias re-added, average by
`(x + y - ((x ^ y) & builder)) >> 1`, bias removed). It is called only when
`setup->merge_fields != 0` or when `artifacts <= -1 && fringing <= -1`. With the
game's stated setup (`merge_fields = 0`, composite artifacts/fringing = 0) it is
**not** executed. Note that the preset literal `snes_ntsc_composite` in
`snes_ntsc.cpp` has `merge_fields = 1`, so the caller must explicitly clear it
(see 7.1).

**Black entry (`T[0]`).** Value 0 decodes to DAC level 0 for all channels,
`to_float[0] = 256.5`, `Y = I = Q = 0` up to float32 rounding, so every tap rounds
to 256 per channel and stores 0; `correct_errors` then adds `error = rgb_bias`,
`fourth = 0`. Expected content per phase: words 0..6 = `0x20080200`, words 7..41 = 0
(identical for all three phases).

---

## 4. `snes_ntsc_blit_hires`, de-macroed

### 4.1 Pseudo-code for one row

```c
// T viewed as uint32_t T[65536][128]; K(v) = &T[v][42*burst]
void hires_row(const uint16_t* in, uint32_t* out, int burst, int in_width, uint32_t A)
{
    const int chunk_count = (in_width - 2) / 6;             // 106 for in_width = 640
    const uint32_t *k[6], *kx[6];                            // current-chunk / previous-chunk entry pointers

    // prologue: virtual chunk -1 = (black, black, black, black, in[0], in[1]); chunk -2 = all black
    k[0] = k[1] = k[2] = k[3] = K(0);
    k[4] = K(in[0]);
    k[5] = K(in[1]);
    kx[0] = undefined;                                       // written before first use
    kx[1] = kx[2] = kx[3] = kx[4] = kx[5] = K(0);
    in += 2;

    for (int c = 0; c <= chunk_count; ++c) {                 // chunk_count real chunks + 1 tail chunk
        const int tail = (c == chunk_count);
        for (int x = 0; x < 6; ++x) {
            kx[x] = k[x];                                    // rename: current becomes previous
            k[x]  = K(tail ? 0 : in[x]);                     // load next input pixel (black in the tail)
            out[x] = make_pixel(k, kx, x, A);
        }
        out[6] = make_pixel(k, kx, 6, A);
        in += 6; out += 7;                                   // (harmless past-the-end increments in the tail)
    }
}

uint32_t make_pixel(const uint32_t* k[6], const uint32_t* kx[6], int x, uint32_t A)
{
    uint32_t s = k[0][x]          + k[2][(x+5)%7+14] + k[4][(x+3)%7+28]
               + kx[0][x+7]       + kx[2][(x+5)%7+21] + kx[4][(x+3)%7+35]
               + k[1][(x+6)%7]    + k[3][(x+4)%7+14] + k[5][(x+2)%7+28]
               + kx[1][(x+6)%7+7] + kx[3][(x+4)%7+21] + kx[5][(x+2)%7+35];
    uint32_t sub = (s >> 9) & 0x00300C03;
    uint32_t cl  = 0x20280A02 - sub;
    s |= cl;  cl -= sub;  s &= cl;
    uint32_t rgba = ((s<<3)&0xFF000000u) | ((s<<5)&0x00FF0000u) | ((s<<7)&0x0000FF00u) | A;
    return bswap32(rgba);                                    // memory bytes R,G,B,A
}
```

Per row of the outer function: `burst_phase = (burst_phase + 1) % 3`,
`input += in_row_width` (elements), `rgb_out += out_pitch` (bytes). Each row is
seeded afresh; no state survives from one row to the next except the phase counter.

### 4.2 Pointer state and slot-to-pixel mapping

Chunk `c >= 0` loads input pixels `j = 6c+2 .. 6c+7` into slots 0..5 (`k[s]`).
Slot `s` of chunk `c` therefore holds `j = 6c + 2 + s`; conversely pixel `j` is in
slot `s = (j+4) mod 6`, chunk `c = floor((j+4)/6) - 1`. Chunk -1 holds
`(black, black, black, black, j=0, j=1)` and chunk -2 is all black.

At `make_pixel(x)` inside chunk `c`:

- `k[s]` = slot `s` of chunk `c` for `s <= x` (already loaded this chunk), else slot `s` of chunk `c-1`;
- `kx[s]` = slot `s` of chunk `c-1` for `s <= x`, else slot `s` of chunk `c-2`;
- for `x = 6` all six have been loaded.

### 4.3 Resolved tap table

Indices are relative to the phase base `&T[v][42*burst]`; `k<s>` / `kx<s>` are the
current/previous-chunk pointers of slot `s`. Verified line by line against the
preprocessed expansion (lines 418, 439, 460, 481, 502, 523, 538) and by emulation.

| x | k0 | k2 | k4 | kx0 | kx2 | kx4 | k1 | k3 | k5 | kx1 | kx3 | kx5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 19 | 31 | 7  | 26 | 38 | 6 | 18 | 30 | 13 | 25 | 37 |
| 1 | 1 | 20 | 32 | 8  | 27 | 39 | 0 | 19 | 31 | 7  | 26 | 38 |
| 2 | 2 | 14 | 33 | 9  | 21 | 40 | 1 | 20 | 32 | 8  | 27 | 39 |
| 3 | 3 | 15 | 34 | 10 | 22 | 41 | 2 | 14 | 33 | 9  | 21 | 40 |
| 4 | 4 | 16 | 28 | 11 | 23 | 35 | 3 | 15 | 34 | 10 | 22 | 41 |
| 5 | 5 | 17 | 29 | 12 | 24 | 36 | 4 | 16 | 28 | 11 | 23 | 35 |
| 6 | 6 | 18 | 30 | 13 | 25 | 37 | 5 | 17 | 29 | 12 | 24 | 36 |

Reading down any column, the index increases by 1 per output and wraps within its
7-word half (`k2`: 19,20,14,15,...). The `k1/k3/k5/kx1/kx3/kx5` columns are the
`k0/k2/k4/kx0/kx2/kx4` columns shifted down by one row (odd slot = same kernel, one
output later). The two words with index <= 6 in every row (`k0`, `k1`) are the two
bias-carrying taps.

### 4.4 Closed form (equivalent to the loop, checked for all 749 outputs)

Define for hires input pixel `j` (real pixels 0..637, black for `j < 0` and
`j >= 638`, up to `j = 643`):

    t0(j) = j - 3 + floor((j + 4) / 6)       // output position of tap 0
    a(j)  = ((j + 4) mod 6) >> 1             // alignment block

    sum(t) = Sum over j with 0 <= t - t0(j) <= 13 of  T[P(j)][42*burst + 14*a(j) + (t - t0(j))]

Examples: `t0 = -3, -2, 0, 1, 2, 3, 4, 5, 7, 8, ...` for `j = 0, 1, 2, ...`,
`a = 2, 2, 0, 0, 1, 1, 2, 2, 0, ...`. Six consecutive pixels occupy seven
consecutive start positions (one gap after every slot-5 pixel), so each output
receives contributions from exactly 12 pixels. Equivalently, using the bias facts of
section 2:

    sum(t) = 2*0x20080200 + Sum over the 12 real/black pixels of (T[..] - (0x20080200 if the index <= 6))

### 4.5 Row extent for a 640-pixel input

- `chunk_count = (640 - 2) / 6 = 106`. Prologue consumes `in[0..1]`, the loop consumes
  `in[2..637]` (636 pixels), the tail feeds 6 black pixels.
- **Input pixels 638 and 639 are never read** (dropped). No pixel is duplicated.
- Outputs written: `106*7 + 7 = 749 = SNES_NTSC_OUT_WIDTH(320)`, at `line_out[0..748]`,
  contiguous 2996 bytes (`out_pitch = 749*4`).
- Left edge: pixel 0 starts at `t0 = -3`, so its taps 0..2 (words 28..30) never reach
  the output; pixel 1 loses taps 0..1. Output 0 is
  `p2[0] + p0[31] + p1[30] + nine black taps` (of which `black[6]` carries the second bias).
- Right edge: pixel 637 (slot 5 of chunk 105) starts at `t0 = 740`; only its taps 0..8
  (words 28..36) are used; pixel 636 uses taps 0..9, etc. Outputs 742..748 are the
  decaying tails of pixels 632..637 plus black.

### 4.6 Worked example: second loop iteration (c = 1), output x = 3

This is `line_out[10]`. At this point `k[0..3]` hold pixels 8..11 (loaded this
chunk), `kx[0..3]` hold pixels 2..5, `k[4..5]` still hold pixels 6..7 from chunk 0,
and `kx[4..5]` hold pixels 0..1 from the prologue. The twelve taps, in source order:

| term | pointer | pixel | word index (rel. to phase base) | absolute word in `T[P]` | tap n / alignment |
|---|---|---|---|---|---|
| 1 | `k0[3]` | p8 | 3 | 42b+3 | n=3, a=0 |
| 2 | `k2[15]` | p10 | 15 | 42b+15 | n=1, a=1 |
| 3 | `k4[34]` | p6 | 34 | 42b+34 | n=6, a=2 |
| 4 | `kx0[10]` | p2 | 10 | 42b+10 | n=10, a=0 |
| 5 | `kx2[22]` | p4 | 22 | 42b+22 | n=8, a=1 |
| 6 | `kx4[41]` | p0 | 41 | 42b+41 | n=13, a=2 |
| 7 | `k1[2]` | p9 | 2 | 42b+2 | n=2, a=0 |
| 8 | `k3[14]` | p11 | 14 | 42b+14 | n=0, a=1 |
| 9 | `k5[33]` | p7 | 33 | 42b+33 | n=5, a=2 |
| 10 | `kx1[9]` | p3 | 9 | 42b+9 | n=9, a=0 |
| 11 | `kx3[21]` | p5 | 21 | 42b+21 | n=7, a=1 |
| 12 | `kx5[40]` | p1 | 40 | 42b+40 | n=12, a=2 |

Byte address of term `m`: `(char*)table + P*512 + b*168 + index*4`. The closed form
agrees: e.g. `j = 0`: `t0 = -3`, `t - t0 = 13`, `a = 2` -> word 28+13 = 41.

---

## 5. Cost model (scalar code as written)

Per output pixel:

| item | count |
|---|---|
| table loads (32-bit) | 12, from up to 12 different 512-byte entries |
| integer adds for the sum | 11 |
| clamp ALU | 6 (shift, and, sub, or, sub, and); on AArch64 the shift folds into the `and` -> 5 |
| convert ALU | 9 as written (3 shifts, 3 ands, 3 ors incl. alpha); with shifted-operand `and` -> 6 |
| byte swap | 1 (`REV`) |
| store | 1 (32-bit) |

Per 6-input / 7-output iteration: 6 input loads (`ldrh`), 6 address computations
(`base + v << 9`, 1-2 ops each), 6 pointer renames (register moves, usually free
after unrolling), 84 table loads, 77 adds, ~105-140 other ALU, 7 stores, loop
overhead ~4. Roughly 90 loads, ~200 ALU, 7 stores per iteration; per output about
13 loads, 28 ALU, 1 store. Nothing is multiplied; the only data-dependent
addresses are the 6 entry-base computations per iteration, and every load is
dependent on one of them (no load-to-load chains beyond that). There is no
accumulator carried across outputs.

Per 640-pixel row: 107 iterations (106 + tail) -> 749 outputs, **8988 table
loads**, 638 input loads, 749 stores, about 21k ALU ops.

Entries touched per iteration: pixels of chunks `c` (6), `c-1` (6) and `c-2`
(slots 1..5, at x = 0..4) -> up to 17 distinct entries per iteration (12 per output).

Byte ranges read within a 512-byte entry, per phase, for a pixel in slot group
`g = s >> 1` (the 56-byte window `[168b + 56g, +55]`), assuming the table is
64-byte aligned (entries then are, being 512 B):

| burst b | g=0 bytes / lines | g=1 bytes / lines | g=2 bytes / lines | whole phase |
|---|---|---|---|---|
| 0 | 0-55 / L0 | 56-111 / L0,L1 | 112-167 / L1,L2 | 168 B, lines 0-2 (3) |
| 1 | 168-223 / L2,L3 | 224-279 / L3,L4 | 280-335 / L4,L5 | 168 B, lines 2-5 (4) |
| 2 | 336-391 / L5,L6 | 392-447 / L6 | 448-503 / L7 | 168 B, lines 5-7 (3) |

So one pixel touches 1 or 2 cache lines of its entry (average 5/3, 2, 4/3 lines for
b = 0, 1, 2). Within an iteration, the current-chunk pixels read the first 28 bytes
of their window and the previous-chunk pixels the last 28 bytes.

Table bytes per row:

- every pixel a distinct colour: 8988 x 4 = 35,952 B of words; distinct lines about
  1063 / 1276 / 851 (b = 0/1/2) = ~68 / 82 / 54 KB of new cache lines per row
  (plus the black entry), i.e. the whole row's working set exceeds L1 (32 KB).
- all pixels the same colour: one entry's phase block (168 B, 3-4 lines = 192-256 B)
  plus the black entry's block; 8988 loads all hit L1.

Per frame these figures scale by 448 rows; the phase cycles with the row so all
three phase blocks of every colour on screen are needed across any three consecutive rows.

---

## 6. Structural facts relevant to vectorisation

1. **Pure 14-tap FIR over a 6:7 resampling grid.** `out[t] = Sum_j K_{a(j)}[P(j)][t - t0(j)]`
   with `t0(j) = j - 3 + floor((j+4)/6)`; each input pixel contributes to 14 consecutive
   outputs, each output receives 12 pixels. Period: 6 inputs = 7 outputs; the tap
   pattern for `t` and `t + 7` is identical with `j` shifted by 6.
2. **Adjacent outputs read adjacent words.** For a fixed pixel, outputs `t, t+1, t+2, t+3`
   read words `n, n+1, n+2, n+3` of the same 14-word block (contiguous 16 bytes), except
   where the window ends (n > 13) and the next pixel of the same slot takes over.
   Per slot column, four consecutive outputs never need more than two different entries.
3. **Same words for even/odd slots.** Slots 2p and 2p+1 use the same 14-word block; the
   odd one is delayed by exactly one output.
4. **Exactly two bias taps per output** (`a = 0, n <= 6`), always: bias 512 per field.
   Any re-layout that changes how many `n <= 6` words are summed must compensate with a
   constant.
5. **Sums are plain 32-bit wrap-around adds** on packed fields (10-bit lanes with 2-bit
   headroom); no saturating arithmetic anywhere; the clamp is 5 bit-ops on the final sum.
   The three fields are independent under addition as long as each stays within
   0..1023 in field units.
6. **No state between outputs**, and the only state between iterations is the 12
   entry pointers (a sliding window of the last 12 input values); nothing arithmetic
   is carried. The prologue is a window pre-filled with black (value 0).
7. **Rows are independent** given their phase; the phase is `(burst_phase_arg + row) mod 3`
   within a call. Vertical neighbours are never read.
8. **Phases are unrelated tables**; the phase selects a 168-byte sub-block at offset
   `168*b` of every entry. Words 126-127 of each entry are unused.
9. **Black padding constant**: input value 0, entry `T[0]`; contributes 0 for all words
   `n >= 7` and `0x20080200` for `a = 0, n <= 6` (expected values; see 3.6).
10. **Edges**: the 638 consumed input pixels start at `t0 = -3`; taps falling outside
    `0..748` are simply not produced; input pixels 638-639 are dropped.
11. **Output** is 749 contiguous `uint32` per row, bytes R,G,B,A; alpha is the
    `Alevel` argument unchanged; `out_pitch` equals the row size.
12. **Address computation** is `table + (v << 9) + 168*b` with the 16-bit input used
    verbatim; there is no dependence on neighbouring input values.

---

## 7. Surprises and caveats

1. **Preset has `merge_fields = 1`.** `snes_ntsc_composite = {0,0,0,0,0, 0,0,0,0,0, 1,0,0}`;
   the 11th initialiser is `merge_fields`. If the caller passed the preset unmodified or
   `NULL`, `merge_kernel_fields` would run and every phase would become the average of two
   phases. The brief states `merge_fields = 0`; this document assumes the caller clears it.
   Anyone regenerating the table must match whichever the game actually does.
2. **Two input pixels per row are dropped** (`in[638]`, `in[639]`), because
   `chunk_count = (in_width - 2) / 6` rounds 638/6 down to 106. Bit-exact tail outputs
   742..748 depend on those pixels being replaced by black, not read.
3. **Saturation clamps to 254, not 255**, and a field that underflows below -256 (relative
   to the bias) yields 1, not 0 (section 2 table, verified numerically). The 254 case is
   reachable with the composite preset (bright next to dark with chroma artefacts). A
   reimplementation using ordinary saturating arithmetic would differ in exactly those
   pixels.
4. **Flat areas only produce even channel values** (`2*R'`), maximum 242 for DAC level 242.
   This is a consequence of `rgb_bits = 7` plus the doubled tap set, not a bug, but it is
   easy to mistake for one when comparing against a "true" 8-bit reference.
5. **`snes_ntsc.txt` is out of date for this build**: it describes 13-bit RGB input and a
   4 MB table; this build indexes all 16 bits (32 MiB) and uses no bit reduction. Its
   burst-phase advice (toggle 0/1) also does not describe the game's 0,1,2 cycle.
6. **`Alevel` is not masked**: any bits above 7 would corrupt B/G/R. Fine for 255.
7. **The ASCII table in `snes_ntsc.cpp` (lines 291-322)** illustrates the pointer flow
   but with its own labelling ("2.14" etc.); it is not authoritative and I did not use it.
8. **`correct_errors` uses burst-0 `to_rgb` for all phases** (via `rgb` computed once in
   init). This is consistent because the decoded flat colour is phase-invariant, but note
   the target is the *round-tripped* colour (RGB -> YIQ -> RGB with slightly non-inverse
   matrices and gamma), not the original DAC level.
9. **Floating-point reproducibility (uncertain).** Table values depend on float32
   evaluation order. GCC's default `-ffp-contract=fast` may fuse `a*b + c` into FMA on
   AArch64 (and not on SSE2), so a table generated on one platform/compiler is not
   guaranteed bit-identical to another. Only matters if a new design regenerates or
   reformats the table with different code; comparing against a table produced by the
   *same* init on the same platform is safe.
10. **Odd-slot approximation.** A hires pixel is 7/6 of an output pixel wide, but odd
    slots reuse the even slot's kernel delayed by exactly one output. Deliberate library
    design (same as Blargg's SNES hires blitter), stated here because a "cleaner" filter
    would not be bit-identical.
11. **Two uninitialised words per entry** (126, 127). Never read by the blitter; only
    relevant if a new layout copies whole entries and compares them byte-wise.
12. **Bottom-half thread phase (outside this code, uncertain).** Phase advances by one per
    row inside a call, so the second half-frame call must be given `(phase + 224) mod 3 =
    (phase + 2) mod 3` to reproduce a single full-frame call. Whether the renderer does
    this was not visible in the files reviewed.
13. **Black entry contents** (3.6) are derived, not dumped: `Y` for value 0 is
    `256.5f*(0.299f+0.587f+0.114f) - 256.5f`, a few float32 ulps from 0; the taps still
    round to 256 with a wide margin, so the stated values (0 and `0x20080200`) are
    expected with high confidence but should be confirmed by reading `T[0]` once.
