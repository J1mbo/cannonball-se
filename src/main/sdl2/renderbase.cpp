#include "renderbase.hpp"
#include <iostream>


RenderBase::RenderBase()
{
    surface       = NULL;
    screen_pixels = NULL;

    orig_width  = 0;
    orig_height = 0;
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

const uint32_t rgbVal[32] = {
  0,8,16,24,31,39,47,55,62,70,78,86,94,102,109,117,125,133,140,148,156,164,171,179,187,195,203,211,
  218,226,234,242};

const uint32_t shadowVal[32] = {
  0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120,126,130,136,140,146,
  150,156};

const uint32_t hiliteVal[32] = {
  91,96,101,106,111,116,121,126,131,137,141,147,151,157,161,167,172,177,182,187,192,197,202,207,212,
  217,222,227,232,237,242,247};


// See: SDL_PixelFormat
#define CURRENT_RGB() (r << Rshift) | (g << Gshift) | (b << Bshift);

/*
void RenderBase::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;

    uint32_t r = r1 * 8;
    uint32_t g = g1 * 8;
    uint32_t b = b1 * 8;

    rgb[adr] = CURRENT_RGB();

    // Create shadow colours at end of RGB array
    r = r1 * shadow_multi / 31;
    g = g1 * shadow_multi / 31;
    b = b1 * shadow_multi / 31;
        
    rgb[adr + S16_PALETTE_ENTRIES] = CURRENT_RGB(); // Add to the end of the array

    // Highlight colour code would be added here, but unused.
}
*/

void RenderBase::convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1)
{
    adr >>= 1;

    // Standard colours
    uint32_t r = rgbVal[r1];
    uint32_t g = rgbVal[g1];
    uint32_t b = rgbVal[b1];
    rgb[adr] = CURRENT_RGB();

    // Shadows
    r = shadowVal[r1];
    g = shadowVal[g1];
    b = shadowVal[b1];
//    r = r / 2; g = g / 2; b = b / 2;
    rgb[adr + S16_PALETTE_ENTRIES] = CURRENT_RGB();

    // Hilights
//    r = shadowVal[r1];
//    g = shadowVal[g1];
//    b = shadowVal[b1];
    r = hiliteVal[r1];
    g = hiliteVal[g1];
    b = hiliteVal[b1];
    rgb[adr + (S16_PALETTE_ENTRIES << 1)] = CURRENT_RGB();
}


void RenderBase::set_shadow_intensity(float f)
{
    shadow_multi = (int) std::round(255.0f * f);
}
