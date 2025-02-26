#include "renderbase.hpp"
#include <iostream>


RenderBase::RenderBase()
{
    surface       = NULL;
    screen_pixels = NULL;

    orig_width  = 0;
    orig_height = 0;

    // initial colour lookup tables
    memset(rgb, 0, S16_PALETTE_ENTRIES * 3 * sizeof(uint32_t));
    memset(rgb_blargg, 0, S16_PALETTE_ENTRIES * 3 * sizeof(uint32_t));
}

// Setup screen size
bool RenderBase::sdl_screen_size()
{
    if (orig_width == 0 || orig_height == 0)
    {
	SDL_DisplayMode info;

	SDL_GetCurrentDisplayMode(0, &info);

        orig_width  = info.w;
        orig_height = info.h;
    }

    scn_width  = orig_width;
    scn_height = orig_height;

    return true;
}


// The following lookup tables match the ladder DAC to linear RGB values, assuming the arcade monitor
// has an input impedence of 2k2. Shadows and highlights work by applying an extra 220R resistor to
// VCC or GND when called, drived by a 74LS125 (tri-state).
//
// These are used to convert the S16 tile colours to RGB values for rendering, via applied colour curve
// settings which applied.

const uint32_t S16_rgbVal[32] = {
  0,8,16,24,31,39,47,55,62,70,78,86,94,102,109,117,125,133,140,148,156,164,171,179,187,195,203,211,
  218,226,234,242};

uint32_t r_rgbVal[32];
uint32_t g_rgbVal[32];
uint32_t b_rgbVal[32];

const uint32_t S16_shadowVal[32] = {
  0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120,126,130,136,140,146,
  150,156};

uint32_t r_shadowVal[32];
uint32_t g_shadowVal[32];
uint32_t b_shadowVal[32];

const uint32_t S16_hiliteVal[32] = {
  91,96,101,106,111,116,121,126,131,137,141,147,151,157,161,167,172,177,182,187,192,197,202,207,212,
  217,222,227,232,237,242,247};

uint32_t r_hiliteVal[32];
uint32_t g_hiliteVal[32];
uint32_t b_hiliteVal[32];


// See: SDL_PixelFormat
#define CURRENT_RGB() (r << Rshift) | (g << Gshift) | (b << Bshift);
// Blargg filter expects 565 format
//#define CURRENT_RGB_BLARRG() ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);

// The following passes the S16 raw RGB values plus flags for shadow/hilite to Blargg
// The actual S16 output values are looked up in the Blargg filter
// This reduces the computation required for the Blargg filter to convert the S16 values to RGB
// to 32 x 32 x 32 x 3 = 98,304 calculations at initialistion (otherwise would be 256x256x256=16,777,216)
#define S16_STANDARD 0x000000
#define S16_SHADOW   0x008000
#define S16_HILITE   0x010000
#define CURRENT_RGB_BLARRG_STD()    (r1 << 10) | (g1 << 5) | b1 | S16_STANDARD;
#define CURRENT_RGB_BLARRG_SHADOW() (r1 << 10) | (g1 << 5) | b1 | S16_SHADOW;
#define CURRENT_RGB_BLARRG_HILITE() (r1 << 10) | (g1 << 5) | b1 | S16_HILITE;

void RenderBase::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;

    // Standard colours
    uint32_t r = r_rgbVal[r1];
    uint32_t g = g_rgbVal[g1];
    uint32_t b = b_rgbVal[b1];
    rgb[adr] = CURRENT_RGB();
	rgb_blargg[adr] = CURRENT_RGB_BLARRG_STD();

    // Shadows
    r = r_shadowVal[r1];
    g = g_shadowVal[g1];
    b = b_shadowVal[b1];
    rgb[adr + S16_PALETTE_ENTRIES] = CURRENT_RGB();
    rgb_blargg[adr + S16_PALETTE_ENTRIES] = CURRENT_RGB_BLARRG_SHADOW();

    // Hilights
    r = r_hiliteVal[r1];
    g = g_hiliteVal[g1];
    b = b_hiliteVal[b1];
    rgb[adr + (S16_PALETTE_ENTRIES << 1)] = CURRENT_RGB();
	rgb_blargg[adr + (S16_PALETTE_ENTRIES << 1)] = CURRENT_RGB_BLARRG_HILITE();
}


void RenderBase::set_shadow_intensity(float f)
{
    shadow_multi = (int) std::round(255.0f * f);
}

void RenderBase::init_palette(int red_curve, int green_curve, int blue_curve)
// curvve and gain are 0-255 where 100 is no-change. S16 values, whilst accurate to the electrical DAC output, can be adjusted by the
// curve and gain settings to compensate for the colour output response of an LCD (which is broadly linear) being different to that of a CRT,
// which is more like a gamma curve.
{
    for (int x = 0; x < 32; x++) {
        if (red_curve != 100) {
            r_rgbVal[x] = uint32_t(pow((float(S16_rgbVal[x]) / 255.0), (float(red_curve) / 100.0)) * 255.0) & 0xff;
            r_shadowVal[x] = uint32_t(pow((float(S16_shadowVal[x]) / 255.0), (float(red_curve) / 100.0)) * 255.0) & 0xff;
            r_hiliteVal[x] = uint32_t(pow((float(S16_hiliteVal[x]) / 255.0), (float(red_curve) / 100.0)) * 255.0) & 0xff;
        }
        else {
            r_rgbVal[x] = S16_rgbVal[x];
            r_shadowVal[x] = S16_shadowVal[x];
            r_hiliteVal[x] = S16_hiliteVal[x];
        }
        if (green_curve != 100) {
            g_rgbVal[x] = uint32_t(pow((float(S16_rgbVal[x]) / 255.0), (float(green_curve) / 100.0)) * 255.0) & 0xff;
            g_shadowVal[x] = uint32_t(pow((float(S16_shadowVal[x]) / 255.0), (float(green_curve) / 100.0)) * 255.0) & 0xff;
            g_hiliteVal[x] = uint32_t(pow((float(S16_hiliteVal[x]) / 255.0), (float(green_curve) / 100.0)) * 255.0) & 0xff;
        }
        else {
            g_rgbVal[x] = S16_rgbVal[x];
            g_shadowVal[x] = S16_shadowVal[x];
            g_hiliteVal[x] = S16_hiliteVal[x];
        }
        if (blue_curve != 100) {
            b_rgbVal[x] = uint32_t(pow((float(S16_rgbVal[x]) / 255.0), (float(blue_curve) / 100.0)) * 255.0) & 0xff;
            b_shadowVal[x] = uint32_t(pow((float(S16_shadowVal[x]) / 255.0), (float(blue_curve) / 100.0)) * 255.0) & 0xff;
            b_hiliteVal[x] = uint32_t(pow((float(S16_hiliteVal[x]) / 255.0), (float(blue_curve) / 100.0)) * 255.0) & 0xff;
        }
        else {
            b_rgbVal[x] = S16_rgbVal[x];
            b_shadowVal[x] = S16_shadowVal[x];
            b_hiliteVal[x] = S16_hiliteVal[x];
        }
    }
}