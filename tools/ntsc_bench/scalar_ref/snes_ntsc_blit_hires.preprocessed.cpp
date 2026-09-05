// ============================================================================
// snes_ntsc_blit_hires.preprocessed.cpp
//
// Fully macro-expanded (g++ -E -P -std=c++17) view of the SCALAR Blargg SNES
// NTSC filter, generated from the sanitised snes_ntsc.cpp in this directory.
// It is a READING AID ONLY, not a build target: every SNES_NTSC_* macro used
// by snes_ntsc_init() and snes_ntsc_blit_hires() has been expanded inline so the
// literal arithmetic performed on each pixel is visible without cross-referencing
// snes_ntsc.h / snes_ntsc_impl.h. Struct/typedef definitions (init_t, pixel_info_t,
// snes_ntsc_t, etc.) are NOT duplicated here - see snes_ntsc_impl.h and snes_ntsc.h
// for those. Functions appear in dependency order: helpers first, the two
// functions of interest last.
// ============================================================================
// ---------------------------------------------------------------------------
// static void init_filters(...)
// ---------------------------------------------------------------------------
static void init_filters( init_t* impl, snes_ntsc_setup_t const* setup )
{
 float kernels [kernel_size * 2];
 {
  float const rolloff = 1 + (float) setup->sharpness * (float) 0.032;
  float const maxh = 32;
  float const pow_a_n = (float) pow( rolloff, maxh );
  float sum;
  int i;
  float to_angle = (float) setup->resolution + 1;
  to_angle = 3.14159265358979323846f / maxh * (float) 0.20 * (to_angle * to_angle + 1);
  kernels [kernel_size * 3 / 2] = maxh;
  for ( i = 0;
 i < kernel_half * 2 + 1;
 i++ )
  {
   int x = i - kernel_half;
   float angle = x * to_angle;
   if ( x || pow_a_n > (float) 1.056 || pow_a_n < (float) 0.981 )
   {
    float rolloff_cos_a = rolloff * (float) cos( angle );
    float num = 1 - rolloff_cos_a -
      pow_a_n * (float) cos( maxh * angle ) +
      pow_a_n * rolloff * (float) cos( (maxh - 1) * angle );
    float den = 1 - rolloff_cos_a - rolloff_cos_a + rolloff * rolloff;
    float dsf = num / den;
    kernels [kernel_size * 3 / 2 - kernel_half + i] = dsf - (float) 0.5;
   }
  }
  sum = 0;
  for ( i = 0;
 i < kernel_half * 2 + 1;
 i++ )
  {
   float x = 3.14159265358979323846f * 2 / (kernel_half * 2) * i;
   float blackman = 0.42f - 0.5f * (float) cos( x ) + 0.08f * (float) cos( x * 2 );
   sum += (kernels [kernel_size * 3 / 2 - kernel_half + i] *= blackman);
  }
  sum = 1.0f / sum;
  for ( i = 0;
 i < kernel_half * 2 + 1;
 i++ )
  {
   int x = kernel_size * 3 / 2 - kernel_half + i;
   kernels [x] *= sum;
   (static_cast <bool> (kernels [x] == kernels [x]) ? void (0) : __assert_fail ("kernels [x] == kernels [x]", __builtin_FILE (), __builtin_LINE (), __extension__ __PRETTY_FUNCTION__));
  }
 }
 {
  float const cutoff_factor = -0.03125f;
  float cutoff = (float) setup->bleed;
  int i;
  if ( cutoff < 0 )
  {
   cutoff *= cutoff;
   cutoff *= cutoff;
   cutoff *= cutoff;
   cutoff *= -30.0f / 0.65f;
  }
  cutoff = cutoff_factor - 0.65f * cutoff_factor * cutoff;
  for ( i = -kernel_half;
 i <= kernel_half;
 i++ )
   kernels [kernel_size / 2 + i] = (float) exp( i * i * cutoff );
  for ( i = 0;
 i < 2;
 i++ )
  {
   float sum = 0;
   int x;
   for ( x = i;
 x < kernel_size;
 x += 2 )
    sum += kernels [x];
   sum = 1.0f / sum;
   for ( x = i;
 x < kernel_size;
 x += 2 )
   {
    kernels [x] *= sum;
    (static_cast <bool> (kernels [x] == kernels [x]) ? void (0) : __assert_fail ("kernels [x] == kernels [x]", __builtin_FILE (), __builtin_LINE (), __extension__ __PRETTY_FUNCTION__));
   }
  }
 }
 {
  float weight = 1.0f;
  float* out = impl->kernel;
  int n = 7;
  do
  {
   float remain = 0;
   int i;
   weight -= 1.0f / 8;
   for ( i = 0;
 i < kernel_size * 2;
 i++ )
   {
    float cur = kernels [i];
    float m = cur * weight;
    *out++ = m + remain;
    remain = cur - m;
   }
  }
  while ( --n );
 }
}
// ---------------------------------------------------------------------------
// static void init(...)
// ---------------------------------------------------------------------------
static void init( init_t* impl, snes_ntsc_setup_t const* setup )
{
 impl->brightness = (float) setup->brightness * (0.5f * (1 << 7)) + ((1 << 7) * 2 + 0.5f);
 impl->contrast = (float) setup->contrast * (0.5f * (1 << 7)) + (1 << 7);
 impl->artifacts = (float) setup->artifacts;
 if ( impl->artifacts > 0 )
  impl->artifacts *= (1.0f * 1.5f) - 1.0f;
 impl->artifacts = impl->artifacts * 1.0f + 1.0f;
 impl->fringing = (float) setup->fringing;
 if ( impl->fringing > 0 )
  impl->fringing *= (1.0f * 2) - 1.0f;
 impl->fringing = impl->fringing * 1.0f + 1.0f;
 init_filters( impl, setup );
 if ( 256 > 1 )
 {
  float const to_float = 1.0f / (256 - (256 > 1));
  float const gamma = 1.1333f - (float) setup->gamma * 0.5f;
  int i;
  for ( i = 0;
 i < 256;
 i++ )
   impl->to_float [i] =
     (float) pow( i * to_float, gamma ) * impl->contrast + impl->brightness;
 }
 {
  float hue = (float) setup->hue * 3.14159265358979323846f + 3.14159265358979323846f / 180 * (0 + 15);
  float sat = (float) setup->saturation + 1;
  float const* decoder = setup->decoder_matrix;
  if ( !decoder )
  {
   decoder = default_decoder;
   if ( 1 )
    hue += 3.14159265358979323846f / 180 * (0 - (0 + 15));
  }
  {
   float s = (float) sin( hue ) * sat;
   float c = (float) cos( hue ) * sat;
   float* out = impl->to_rgb;
   int n;
   n = 3;
   do
   {
    float const* in = decoder;
    int n = 3;
    do
    {
     float i = *in++;
     float q = *in++;
     *out++ = i * c - q * s;
     *out++ = i * s + q * c;
    }
    while ( --n );
    if ( 3 <= 1 )
     break;
    {
 float t;
 t = s * -0.5f - c * 0.866025f;
 c = s * 0.866025f + c * -0.5f;
 s = t;
}
;
   }
   while ( --n );
  }
 }
}
// ---------------------------------------------------------------------------
// static void gen_kernel(...)
// ---------------------------------------------------------------------------
static void gen_kernel( init_t* impl, float y, float i, float q, snes_ntsc_rgb_t* out )
{
 float const* to_rgb = impl->to_rgb;
 int burst_remain = 3;
 y -= ((1 << 7) * 2 + 0.5f);
 do
 {
  pixel_info_t const* pixel = snes_ntsc_pixels;
  int alignment_remain = 3;
  do
  {
   float const yy = y * impl->fringing * pixel->negate;
   float const ic0 = (i + yy) * pixel->kernel [0];
   float const qc1 = (q + yy) * pixel->kernel [1];
   float const ic2 = (i - yy) * pixel->kernel [2];
   float const qc3 = (q - yy) * pixel->kernel [3];
   float const factor = impl->artifacts * pixel->negate;
   float const ii = i * factor;
   float const yc0 = (y + ii) * pixel->kernel [0];
   float const yc2 = (y - ii) * pixel->kernel [2];
   float const qq = q * factor;
   float const yc1 = (y + qq) * pixel->kernel [1];
   float const yc3 = (y - qq) * pixel->kernel [3];
   float const* k = &impl->kernel [pixel->offset];
   int n;
   ++pixel;
   for ( n = rgb_kernel_size;
 n;
 --n )
   {
    float i = k[0]*ic0 + k[2]*ic2;
    float q = k[1]*qc1 + k[3]*qc3;
    float y = k[kernel_size+0]*yc0 + k[kernel_size+1]*yc1 +
              k[kernel_size+2]*yc2 + k[kernel_size+3]*yc3 + ((1 << 7) * 2 + 0.5f);
    if ( 7 <= 1 )
     k--;
    else if ( k < &impl->kernel [kernel_size * 2 * (7 - 1)] )
     k += kernel_size * 2 - 1;
    else
     k -= kernel_size * 2 * (7 - 1) + 2;
    {
     int r, g, b = ( r = (int) (y + to_rgb [0] * i + to_rgb [1] * q), g = (int) (y + to_rgb [2] * i + to_rgb [3] * q), (int) (y + to_rgb [4] * i + to_rgb [5] * q));
     *out++ = ((r) << 21 | (g) << 11 | (b) << 1) - rgb_bias;
    }
   }
  }
  while ( 3 > 1 && --alignment_remain );
  if ( 3 <= 1 )
   break;
  to_rgb += 6;
  {
 float t;
 t = i * -0.5f - q * -0.866025f;
 q = i * -0.866025f + q * -0.5f;
 i = t;
}
;
 }
 while ( --burst_remain );
}
// ---------------------------------------------------------------------------
// pixel_info_t const snes_ntsc_pixels[] (data referenced by init_filters)
// ---------------------------------------------------------------------------
pixel_info_t const snes_ntsc_pixels[3] = {
    {
 (kernel_size / 2 + ((-4) - (-9) / 7 * 8) + ((((-9) + 7 * 10) % 7) != 0) + (7 - (((-9) + 7 * 10) % 7)) % 7 + (kernel_size * 2 * (((-9) + 7 * 10) % 7))), (1.0f - (((-4) + 100) & 2)), {
 1, 1, .6667f, 0 }
 }
,
    {
 (kernel_size / 2 + ((-2) - (-7) / 7 * 8) + ((((-7) + 7 * 10) % 7) != 0) + (7 - (((-7) + 7 * 10) % 7)) % 7 + (kernel_size * 2 * (((-7) + 7 * 10) % 7))), (1.0f - (((-2) + 100) & 2)), {
 .3333f, 1, 1, .3333f }
 }
,
    {
 (kernel_size / 2 + ((0) - (-5) / 7 * 8) + ((((-5) + 7 * 10) % 7) != 0) + (7 - (((-5) + 7 * 10) % 7)) % 7 + (kernel_size * 2 * (((-5) + 7 * 10) % 7))), (1.0f - (((0) + 100) & 2)), {
 0, .6667f, 1, 1 }
 }
,
}
;
// ---------------------------------------------------------------------------
// const unsigned int S16_rgbVals[] (data referenced by snes_ntsc_init)
// ---------------------------------------------------------------------------
const unsigned int S16_rgbVals[64] = {
    0,8,16,24,31,39,47,55,62,70,78,86,94,102,109,117,125,133,140,148,156,164,171,179,187,195,203,211,
    218,226,234,242,
    0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120,126,130,136,140,146,
    150,156
}
;
// ---------------------------------------------------------------------------
// static void merge_kernel_fields(...)
// ---------------------------------------------------------------------------
static void merge_kernel_fields(snes_ntsc_rgb_t* io)
{
    int n;
    for (n = burst_size;
 n;
 --n)
    {
        snes_ntsc_rgb_t p0 = io[burst_size * 0] + rgb_bias;
        snes_ntsc_rgb_t p1 = io[burst_size * 1] + rgb_bias;
        snes_ntsc_rgb_t p2 = io[burst_size * 2] + rgb_bias;
        io[burst_size * 0] =
            ((p0 + p1 - ((p0 ^ p1) & ((1L << 21) | (1 << 11) | (1 << 1)))) >> 1) - rgb_bias;
        io[burst_size * 1] =
            ((p1 + p2 - ((p1 ^ p2) & ((1L << 21) | (1 << 11) | (1 << 1)))) >> 1) - rgb_bias;
        io[burst_size * 2] =
            ((p2 + p0 - ((p2 ^ p0) & ((1L << 21) | (1 << 11) | (1 << 1)))) >> 1) - rgb_bias;
        ++io;
    }
}
// ---------------------------------------------------------------------------
// static void correct_errors(...)
// ---------------------------------------------------------------------------
static void correct_errors(snes_ntsc_rgb_t color, snes_ntsc_rgb_t* out)
{
    int n;
    for (n = 3;
 n;
 --n)
    {
        unsigned i;
        for (i = 0;
 i < rgb_kernel_size / 2;
 i++)
        {
            snes_ntsc_rgb_t error = color -
                out[i] - out[(i + 12) % 14 + 14] - out[(i + 10) % 14 + 28] -
                out[i + 7] - out[i + 5 + 14] - out[i + 3 + 28];
            {
 snes_ntsc_rgb_t fourth = (error + 2 * ((1L << 21) | (1 << 11) | (1 << 1))) >> 2;
 fourth &= (rgb_bias >> 1) - ((1L << 21) | (1 << 11) | (1 << 1));
 fourth -= rgb_bias >> 2;
 out [i + 3 + 28] += fourth;
 out [i + 5 + 14] += fourth;
 out [i + 7] += fourth;
 out [i] += error - (fourth * 3);
 }
;
        }
        out += 3 * rgb_kernel_size;
    }
}
// ---------------------------------------------------------------------------
// void snes_ntsc_init(...)
// ---------------------------------------------------------------------------
void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup)
{
    int merge_fields;
    init_t impl;
    if (!setup)
        setup = &snes_ntsc_composite;
    init(&impl, setup);
    merge_fields = setup->merge_fields;
    if (setup->artifacts <= -1 && setup->fringing <= -1)
        merge_fields = 1;
        for (unsigned int entry = 0;
 entry < snes_ntsc_palette_size;
 entry++) {
            unsigned int red_index = (entry >> 10 & 0x1F) + ((entry >> 15) * 32);
            unsigned int green_index = (entry >> 5 & 0x1F) + ((entry >> 15) * 32);
            unsigned int blue_index = (entry >> 0 & 0x1F) + ((entry >> 15) * 32);
            unsigned int ir = S16_rgbVals[red_index];
            unsigned int ig = S16_rgbVals[green_index];
            unsigned int ib = S16_rgbVals[blue_index];
            float rr = impl.to_float[ir];
            float gg = impl.to_float[ig];
            float bb = impl.to_float[ib];
            float y, i, q = ( (y = (rr) * 0.299f + (gg) * 0.587f + (bb) * 0.114f), (i = (rr) * 0.596f - (gg) * 0.275f - (bb) * 0.321f), ((rr) * 0.212f - (gg) * 0.523f + (bb) * 0.311f));
            int r, g, b = ( r = (int) (y + impl.to_rgb [0] * i + impl.to_rgb [1] * q), g = (int) (y + impl.to_rgb [2] * i + impl.to_rgb [3] * q), (int) (y + impl.to_rgb [4] * i + impl.to_rgb [5] * q));
            snes_ntsc_rgb_t rgb = ((r) << 21 | (g) << 11 | (b) << 1);
            snes_ntsc_rgb_t* out = ntsc->table[entry];
            gen_kernel(&impl, y, i, q, out);
            if (merge_fields) merge_kernel_fields(out);
            correct_errors(rgb, out);
        }
}
// ---------------------------------------------------------------------------
// void snes_ntsc_blit_hires(...)
// ---------------------------------------------------------------------------
void snes_ntsc_blit_hires(snes_ntsc_t const* ntsc, unsigned short const* input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel)
{
    int chunk_count = (in_width - 2) / (snes_ntsc_in_chunk * 2);
    for (;
 in_height;
 --in_height)
    {
        unsigned short const* line_in = input;
        char const* ktable = (char const*) (ntsc)->table + burst_phase * (snes_ntsc_burst_size * sizeof (snes_ntsc_rgb_t));
 unsigned const snes_ntsc_pixel1_ = (snes_ntsc_black);
 snes_ntsc_rgb_t const* kernel1 = (snes_ntsc_rgb_t const*) (ktable + (snes_ntsc_pixel1_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))));
 unsigned const snes_ntsc_pixel2_ = (snes_ntsc_black);
 snes_ntsc_rgb_t const* kernel2 = (snes_ntsc_rgb_t const*) (ktable + (snes_ntsc_pixel2_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))));
 unsigned const snes_ntsc_pixel3_ = (snes_ntsc_black);
 snes_ntsc_rgb_t const* kernel3 = (snes_ntsc_rgb_t const*) (ktable + (snes_ntsc_pixel3_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))));
 unsigned const snes_ntsc_pixel4_ = (line_in[0]);
 snes_ntsc_rgb_t const* kernel4 = (snes_ntsc_rgb_t const*) (ktable + (snes_ntsc_pixel4_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))));
 unsigned const snes_ntsc_pixel5_ = (line_in[1]);
 snes_ntsc_rgb_t const* kernel5 = (snes_ntsc_rgb_t const*) (ktable + (snes_ntsc_pixel5_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))));
 snes_ntsc_rgb_t const* kernel0 = kernel1;
 snes_ntsc_rgb_t const* kernelx0;
 snes_ntsc_rgb_t const* kernelx1 = kernel1;
 snes_ntsc_rgb_t const* kernelx2 = kernel1;
 snes_ntsc_rgb_t const* kernelx3 = kernel1;
 snes_ntsc_rgb_t const* kernelx4 = kernel1;
 snes_ntsc_rgb_t const* kernelx5 = kernel1;
        snes_ntsc_out_t* __restrict__ line_out = (snes_ntsc_out_t*)rgb_out;
        int n;
        line_in += 2;
        for (n = chunk_count;
 n;
 --n)
        {
            {
 unsigned color_;
 kernelx0 = kernel0;
 kernel0 = (color_ = (line_in[0]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 0 ] + kernel2 [(0 +5)%7+14] + kernel4 [(0 +3)%7+28] + kernelx0 [(0 +7)%7+7] + kernelx2 [(0 +5)%7+21] + kernelx4 [(0 +3)%7+35] + kernel1 [(0 +6)%7 ] + kernel3 [(0 +4)%7+14] + kernel5 [(0 +2)%7+28] + kernelx1 [(0 +6)%7+7] + kernelx3 [(0 +4)%7+21] + kernelx5 [(0 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[0] = raw_;
}
;
            {
 unsigned color_;
 kernelx1 = kernel1;
 kernel1 = (color_ = (line_in[1]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 1 ] + kernel2 [(1 +5)%7+14] + kernel4 [(1 +3)%7+28] + kernelx0 [(1 +7)%7+7] + kernelx2 [(1 +5)%7+21] + kernelx4 [(1 +3)%7+35] + kernel1 [(1 +6)%7 ] + kernel3 [(1 +4)%7+14] + kernel5 [(1 +2)%7+28] + kernelx1 [(1 +6)%7+7] + kernelx3 [(1 +4)%7+21] + kernelx5 [(1 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[1] = raw_;
}
;
            {
 unsigned color_;
 kernelx2 = kernel2;
 kernel2 = (color_ = (line_in[2]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 2 ] + kernel2 [(2 +5)%7+14] + kernel4 [(2 +3)%7+28] + kernelx0 [(2 +7)%7+7] + kernelx2 [(2 +5)%7+21] + kernelx4 [(2 +3)%7+35] + kernel1 [(2 +6)%7 ] + kernel3 [(2 +4)%7+14] + kernel5 [(2 +2)%7+28] + kernelx1 [(2 +6)%7+7] + kernelx3 [(2 +4)%7+21] + kernelx5 [(2 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[2] = raw_;
}
;
            {
 unsigned color_;
 kernelx3 = kernel3;
 kernel3 = (color_ = (line_in[3]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 3 ] + kernel2 [(3 +5)%7+14] + kernel4 [(3 +3)%7+28] + kernelx0 [(3 +7)%7+7] + kernelx2 [(3 +5)%7+21] + kernelx4 [(3 +3)%7+35] + kernel1 [(3 +6)%7 ] + kernel3 [(3 +4)%7+14] + kernel5 [(3 +2)%7+28] + kernelx1 [(3 +6)%7+7] + kernelx3 [(3 +4)%7+21] + kernelx5 [(3 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[3] = raw_;
}
;
            {
 unsigned color_;
 kernelx4 = kernel4;
 kernel4 = (color_ = (line_in[4]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 4 ] + kernel2 [(4 +5)%7+14] + kernel4 [(4 +3)%7+28] + kernelx0 [(4 +7)%7+7] + kernelx2 [(4 +5)%7+21] + kernelx4 [(4 +3)%7+35] + kernel1 [(4 +6)%7 ] + kernel3 [(4 +4)%7+14] + kernel5 [(4 +2)%7+28] + kernelx1 [(4 +6)%7+7] + kernelx3 [(4 +4)%7+21] + kernelx5 [(4 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[4] = raw_;
}
;
            {
 unsigned color_;
 kernelx5 = kernel5;
 kernel5 = (color_ = (line_in[5]), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 5 ] + kernel2 [(5 +5)%7+14] + kernel4 [(5 +3)%7+28] + kernelx0 [(5 +7)%7+7] + kernelx2 [(5 +5)%7+21] + kernelx4 [(5 +3)%7+35] + kernel1 [(5 +6)%7 ] + kernel3 [(5 +4)%7+14] + kernel5 [(5 +2)%7+28] + kernelx1 [(5 +6)%7+7] + kernelx3 [(5 +4)%7+21] + kernelx5 [(5 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[5] = raw_;
}
;
            {
 snes_ntsc_rgb_t raw_ = kernel0 [ 6 ] + kernel2 [(6 +5)%7+14] + kernel4 [(6 +3)%7+28] + kernelx0 [(6 +7)%7+7] + kernelx2 [(6 +5)%7+21] + kernelx4 [(6 +3)%7+35] + kernel1 [(6 +6)%7 ] + kernel3 [(6 +4)%7+14] + kernel5 [(6 +2)%7+28] + kernelx1 [(6 +6)%7+7] + kernelx3 [(6 +4)%7+21] + kernelx5 [(6 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[6] = raw_;
}
;
            line_in += 6;
            line_out += 7;
        }
        {
 unsigned color_;
 kernelx0 = kernel0;
 kernel0 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 0 ] + kernel2 [(0 +5)%7+14] + kernel4 [(0 +3)%7+28] + kernelx0 [(0 +7)%7+7] + kernelx2 [(0 +5)%7+21] + kernelx4 [(0 +3)%7+35] + kernel1 [(0 +6)%7 ] + kernel3 [(0 +4)%7+14] + kernel5 [(0 +2)%7+28] + kernelx1 [(0 +6)%7+7] + kernelx3 [(0 +4)%7+21] + kernelx5 [(0 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[0] = raw_;
}
;
        {
 unsigned color_;
 kernelx1 = kernel1;
 kernel1 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 1 ] + kernel2 [(1 +5)%7+14] + kernel4 [(1 +3)%7+28] + kernelx0 [(1 +7)%7+7] + kernelx2 [(1 +5)%7+21] + kernelx4 [(1 +3)%7+35] + kernel1 [(1 +6)%7 ] + kernel3 [(1 +4)%7+14] + kernel5 [(1 +2)%7+28] + kernelx1 [(1 +6)%7+7] + kernelx3 [(1 +4)%7+21] + kernelx5 [(1 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[1] = raw_;
}
;
        {
 unsigned color_;
 kernelx2 = kernel2;
 kernel2 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 2 ] + kernel2 [(2 +5)%7+14] + kernel4 [(2 +3)%7+28] + kernelx0 [(2 +7)%7+7] + kernelx2 [(2 +5)%7+21] + kernelx4 [(2 +3)%7+35] + kernel1 [(2 +6)%7 ] + kernel3 [(2 +4)%7+14] + kernel5 [(2 +2)%7+28] + kernelx1 [(2 +6)%7+7] + kernelx3 [(2 +4)%7+21] + kernelx5 [(2 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[2] = raw_;
}
;
        {
 unsigned color_;
 kernelx3 = kernel3;
 kernel3 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 3 ] + kernel2 [(3 +5)%7+14] + kernel4 [(3 +3)%7+28] + kernelx0 [(3 +7)%7+7] + kernelx2 [(3 +5)%7+21] + kernelx4 [(3 +3)%7+35] + kernel1 [(3 +6)%7 ] + kernel3 [(3 +4)%7+14] + kernel5 [(3 +2)%7+28] + kernelx1 [(3 +6)%7+7] + kernelx3 [(3 +4)%7+21] + kernelx5 [(3 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[3] = raw_;
}
;
        {
 unsigned color_;
 kernelx4 = kernel4;
 kernel4 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 4 ] + kernel2 [(4 +5)%7+14] + kernel4 [(4 +3)%7+28] + kernelx0 [(4 +7)%7+7] + kernelx2 [(4 +5)%7+21] + kernelx4 [(4 +3)%7+35] + kernel1 [(4 +6)%7 ] + kernel3 [(4 +4)%7+14] + kernel5 [(4 +2)%7+28] + kernelx1 [(4 +6)%7+7] + kernelx3 [(4 +4)%7+21] + kernelx5 [(4 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[4] = raw_;
}
;
        {
 unsigned color_;
 kernelx5 = kernel5;
 kernel5 = (color_ = (snes_ntsc_black), (snes_ntsc_rgb_t const*) (ktable + (color_ * (snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))));
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 5 ] + kernel2 [(5 +5)%7+14] + kernel4 [(5 +3)%7+28] + kernelx0 [(5 +7)%7+7] + kernelx2 [(5 +5)%7+21] + kernelx4 [(5 +3)%7+35] + kernel1 [(5 +6)%7 ] + kernel3 [(5 +4)%7+14] + kernel5 [(5 +2)%7+28] + kernelx1 [(5 +6)%7+7] + kernelx3 [(5 +4)%7+21] + kernelx5 [(5 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[5] = raw_;
}
;
        {
 snes_ntsc_rgb_t raw_ = kernel0 [ 6 ] + kernel2 [(6 +5)%7+14] + kernel4 [(6 +3)%7+28] + kernelx0 [(6 +7)%7+7] + kernelx2 [(6 +5)%7+21] + kernelx4 [(6 +3)%7+35] + kernel1 [(6 +6)%7 ] + kernel3 [(6 +4)%7+14] + kernel5 [(6 +2)%7+28] + kernelx1 [(6 +6)%7+7] + kernelx3 [(6 +4)%7+21] + kernelx5 [(6 +2)%7+35];
 do {
 uint32_t sub = ((raw_) >> 9) & (((1L << 21) | (1 << 11) | (1 << 1)) * 3 / 2);
 uint32_t clamp = (((1L << 21) | (1 << 11) | (1 << 1)) * 0x101) - sub;
 (raw_) |= clamp;
 clamp -= sub;
 (raw_) &= clamp;
 uint32_t rgba = (((raw_) << 3) & 0xFF000000u) | (((raw_) << 5) & 0x00FF0000u) | (((raw_) << 7) & 0x0000FF00u) | (((uint32_t)(Alevel)));
 (raw_) = __bswap_32 (rgba);
 }
 while (0);
 line_out[6] = raw_;
}
;
        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}
