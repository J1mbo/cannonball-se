#include "video.hpp"
#include "hwvideo/hwsprites.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

/***************************************************************************
    Video Emulation: OutRun Sprite Rendering Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/* This version is tuned for CannonBall-SE, modifications Copyright (c) 2025, James Pearce */

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

/*
 Reproduces glowy edge around sprites on top of shadows as seen on Hardware.
 Believed to be caused by shadowing being out by one clock cycle / pixel.

 1/ Sprites Drawn on top of Shadow clears the shadow flags for its opaque pixels.
 2/ Either the flag clear or the sprite itself is offset by one pixel horizontally.

 Thanks to Alex B. for original implementation:

#define draw_pixel()                                                                                  \
{                                                                                                     \
    if (x >= x1 && x < x2)                                                                            \
    {                                                                                                 \
        if (shadow && pix == 0xa)                                                                     \
        {                                                                                             \
            pPixel[x] &= 0xfff;                                                                       \
            pPixel[x] += S16_PALETTE_ENTRIES;                                                         \
        }                                                                                             \
        else if (pix != 0 && pix != 15)                                                               \
        {                                                                                             \
            if (x > x1) pPixel[x-1] &= 0xfff;                                                         \
            pPixel[x] = (pix | color);                                                                \
        }                                                                                             \
    }                                                                                                 \
}
*/


/*
 JJP - In CannonBall-SE, we can write safely to pDst[x-1] at all times because:

 1. The video buffers are 64 bytes bigger than we need, and the published
    pointer video.pixels is offset +64 bytes (meaning we can write to video.pixels[-1]); and

 2. Any write to the last pixel of the row before will be masked by the CRT shape overlay.

 This makes it fast enough that we no longer need to offer rendering without it -
 even the Pi 1 and Zero are stable at 30 fps (in arcade-res).
*/

#define draw_pixels() do {                                          \
    while (xacc < 0x200) {                                          \
        if (pix != 0 && pix != 15 && x >= x1 && x < x2) {           \
            if (shadow && pix == 0xA)                               \
                pDst[x] |= S16_PALETTE_ENTRIES;                     \
            else {                                                  \
                pDst[x-1] &= 0xfff;                                 \
                pDst[x]    = (pix | color);                         \
            }                                                       \
        }                                                           \
        x += xdelta; xacc += hzoom;                                 \
    }                                                               \
    xacc -= 0x200;                                                  \
} while (0)

/* JJP - the following macros are currently not used. The provide slightly faster sprite rendering */

#define draw_pixels_no_glow() do {                                  \
    while (xacc < 0x200) {                                          \
        if (pix != 0 && pix != 15 && x >= x1 && x < x2) {           \
            if (shadow && pix == 0xA)                               \
                pDst[x] |= S16_PALETTE_ENTRIES;                     \
            else                                                    \
                pDst[x] = (pix | color);                            \
        }                                                           \
        x += xdelta; xacc += hzoom;                                 \
    }                                                               \
    xacc -= 0x200;                                                  \
} while (0)

#define draw_pixels_no_shadows() do {                               \
    while (xacc < 0x200) {                                          \
        if (pix != 0 && pix != 15 && x >= x1 && x < x2) {           \
            pDst[x] = (pix | color);                                \
        }                                                           \
        x += xdelta; xacc += hzoom;                                 \
    }                                                               \
    xacc -= 0x200;                                                  \
} while (0)



void hwsprites::render(const uint8_t priority)
{
    const uint32_t numbanks  = SPRITES_LENGTH / 0x10000;
    const int scr_width      = config.s16_width;
    const int scr_height     = config.s16_height;

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 8)
    {
        // stop when we hit the end of sprite list
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri  = 1 << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        // if hidden
        int16_t hide      =  (ramBuff[data+0] & 0x5000);
        if (hide) continue;

        int16_t  bank     =  (ramBuff[data+0] >> 9) & 7;
        int32_t  top      =  (ramBuff[data+0] & 0x1ff) - 0x100;
        uint32_t addr     =   ramBuff[data+1];
        int32_t  pitch    = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        uint8_t  shadow   =  (ramBuff[data+3] >> 14) & 1;
        int32_t  vzoom    =   ramBuff[data+3] & 0x7ff;
        int32_t  ydelta   = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t  flip     = (~ramBuff[data+4] >> 14) & 1;
        int32_t  xdelta   = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t  hzoom    =   ramBuff[data+4] & 0x7ff;
        int32_t  color    = COLOR_BASE +
                            ((ramBuff[data+5] & 0x7f) << 4);
        int32_t  height   =  (ramBuff[data+5] >> 8) + 1;
        int32_t  xpos     =   ramBuff[data+6]; // moved from original structure to accomodate widescreen

        int32_t  x, y, ytarget, yacc = 0, pix;

        // adjust X coordinate
        // note: the threshhold below is a guess. If it is too high, rachero will draw garbage
        // If it is too low, smgp won't draw the bottom part of the road
        if (xpos < 0x80 && xdelta < 0)
            xpos += 0x200;
        xpos -= 0xbe;

        // clamp to within the memory region size
        if (numbanks)
            bank %= numbanks;

        const uint32_t* spritedata = sprites + 0x10000 * bank;

        // clamp to a maximum of 8x (not 100% confirmed)
        if (vzoom < 0x40) vzoom = 0x40;
        if (hzoom < 0x40) hzoom = 0x40;

        // loop from top to bottom
        ytarget = top + ydelta * height;

        // Adjust for widescreen mode
        xpos += config.s16_x_off;

        // Adjust for hi-res mode
        if (config.video.hires) {
            xpos <<= 1;
            top <<= 1;
            ytarget <<= 1;
            hzoom >>= 1;
            vzoom >>= 1;
        }

        // JJP Sprite Handling Analysis:
        // 1. In Outrun, we never seem to encounter sprites that are out of the y clip space
        // 2. 20-25% are drawn flipped.
        // 3. ~65% are drawn with xdelta < 0
        // 4. Within "for (x=xpos..." loop, less than 1% of blocks of 8 are all 0 (transparent)
        // 5. 10-15% are drawn at hzoom >= 0x200
        // 6. ~20% of drawn pixels are shadows
        // 7. ~55% are skipped by the test "if (pix != 0 && pix != 15 && x >= x1 && x < x2)"
        // 7.1 ~50% are skipped by the first test (pix != 0)

        uint16_t* RESTRICT pDst = video.pixels + top*scr_width;
        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < scr_height)
            {
                uint32_t  curAddr = addr;
                int32_t   xacc    = 0;

                // non-flipped case - approx. 75%
                if (flip == 0)
                {
                    for (x = xpos; (xdelta > 0 && x < scr_width) || (xdelta < 0 && x >= 0); )
                    {
                        uint32_t pixels = spritedata[curAddr++];
                        // draw eight pixels
                        pix = (pixels >> 28) & 0xf; draw_pixels();
                        pix = (pixels >> 24) & 0xf; draw_pixels();
                        pix = (pixels >> 20) & 0xf; draw_pixels();
                        pix = (pixels >> 16) & 0xf; draw_pixels();
                        pix = (pixels >> 12) & 0xf; draw_pixels();
                        pix = (pixels >>  8) & 0xf; draw_pixels();
                        pix = (pixels >>  4) & 0xf; draw_pixels();
                        pix = (pixels >>  0) & 0xf; draw_pixels();

                        // stop if the second-to-last pixel in the group was 0xf
                        if ((pixels & 0x000000f0) == 0x000000f0)
                            break;
                    }
                }
                // flipped case - approx. 25%
                else
                {
                    for (x = xpos; (xdelta > 0 && x < scr_width) || (xdelta < 0 && x >= 0); )
                    {
                        uint32_t pixels = spritedata[curAddr--];
                        // draw eight pixels
                        pix = (pixels >>  0) & 0xf; draw_pixels();
                        pix = (pixels >>  4) & 0xf; draw_pixels();
                        pix = (pixels >>  8) & 0xf; draw_pixels();
                        pix = (pixels >> 12) & 0xf; draw_pixels();
                        pix = (pixels >> 16) & 0xf; draw_pixels();
                        pix = (pixels >> 20) & 0xf; draw_pixels();
                        pix = (pixels >> 24) & 0xf; draw_pixels();
                        pix = (pixels >> 28) & 0xf; draw_pixels();

                        // stop if the second-to-last pixel in the group was 0xf
                        if ((pixels & 0x0f000000) == 0x0f000000)
                            break;
                    }
                }
            }
            // accumulate zoom factors; if we carry into the high bit, skip an extra row
            yacc += vzoom;
            addr += pitch * (yacc >> 9);
            yacc &= 0x1ff;
            pDst += scr_width;
        }
    }
}
