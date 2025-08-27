#include "video.hpp"
#include "hwvideo/hwsprites.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include <algorithm>
#include <iostream>

/******************************************************************************************
    Video Emulation: OutRun Sprite Rendering Hardware.
    Based on MAME source code.

    Copyright (c) Aaron Giles. All rights reserved.
    Reproduction of S16 Glowy Edges on Sprites in Shadow Copyright (c) Alex B.

    Performance optimisation for CannonBall-SE, including approximation of S16 glowy edges,
    Copyright (c) 2025, James Pearce.
*******************************************************************************************/

/*******************************************************************************************
*  Out Run/X-Board-style sprites
*
*      Offs  Bits               Usage
*       +0   e------- --------  Signify end of sprite list
*       +0   -h-h---- --------  Hide this sprite if either bit is set
*       +0   ----bbb- --------  Sprite bank
*       +0   -------t tttttttt  Top scanline of sprite + 256
*       +2   oooooooo oooooooo  Offset within selected sprite bank
*       +4   ppppppp- --------  Signed 7-bit pitch value between scanlines
*       +4   -------x xxxxxxxx  X position of sprite (position $BE is screen position 0)
*       +6   -s------ --------  Enable shadows
*       +6   --pp---- --------  Sprite priority, relative to tilemaps
*       +6   ------vv vvvvvvvv  Vertical zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +8   y------- --------  Render from top-to-bottom (1) or bottom-to-top (0) on screen
*       +8   -f------ --------  Horizontal flip: read the data backwards if set
*       +8   --x----- --------  Render from left-to-right (1) or right-to-left (0) on screen
*       +8   ------hh hhhhhhhh  Horizontal zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +E   dddddddd dddddddd  Scratch space for current address
*
*  Out Run only:
*       +A   hhhhhhhh --------  Height in scanlines - 1
*       +A   -------- -ccccccc  Sprite color palette
*
*  X-Board only:
*       +A   ----hhhh hhhhhhhh  Height in scanlines - 1
*       +C   -------- cccccccc  Sprite color palette
*
*  Final bitmap format:
*
*            -s------ --------  Shadow control
*            --pp---- --------  Sprite priority
*            ----cccc cccc----  Sprite color palette
*            -------- ----llll  4-bit pixel data
*
 *******************************************************************************************/


hwsprites::hwsprites()
{
}

hwsprites::~hwsprites()
{
}

void hwsprites::init(const uint8_t* src_sprites)
{
    reset();

    if (src_sprites)
    {
        // Convert S16 tiles to a more useable format
        const uint8_t *spr = src_sprites;

        for (uint32_t i = 0; i < SPRITES_LENGTH; i++)
        {
            uint8_t d3 = *spr++;
            uint8_t d2 = *spr++;
            uint8_t d1 = *spr++;
            uint8_t d0 = *spr++;

            sprites[i] = (d0 << 24) | (d1 << 16) | (d2 << 8) | d3;
        }
    }
}

void hwsprites::reset()
{
    // Clear Sprite RAM buffers
    for (uint16_t i = 0; i < SPRITE_RAM_SIZE; i++)
    {
        ram[i] = 0;
        ramBuff[i] = 0;
    }
}

// Clip areas of the screen in wide-screen mode
void hwsprites::set_x_clip(bool on)
{
    // Clip to central 320 width window.
    if (on)
    {
        x1 = config.s16_x_off;
        x2 = x1 + S16_WIDTH;

        if (config.video.hires)
        {
            x1 <<= 1;
            x2 <<= 1;
        }
    }
    // Allow full wide-screen.
    else
    {
        x1 = 0;
        x2 = config.s16_width;
    }
}

uint8_t hwsprites::read(const uint16_t adr)
{
    uint16_t a = adr >> 1;
    if ((adr & 1) == 1)
        return ram[a] & 0xff;
    else
        return ram[a] >> 8;
}

void hwsprites::write(const uint16_t adr, const uint16_t data)
{
    ram[adr >> 1] = data;
}

// Copy back buffer to main ram, ready for blit
void hwsprites::swap()
{
    uint16_t *src = (uint16_t *)ram;
    uint16_t *dst = (uint16_t *)ramBuff;

    // swap the halves of the road RAM
    for (uint16_t i = 0; i < SPRITE_RAM_SIZE; i++)
    {
        uint16_t temp = *src;
        *src++ = *dst;
        *dst++ = temp;
    }
}

// S16 Pixel Accuracy
// The glowy edge around sprites on top of shadows as seen on arcade hardware
// (e.g. at the base of the tree on the right at the start) is believed to be
// caused by shadowing being out by one clock cycle / pixel. Therefore:
//
// 1/ Sprites Drawn on top of Shadow clears the shadow flags for its opaque pixels.
// 2/ Either the flag clear or the sprite itself is offset by one pixel horizontally.
//
// Original reproduction in CannonBall by Alex B.
//
// Reproducing this is somewhat expensive for limited systems (like Pi2) because
// of the branchy nature of the code and the conditional write to the extra pixels.
//
// On Pi2 v1.1, this reduces frame rates achievable by about 10%. To this end, an
// approximation mode is provided (enabled when config.video.s16accuracy==0) that
// provides some of the effect but take about 80% of the draws through a fast-path.
// This work Copyright (c) 2025 James Pearce.

/*
   JJP - tuned macro based implementation (for reference)
   Adds a test for is_shadow or is_draw since over 50% are skipped at this test

#define draw_pixels() do {                                      \
    int is_shadow = (shadow && pix == 0xA);                     \
    int is_draw   = (pix != 0 && pix != 15);                    \
    if (is_shadow || is_draw) {                                 \
        while (xacc < 0x200) {                                  \
            if ((unsigned)(x - x1) < (unsigned)(clipWidth)) {   \
                if (is_shadow) {                                \
                    *(pDst) |= S16_PALETTE_ENTRIES;             \
                } else {                                        \
                    if (x > x1) pDst[-1] &= 0xfff;              \
                    *(pDst) = (pix | color);                    \
                }                                               \
            }                                                   \
            x += xdelta;                                        \
            pDst += xdelta;                                     \
            xacc += hzoom;                                      \
        }                                                       \
    } else {                                                    \
        while (xacc < 0x200) {                                  \
            x += xdelta;                                        \
            pDst += xdelta;                                     \
            xacc += hzoom;                                      \
        }                                                       \
    }                                                           \
} while (0)
*/


// Inline based approach.
// draw_nibble_shadow_aware() draws one 4-bit nibble and produces output
// identical to the macro above.
static inline __attribute__((always_inline))
void draw_nibble_shadow_aware(
    uint8_t pix,
    uint8_t shadow,          // 0/1, as in your code
    int     color,           // palette/color bits as in your code
    int     x1,              // clip left
    int     clipWidth,       // x2 - x1
    int     xdelta,          // +1 or -1
    int     hzoom,           // horizontal zoom factor
    int&    x,               // current x (updated)
    int&    xacc,            // accumulator (updated)
    uint16_t* __restrict__& pDst          // dest pointer (updated)
)
{
    const bool is_shadow = (shadow && pix == 0xA);
    const bool is_draw   = (pix != 0 && pix != 15);

    // Transparent nibbles are common; make the skip path likely.
    if (__builtin_expect(!(is_shadow || is_draw), 1)) {
        // Transparent nibble: advance and break adjacency so the next opaque run will left-clear.
        while (xacc < 0x200) {
            x += xdelta;  pDst += xdelta;  xacc += hzoom;
        }
        return;
    }

    while (xacc < 0x200) {
        if ((unsigned)(x - x1) < (unsigned)clipWidth) {
            if (is_shadow) {
                // set shadow bit
                *pDst |= S16_PALETTE_ENTRIES;
            } else {
                // opaque draw
                if (x > x1) pDst[-1] &= 0xfff;
                *(pDst) = (pix | color);
            }
        }
        x += xdelta;  pDst += xdelta;  xacc += hzoom;
    }
}


// draw_nibble_no_shadows() drops the whole shadow test so we're only testing against clip-
// width. Hence, there is no check for shadows (this is moved to the outer loop in
// hwsprites::render(), below), and no glowy-edges produced by this path.
//
// We can however obtain correct shadows and an approximation of S16 glowy edges by:
//
// 1. Calling this only for pixel blocks where NONE of the 4-bit pixels are shadow, and
// 2. Always calling draw_nibble_shadow_aware() for the FIRST processed nibble.
//
// This approach does not produce the same output, but reduces traffic through the complex
// (shadow_aware) path by around 80%, offering Pi2 about 10% frame-rate improvement.
//
// The approximation happens because the FIRST processeed nibble of a block is not necessarily
// the first *drawn* pixel, we in effect miss the over-write of the underlying pixel (to remove
// it's shadow bit). This can be seen on the start-line, where the right hand tree looks correct
// but the man with the red shirt near it doesn't have a glow around his shirt.
//
static inline __attribute__((always_inline))
void draw_nibble_no_shadows(
    uint8_t pix,
    int     color,           // palette/color bits as in your code
    int     x1,              // clip left
    int     clipWidth,       // x2 - x1
    int     xdelta,          // +1 or -1
    int     hzoom,           // horizontal zoom factor
    int&    x,               // current x (updated)
    int&    xacc,            // accumulator (updated)
    uint16_t* __restrict__& pDst          // dest pointer (updated)
)
{
    const bool is_draw   = (pix != 0 && pix != 15);

    if (__builtin_expect(!(is_draw), 1)) {
//    if (!(is_draw)) {
        // Transparent nibble: advance and break adjacency so the next opaque run will left-clear.
        while (xacc < 0x200) {
            x += xdelta;  pDst += xdelta;  xacc += hzoom;
        }
        return;
    }

    while (xacc < 0x200) {
        if ((unsigned)(x - x1) < (unsigned)clipWidth) {
            // Opaque draw
            *pDst = uint16_t(pix | color);
        }
        x += xdelta;  pDst += xdelta;  xacc += hzoom;
    }
}


static inline __attribute__((always_inline))
bool any_shadow_nibble(uint32_t w) {
    // Returns true if any nibble has the value 0xA (in which case the slower rendering path is
    // needed to correctly apply shadows).
    // XOR makes nibbles that equal 0xA become 0.
    uint32_t t = w ^ 0xAAAAAAAAu;
    // Detect any zero nibble: (t - 0x1111..) sets a borrow into the nibble's MSB
    // only when that nibble was zero; ~t masks out non-zero nibbles; 0x8888.. selects those MSBs.
    return ((t - 0x11111111u) & ~t & 0x88888888u) != 0;
}



void hwsprites::render(const uint8_t priority)
{
    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;
    const int clipWidth     = x2 - x1;
    const int scr_width     = config.s16_width;
    const int scr_height    = config.s16_height;

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 8)
    {
        // stop when we hit the end of sprite list
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri  = 1 << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        // if hidden, or top greater than/equal to bottom, or invalid bank, punt
        int16_t hide    = (ramBuff[data+0] & 0x5000);
        int32_t height  = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        int16_t bank    = (ramBuff[data+0] >> 9) & 7;
        int32_t top     = (ramBuff[data+0] & 0x1ff) - 0x100;
        uint32_t addr   = ramBuff[data+1];
        int32_t pitch   = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        int32_t xpos    =  ramBuff[data+6]; // moved from original structure to accomodate widescreen
        uint8_t shadow  = (ramBuff[data+3] >> 14) & 1;
        int32_t vzoom   = ramBuff[data+3] & 0x7ff;
        int32_t ydelta  = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t flip    = (~ramBuff[data+4] >> 14) & 1;
        int32_t xdelta  = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t hzoom   = ramBuff[data+4] & 0x7ff;
        int32_t color   = COLOR_BASE + ((ramBuff[data+5] & 0x7f) << 4);
        int32_t x, y, ytarget;
        int32_t yacc = 0;
        // adjust X coordinate
        // note: the threshhold below is a guess. If it is too high, rachero will draw garbage
        // If it is too low, smgp won't draw the bottom part of the road
        if (xpos < 0x80 && xdelta < 0)
            xpos += 0x200;
        xpos -= 0xbe;

        // initialize the end address to the start address
        ramBuff[data+7] = addr;

        // clamp to within the memory region size
        if (numbanks)
            bank %= numbanks;

        const uint32_t* spritedata = sprites + 0x10000 * bank;

        // the sprite render can be made significantly faster by using an approximation to
        // the S16 emulation whereby glowy edges around objects in shadows is less accurately reproduced.
        const int consider_shadow  = (config.video.s16accuracy ? shadow : 0);

        // clamp to a maximum of 8x (not 100% confirmed)
        if (vzoom < 0x40) vzoom = 0x40;
        if (hzoom < 0x40) hzoom = 0x40;

        // loop from top to bottom
        ytarget = top + ydelta * height;

        // Adjust for widescreen mode
        xpos += config.s16_x_off;

        // Adjust for hi-res mode
        if (config.video.hires)
        {
            xpos <<= 1;
            top <<= 1;
            ytarget <<= 1;
            hzoom >>= 1;
            vzoom >>= 1;
        }

        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < scr_height)
            {
                uint16_t* pDst = video.pixels + y*scr_width + xpos;
                uint32_t curAddr = addr;
                int xacc = 0;

                // non-flipped case
                if (flip == 0) {
                    for (x = xpos; (xdelta > 0 && x < scr_width) || (xdelta < 0 && x >= 0); ) {
                        // Prefetch future sprite words so 'pixels' is warm next iterations
                        __builtin_prefetch(spritedata + curAddr + 8,  0, 0);
                        uint32_t pixels = spritedata[curAddr++];
                        if (consider_shadow || any_shadow_nibble(pixels)) {
                            draw_nibble_shadow_aware(uint8_t( pixels >> 28       ), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 24) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 20) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 16) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 12) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >>  8) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >>  4) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t( pixels        & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                        } else {
                            draw_nibble_shadow_aware(uint8_t( pixels >> 28), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 24) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 20) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 16) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 12) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >>  8) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >>  4) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t( pixels        & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                        }
                        // stop if the second-to-last pixel in the group was 0xf
                        if (__builtin_expect((pixels & 0x000000f0) == 0x000000f0, 0)) break;
                    }
                } else {
                    // flipped case
                    for (x = xpos; (xdelta > 0 && x < scr_width) || (xdelta < 0 && x >= 0); ) {
                        // Prefetch future sprite words so 'pixels' is warm next iterations
                        __builtin_prefetch(spritedata + curAddr - 8,  0, 0);
                        uint32_t pixels = spritedata[curAddr--];
                        if (consider_shadow || any_shadow_nibble(pixels)) {
                            draw_nibble_shadow_aware(uint8_t( pixels        & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >>  4) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >>  8) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 12) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 16) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 20) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t((pixels >> 24) & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_shadow_aware(uint8_t( pixels >> 28       ), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                        } else {
                            draw_nibble_shadow_aware(uint8_t( pixels & 0xF), shadow, color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >>  4) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >>  8) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 12) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 16) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 20) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t((pixels >> 24) & 0xF), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                            draw_nibble_no_shadows(uint8_t( pixels >> 28       ), color, x1, clipWidth, xdelta, hzoom, x, xacc, pDst); xacc -= 0x200;
                        }
                        // stop if the second-to-last pixel in the group was 0xf
                        if (__builtin_expect((pixels & 0x0f000000) == 0x0f000000, 0)) break;
                    }
                }
            }
            // accumulate zoom factors; if we carry into the high bit, skip an extra row
            yacc += vzoom;
            addr += pitch * (yacc >> 9);
            yacc &= 0x1ff;
        }
    }
}
