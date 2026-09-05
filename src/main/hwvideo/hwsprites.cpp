#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwarch.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"
#include <cstring>
#include <algorithm>
#include <iterator>

/***************************************************************************
    Video Emulation: OutRun Sprite Rendering Hardware.
    Based on MAME source code.

    Copyright Aaron Giles.
    All rights reserved.
***************************************************************************/

/* This version is tuned for CannonBall-SE, modifications Copyright (c) 2025, James Pearce
   Approx. 40% speed-up on Pi2 */

/*******************************************************************************************
*  Out Run/X-Board-style sprites
*
*      Offs  Bits               Usage
*       +0   e------- --------  Signify end of sprite list
*       +0   -h-h---- --------  Hide this sprite if either bit is set
*       +0   --c----- --------  JJP - clip required (partially off-screen)
*       +0   ----bbb- --------  Sprite bank
*       +0   -------t tttttttt  Top scanline of sprite + 256
*       +2   oooooooo oooooooo  Offset within selected sprite bank
*       +4   ppppppp- --------  Signed 7-bit pitch value between scanlines
*       +4   -------x xxxxxxxx  X position of sprite (position $BE is screen position 0)
*       +6   -s------ --------  Enable shadows
*       +6   --pp---- --------  Sprite priority, relative to tilemaps
*       +6   ----vvvv vvvvvvvv  Zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size) - JJP - this is wrong, 0x100 is 2x size
*       +8   y------- --------  Render from top-to-bottom (1) or bottom-to-top (0) on screen
*       +8   -f------ --------  Horizontal flip: read the data backwards if set
*       +8   --x----- --------  Render from left-to-right (1) or right-to-left (0) on screen
//*       +8   ------hh hhhhhhhh  Horizontal zoom factor (0x200 = full size, 0x100 = half size, 0x300 = 2x size)
*       +8   ------ww wwwwwwww  Sprite width on-screen (in native res)
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

// SPRITE_IMPLEMENTATION_SPEC.md S0.2: the CMake option PIXEL_ACCURACY
// (CMakeLists.txt, default ON => -DPIXEL_ACCURACY=1) used to have no effect
// on this file at all: an unconditional "#define PIXEL_ACCURACY 0" used to
// sit here and always override it before the macro table further down was
// ever reached, so hardware pixel accuracy (glow) has always shipped
// disabled regardless of the CMake setting. Fixed to a default that a
// caller-supplied definition can still override. The macro table used by
// render()'s dispatch switch below no longer depends on this value at all
// (see the note there), so render()'s compiled behaviour is unchanged by
// this fix; only the initial value of the new runtime `glow` member (S0.5,
// not yet consulted by render() itself) is affected.
#ifndef PIXEL_ACCURACY
#define PIXEL_ACCURACY 1
#endif

hwsprites::hwsprites() : glow(PIXEL_ACCURACY != 0)
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

            // Forward (just endian swap of bytes, keep pixel order p0..p7)
            sprites[i] = ((uint32_t)d0 << 24) |
                         ((uint32_t)d1 << 16) |
                         ((uint32_t)d2 <<  8) |
                         ((uint32_t)d3 <<  0);
        }
    }
}


void hwsprites::reset()
{
    // Clear Sprite RAM buffers
    std::fill_n(ram,                 std::size(ram),                0x0);
    std::fill_n(ramBuff,             std::size(ramBuff),            0x0);
    std::fill_n(sprites_flipped,     std::size(sprites_flipped),    0xffffffff);
    std::fill_n(sprites_shadowinfo,  std::size(sprites_shadowinfo), 0xff);
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

// S0.5: unconditional test/inspection accessor. Copies exactly what
// hwsprites::swap() leaves ramBuff holding (SPRITE_RAM_SIZE == 0x800
// halfwords) -- used by the driver's `replay` to load a captured frame's
// sprite RAM without needing friend access.
void hwsprites::load_sprite_ram(const uint16_t* buf)
{
    std::memcpy(ramBuff, buf, sizeof(ramBuff));
}


// -----------------------------------------------------------------------------------------
// SPRITE_IMPLEMENTATION_SPEC.md S0.2: this table used to be split
// "#if PIXEL_ACCURACY / #else" between a single glow-capable macro
// (draw_pixel(), which -- grep confirms -- was never actually invoked
// anywhere in render()'s dispatch switch below) and the twelve macros the
// switch really uses. Since only the twelve are ever called, that split made
// PIXEL_ACCURACY==1 fail to compile (draw_pixel_1row() etc. would not exist)
// -- collateral damage of the PIXEL_ACCURACY fix above, now corrected. The
// twelve macros are unconditional, with byte-identical bodies to what
// render() has always compiled against, so render()'s output is unchanged
// for any value of PIXEL_ACCURACY. draw_pixel() itself (the dead glow macro)
// is not carried forward; the hardware glow behaviour it once sketched --
// "a sprite drawn on top of a shadow clears the shadow flag for its opaque
// pixels, offset by one pixel horizontally" -- is now the definition
// implemented and tested by draw_sprite_px's glow branch below (S0.4).
// -----------------------------------------------------------------------------------------

// MACROS WITH SHADOW SUPPORT

#define IF_IS_DRAWABLE_()        if (x >= x1 && x < x2 && pix != 0 && pix != 15)
#define IF_IS_DRAWABLE()         if (((unsigned)(pix - 1u) < 14u) && ((unsigned)(x - x1) < span))
#define IS_DRAWABLE()               (((unsigned)(pix - 1u) < 14u) && ((unsigned)(x - x1) < span))
#define IF_IS_DRAWABLE_NO_CLIP() if  ((unsigned)(pix - 1u) < 14u)
#define IS_DRAWABLE_NO_CLIP()        ((unsigned)(pix - 1u) < 14u)

#define draw_pixel_1row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

// MACROS WITHOUT SHADOW SUPPORT

#define draw_pixel_1row_ns()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_ns()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_ns()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc_ns()                                 \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc_ns()                                 \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc_ns()                                 \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}



void hwsprites::render(uint16_t* pixels, const uint8_t priority)
{
    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 16) // was +=8
    {
        // stop when we hit the end of sprite list
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri  = 1 << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        // if hidden, or top greater than/equal to bottom, or invalid bank, punt
        int16_t hide    = (ramBuff[data+0] & 0x5000);
        int32_t height  = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        int16_t bank      =  (ramBuff[data+0] >> 9) & 7;
        int32_t top       =  (ramBuff[data+0] & 0x1ff) - 0x100;
        bool     clip     =  (ramBuff[data+0] & 0x2000);
        uint32_t addr     =   ramBuff[data+1];
        int32_t pitch     = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        uint8_t shadow    =  (ramBuff[data+3] >> 14) & 1;
//        int32_t  zoom     =   ramBuff[data+3] & 0x7ff;
        int32_t  zoom     =   ramBuff[data+3] & 0x0fff;
        int32_t ydelta    = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t flip      = (~ramBuff[data+4] >> 14) & 1;
        int32_t xdelta    = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
//        int32_t hzoom     =   ramBuff[data+4] & 0x7ff;
        int32_t width     =   ramBuff[data+4] & 0x7ff;
        int32_t color     = COLOR_BASE +
                            ((ramBuff[data+5] & 0x7f) << 4);
        int32_t sprite_scanlines =
                            ((ramBuff[data+5] & 0xff00) >> 16) +1;
        int32_t xpos      =   ramBuff[data+6]; // moved from original structure to accomodate widescreen
        int32_t rawh      =   ramBuff[data+7]; // JJP - height of sprite in rows
        int32_t y, ytarget, yacc = 0;

        int16_t offset    =   ramBuff[data+15]; // JJP - provides horizontal offset for hires sprite rendering
        // adjust X coordinate
        // note: the threshhold below is a guess. If it is too high, rachero will draw garbage
        // If it is too low, smgp won't draw the bottom part of the road
        if (xpos < 0x80 && xdelta < 0)
            xpos += 0x200;
        xpos -= 0xbe;

 //       // initialize the end address to the start address
 //       ramBuff[data+7] = addr;

        // clamp to within the memory region size
        if (numbanks)
            bank %= numbanks;

        const uint32_t* spriterom = sprites + 0x10000 * bank;
        uint32_t* spriterom_flipped = sprites_flipped + 0x10000 * bank;
        uint8_t* spriterom_shadowinfo = sprites_shadowinfo + 0x10000 * bank;

        // loop from top to bottom
        ytarget = top + ydelta * height;

        // Adjust for widescreen mode
        xpos += config.s16_x_off;

        // Adjust for hi-res mode
        if (config.video.hires) {
            xpos <<= 1;
            top <<= 1;
            ytarget <<= 1;
            zoom >>= 1;
        }

        // JJP - maintain converted sprites to aid writing them
        uint32_t sprite_height = 0;
        // Determine underlying sprite height
        int32_t steps = ytarget - top;  // true as ydelta is always + or - 1.
        // Use 64-bit multiply to avoid overflow
        sprite_height = ((uint64_t)steps * zoom) >> 9;

//std::cout << "\rSprite height: " << height << ", Calculated Height: " << sprite_height << ", raw height: " << rawh << "\n";

/* The above replaces...
        uint32_t yacc_t = 0;
        for (int y = top; y != ytarget; y += ydelta) {
            yacc_t += zoom;
            sprite_height += (yacc_t >> 9);
            yacc_t &= 0x1ff;
        }
*/
        static uint32_t converted_sprites = 0;
        static uint32_t processed_lines = 0;

        if (xdelta == -1) {
            xdelta = 1;       // always draw left-to-right
            xpos  -= (width << (config.video.hires ? 1 : 0));   // move draw position left by rendered width
            if (flip) {
                flip = 0;
                addr -= (pitch - 1);
            } else {
                flip = 1;
                addr += (pitch - 1);
            }
        }

        uint32_t shadowaddr = addr;

        if (flip) {
/*
            converted_sprites++;
            if (converted_sprites==1024) {
                write_bytes_u32("sprites_flipped.bin", sprites_flipped, SPRITES_LENGTH);
                write_bytes_u8("sprites_shadowinfo.bin", sprites_shadowinfo, SPRITES_LENGTH);
                converted_sprites=0;
            }
*/
            // first encounter of sprite, created flipped entry
            shadowaddr -= (pitch - 1); // start of data, if unflipped

            for (int y = 0; y < sprite_height; y++) {
//            for (int y = 0; y < height; y++) {
                // calculate the addresses for this line
                uint32_t readaddr     = shadowaddr;
                uint32_t writeaddr    = shadowaddr + pitch;
                uint32_t shadow_found = 0;
                if (spriterom_flipped[readaddr] == 0xffffffff) {
/*
processed_lines++;
std::cout << "\r\t\t\t\t" << processed_lines << " sprite lines flipped";
*/
                    // first time we've seen this row
                    for (int x = 0; x < pitch; x++) {
                        uint32_t pixels = spriterom[readaddr++];
                        uint8_t  px[8];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0)
                            shadow_found = 0x11;
                        // process eight pixels
                        px[0] = (pixels >> 28) & 0xf;
                        px[1] = (pixels >> 24) & 0xf;
                        px[2] = (pixels >> 20) & 0xf;
                        px[3] = (pixels >> 16) & 0xf;
                        px[4] = (pixels >> 12) & 0xf;
                        px[5] = (pixels >>  8) & 0xf;
                        px[6] = (pixels >>  4) & 0xf;
                        px[7] = (pixels >>  0) & 0xf;

                        spriterom_flipped[--writeaddr] =
                            (px[0] <<  0) |
                            (px[1] <<  4) |
                            (px[2] <<  8) |
                            (px[3] << 12) |
                            (px[4] << 16) |
                            (px[5] << 20) |
                            (px[6] << 24) |
                            (px[7] << 28);
                    };
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        } else {
            // not flipped
            for (int y = 0; y < sprite_height; y++) {
                if (spriterom_shadowinfo[shadowaddr] == 0xff) {
                    // first time seeing this sprite line
                    uint32_t readaddr = shadowaddr;
                    uint32_t shadow_found = 0;
                    for (int i = 0; i < pitch; ++i) {
                        uint32_t pixels = spriterom[readaddr++];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0) {
                            shadow_found = 0x11;
                            break;
                        }
                    }
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        }

        // adjust x-position with pre-determined offset for hi-res sprite rendering
        if (config.video.hiresprites == 1)
            xpos += offset;

        // choose which ROM to read from - flipped or non-flipped
        const uint32_t* spritedata;
        if (flip) {
            addr = addr - pitch + 1; // set pointer to start of sprite row
            spritedata = &spriterom_flipped[0];
        } else {
            spritedata = &spriterom[0];
        }

        const uint16_t scrn_width = config.s16_width;
        const unsigned span = (unsigned)(x2 - x1);

        uint32_t jump_key = 3;
        // drawing loop
        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < config.s16_height)
            {
                uint16_t* pPix1 = pixels + ((y+0) * scrn_width) + xpos;
                uint32_t spriteaddr = addr;
                int32_t xacc = 0;

                // determine how many rows will be written on this pass
                uint16_t count = 1;
                uint32_t frac = yacc + zoom;
                uint16_t countmax = std::min( (uint16_t)(ytarget - y),
                                              std::min<uint16_t>(3, config.s16_height - y));
                while (frac < 0x200 && count < countmax) {
                    frac += zoom;
                    count++;
                }
                // count now contains a value between 1 and 3.
                // we can also use (count-1) as the *minimum* number of pixels that will be written out
                // but each x-pass, since hzoom=vzoom.

                bool shadowfound = shadow && (spriterom_shadowinfo[addr] == 0x11);

                // the following should compile to a jump table, making it much quicker to get to the
                // optimised drawing path we need without wading through layers of if/else etc

                count = (count - 1) & 3; // 0..3
                //uint32_t jump_key = //((xdelta>0)     ? 0u : 1u)   /* draw direction */
                jump_key =   (count                   ) | /* output rows, 1-4, selected via 0-3 */
                             ((clip)         ? 0u : 4u) | /* sprite requires x-clip */
                             ((shadowfound)  ? 8u : 0u); /* sprite has shadows */

                // Note - the proportion of sprite *lines* following the (shadowfound) path (jump_key>7) is
                // low, approx 10%, however the cost of rendering them is much, much higher.

//std::cout << "Clip: " << clip << ", Count: " << count << ", flip: " << flip << ", jump_key: " << jump_key << std::endl;

                switch (jump_key) {

                    case 0:
                    {
                        // no shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 1:
                    {
                        // no shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        // accumulate extra zoom factor
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 2:
                    {
                        // no shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        // accumulate extra zoom factors
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 3: break; // count==4 - not used as <1%
                    case 4:
                    {
                        // no shadows, not clipped, count==1, not flipped
                        //for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 5:
                    {
                        // no shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        // accumulate extra zoom factor
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 6:
                    {
                        // no shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc_ns();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc_ns();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        // accumulate extra zoom factors
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 7: break; // count==4 - not used as <1%
                    case 8:
                    {
                        // shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 9:
                    {
                        // shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        // accumulate extra zoom factor
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 10:
                    {
                        // shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        // accumulate extra zoom factors
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 11: break; // count==4 - not used as <1%
                    case 12:
                    {
                        // shadows, not clipped, count==1, not flipped
                        //for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 13:
                    {
                        // shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        // accumulate extra zoom factor
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 14:
                    {
                        // shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        // accumulate extra zoom factors
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 15: break; // count==4 - not used as <1%
                }
            }
            // accumulate zoom factors; if we carry into the high bit, skip an extra row
            yacc += zoom;
            addr += pitch * (yacc >> 9);
            yacc &= 0x1ff;
        }
    }
}


// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S0.4: nibrev, parse_record, draw_sprite_px, render_px.
//
// Compiled always (S0.4): render_px is also the in-game fallback for
// out-of-envelope sprites from S1.2 onward, so it (and everything it needs)
// lives here unconditionally rather than under CB_RENDER_TEST. It is a
// straightforward, obviously-correct reimplementation that draws one output
// pixel at a time from the source using the documented S16 semantics --
// deliberately not sharing any code or lazy state with render() above, so it
// serves as an independent correctness oracle: glow=false reproduces the
// shipped output modulo F1/F2 (SPRITE_IMPLEMENTATION_SPEC.md preamble items
// 1 and 6); glow=true is the PIXEL_ACCURACY==1 hardware-accurate output.
// =============================================================================================

uint32_t hwsprites::nibrev(uint32_t w)
{
    return cb_bswap32(((w >> 4) & 0x0F0F0F0Fu) | ((w << 4) & 0xF0F0F0F0u));
}

// Verbatim (S0.4) from hwsprites.cpp:425-462,468-469,476-487,509-519,601-611
// of the shipped render() above, re-expressed as a standalone parse with no
// side effects on the lazy tables. Returns false to skip this record (wrong
// priority, hidden, or zoom == 0 -- the shipped renderer would loop forever
// on zoom == 0). The end-of-list test (ramBuff[d+0] & 0x8000) is the
// caller's job, exactly as render()'s own dispatch loop does it.
bool hwsprites::parse_record(uint16_t d, uint8_t priority, SpriteRec& r) const
{
    r.sprpri = 1u << ((ramBuff[d+3] >> 12) & 3);
    if (r.sprpri != priority) return false;

    int16_t hide = (int16_t)(ramBuff[d+0] & 0x5000);
    r.height = (ramBuff[d+5] >> 8) + 1;
    if (hide != 0) return false;

    // bank is 3 bits (0-7); SPRITES_LENGTH is always exactly 4 banks of
    // 0x10000, so "% 4" (SPRITE_IMPLEMENTATION_SPEC.md S0.4) and
    // "% (SPRITES_LENGTH / 0x10000)" (render()'s numbanks) are the same
    // operation -- spelled via the class constant so the two can never drift.
    r.bank   = ((ramBuff[d+0] >> 9) & 7) % (SPRITES_LENGTH / 0x10000);
    r.top    = (int32_t)(ramBuff[d+0] & 0x1ff) - 0x100;
    r.clip   = (ramBuff[d+0] & 0x2000) != 0;
    r.addr   = (int32_t)ramBuff[d+1];
    r.pitch  = ((ramBuff[d+2] >> 1) | ((ramBuff[d+4] & 0x1000) << 3)) >> 8;
    r.shadow = (ramBuff[d+3] >> 14) & 1;
    r.zoom   = ramBuff[d+3] & 0xfff;
    r.ydelta = (ramBuff[d+4] & 0x8000) ? 1 : -1;
    r.flip   = (~ramBuff[d+4] >> 14) & 1;
    r.xdelta = (ramBuff[d+4] & 0x2000) ? 1 : -1;
    r.width  = ramBuff[d+4] & 0x7ff;
    r.color  = 0x800 + ((ramBuff[d+5] & 0x7f) << 4);
    r.xpos   = ramBuff[d+6];
    r.offset = (int16_t)ramBuff[d+15];

    if (r.xpos < 0x80 && r.xdelta < 0)
        r.xpos += 0x200;
    r.xpos -= 0xbe;

    r.ytarget = r.top + r.ydelta * r.height;
    r.xpos += config.s16_x_off;

    if (config.video.hires) {
        r.xpos <<= 1;
        r.top <<= 1;
        r.ytarget <<= 1;
        r.zoom >>= 1;
    }

    r.rtl = (r.xdelta == -1);
    if (r.rtl) {
        r.xpos -= (r.width << (config.video.hires ? 1 : 0));
        if (r.flip) {
            r.flip = 0;
            r.addr -= (r.pitch - 1);
        } else {
            r.flip = 1;
            r.addr += (r.pitch - 1);
        }
    }

    if (config.video.hiresprites == 1)
        r.xpos += r.offset;

    if (r.flip)
        r.addr = r.addr - r.pitch + 1;

    r.H = r.ytarget - r.top;
    if (r.zoom == 0) return false;

    return true;
}

// One sprite's worth of render_px (S0.4): expand each visited source row
// into ex[] (output-order nibbles), then apply in hardware order. Expansion
// has no feedback into drawing, so "expand then apply" equals the
// interleaved upstream form. Rows outside [0, s16_height) are skipped (no
// ROM read, no drawing) but the row accumulator still advances for them,
// exactly as render()'s unconditional per-iteration yacc/addr update does.
void hwsprites::draw_sprite_px(const SpriteRec& r, uint16_t* pixels, bool glow)
{
    const int32_t pend = x2 - r.xpos;
    if (pend <= 0) return;

    const int32_t nsteps = (r.ydelta > 0) ? r.H : -r.H;
    if (nsteps <= 0) return;

    uint8_t ex[SPR_EX_LEN];
    int64_t row_addr = r.addr;
    uint32_t yacc = 0;
    const int32_t scrn_width = config.s16_width;

    for (int32_t d = 0; d < nsteps; d++) {
        const int32_t y = r.top + r.ydelta * d;
        if (y >= 0 && y < config.s16_height) {
            // ---- expand this source row into ex[] (output-order nibbles) ----
            int32_t p = 0;
            int32_t xacc = 0;
            int32_t wk = 0;
            for (;;) {
                if (p >= pend || wk == SPR_WMAX) break;
                const int64_t widx = (int64_t)r.bank * 0x10000 + row_addr +
                                      (r.flip ? (int64_t)(r.pitch - 1 - wk) : (int64_t)wk);
                if (widx < 0 || widx >= (int64_t)SPRITES_LENGTH) break;
                uint32_t w = sprites[widx];
                if (r.flip) w = nibrev(w);

                for (int nb = 0; nb < 8; nb++) {
                    const unsigned n = (w >> (28 - 4 * nb)) & 0xF;
                    while (xacc < 0x200 && p < SPR_EX_LEN) {
                        ex[p++] = (uint8_t)n;
                        xacc += r.zoom;
                    }
                    xacc -= 0x200;
                }
                wk++;
                if ((w & 0xF0) == 0xF0) break;
            }

            // ---- apply in hardware draw order ----
            uint16_t* row = pixels + (size_t)y * scrn_width;
            if (!r.rtl) {
                for (int32_t q = 0; q < p; q++) {
                    const int32_t x = r.xpos + q;
                    if (x < x1 || x >= x2) continue;
                    const unsigned n = ex[q];
                    if (r.shadow && n == 0xA) {
                        row[x] |= S16_PALETTE_ENTRIES;
                    } else if (n != 0 && n != 15) {
                        if (glow && x > x1) row[x - 1] &= 0xfff;
                        row[x] = (uint16_t)(n | r.color);
                    }
                }
            } else {
                for (int32_t q = p - 1; q >= 0; q--) {
                    const int32_t x = r.xpos + q;
                    if (x < x1 || x >= x2) continue;
                    const unsigned n = ex[q];
                    if (r.shadow && n == 0xA) {
                        row[x] |= S16_PALETTE_ENTRIES;
                    } else if (n != 0 && n != 15) {
                        if (glow && x > x1) row[x - 1] &= 0xfff;
                        row[x] = (uint16_t)(n | r.color);
                    }
                }
            }
        }

        // advance the row accumulator regardless of whether this row was drawn
        yacc += r.zoom;
        row_addr += r.pitch * (int64_t)(yacc >> 9);
        yacc &= 0x1ff;
    }
}

void hwsprites::render_px(uint16_t* pixels, uint8_t priority, bool glow)
{
    for (uint16_t d = 0; d < SPRITE_RAM_SIZE; d += 16) {
        if ((ramBuff[d + 0] & 0x8000) != 0) break;
        SpriteRec rec;
        if (!parse_record(d, priority, rec)) continue;
        draw_sprite_px(rec, pixels, glow);
    }
}


#ifdef CB_RENDER_TEST
// =============================================================================================
// SPRITE_IMPLEMENTATION_SPEC.md S0.3: render_ref, a verbatim twin of the
// shipped render() above, with exactly one behavioural switch permitted
// (ref_prepass_fix, the F1 fix at the two pre-pass loop bounds below).
// Everything else -- variable names, control flow, comments -- is copied
// unchanged, except the dead instrumentation the spec calls out for
// deletion (reps/freq/the two high_resolution_clock::now() calls/
// draw[jump_key]+=/the trailing commented-out debug block), which render()
// itself no longer has either (S0.5). The lazy side tables and the twelve
// draw_pixel_* macros are duplicated (sprites_flipped_ref/
// sprites_shadowinfo_ref, the "_ref" macro suffixes) so this twin can be
// driven through its own independent session history without contaminating
// -- or being contaminated by -- the shipped render()'s tables.
// =============================================================================================

void hwsprites::reset_ref_tables()
{
    std::fill_n(sprites_flipped_ref,    std::size(sprites_flipped_ref),    0xffffffff);
    std::fill_n(sprites_shadowinfo_ref, std::size(sprites_shadowinfo_ref), 0xff);
}

#define draw_pixel_1row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_ref()                                   \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (pix == 0xa) {                                   \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_ns_ref()                                \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        x++; xacc += zoom;                                      \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
        }                                                       \
        pPix1++;                                                \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++;                                       \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc_ns_ref()                              \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            *pPix1 = (pix | color);                             \
            *pPix2 = (pix | color);                             \
            *pPix3 = (pix | color);                             \
        }                                                       \
        pPix1++; pPix2++; pPix3++;                              \
        xacc += zoom;                                           \
    }                                                           \
    xacc -= 0x200;                                              \
}


void hwsprites::render_ref(uint16_t* pixels, const uint8_t priority)
{
    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;

    for (uint16_t data = 0; data < SPRITE_RAM_SIZE; data += 16) // was +=8
    {
        // stop when we hit the end of sprite list
        if ((ramBuff[data+0] & 0x8000) != 0) break;

        uint32_t sprpri  = 1 << ((ramBuff[data+3] >> 12) & 3);
        if (sprpri != priority) continue;

        // if hidden, or top greater than/equal to bottom, or invalid bank, punt
        int16_t hide    = (ramBuff[data+0] & 0x5000);
        int32_t height  = (ramBuff[data+5] >> 8) + 1;
        if (hide != 0 || height == 0) continue;

        int16_t bank      =  (ramBuff[data+0] >> 9) & 7;
        int32_t top       =  (ramBuff[data+0] & 0x1ff) - 0x100;
        bool     clip     =  (ramBuff[data+0] & 0x2000);
        uint32_t addr     =   ramBuff[data+1];
        int32_t pitch     = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        uint8_t shadow    =  (ramBuff[data+3] >> 14) & 1;
        int32_t  zoom     =   ramBuff[data+3] & 0x0fff;
        int32_t ydelta    = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t flip      = (~ramBuff[data+4] >> 14) & 1;
        int32_t xdelta    = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t width     =   ramBuff[data+4] & 0x7ff;
        int32_t color     = COLOR_BASE +
                            ((ramBuff[data+5] & 0x7f) << 4);
        int32_t sprite_scanlines =
                            ((ramBuff[data+5] & 0xff00) >> 16) +1;
        int32_t xpos      =   ramBuff[data+6]; // moved from original structure to accomodate widescreen
        int32_t rawh      =   ramBuff[data+7]; // JJP - height of sprite in rows
        int32_t y, ytarget, yacc = 0;

        int16_t offset    =   ramBuff[data+15]; // JJP - provides horizontal offset for hires sprite rendering
        // adjust X coordinate
        // note: the threshhold below is a guess. If it is too high, rachero will draw garbage
        // If it is too low, smgp won't draw the bottom part of the road
        if (xpos < 0x80 && xdelta < 0)
            xpos += 0x200;
        xpos -= 0xbe;

        // clamp to within the memory region size
        if (numbanks)
            bank %= numbanks;

        const uint32_t* spriterom = sprites + 0x10000 * bank;
        uint32_t* spriterom_flipped = sprites_flipped_ref + 0x10000 * bank;
        uint8_t* spriterom_shadowinfo = sprites_shadowinfo_ref + 0x10000 * bank;

        // loop from top to bottom
        ytarget = top + ydelta * height;

        // Adjust for widescreen mode
        xpos += config.s16_x_off;

        // Adjust for hi-res mode
        if (config.video.hires) {
            xpos <<= 1;
            top <<= 1;
            ytarget <<= 1;
            zoom >>= 1;
        }

        // JJP - maintain converted sprites to aid writing them
        uint32_t sprite_height = 0;
        // Determine underlying sprite height
        int32_t steps = ytarget - top;  // true as ydelta is always + or - 1.
        // Use 64-bit multiply to avoid overflow
        sprite_height = ((uint64_t)steps * zoom) >> 9;

        if (xdelta == -1) {
            xdelta = 1;       // always draw left-to-right
            xpos  -= (width << (config.video.hires ? 1 : 0));   // move draw position left by rendered width
            if (flip) {
                flip = 0;
                addr -= (pitch - 1);
            } else {
                flip = 1;
                addr += (pitch - 1);
            }
        }

        uint32_t shadowaddr = addr;

        if (flip) {
            // first encounter of sprite, created flipped entry
            shadowaddr -= (pitch - 1); // start of data, if unflipped

            // S0.3/F1 fix: ref_prepass_fix widens this bound from
            // "y < sprite_height" (the shipped, history-dependent bound) to
            // "y <= sprite_height", building every row the draw loop below
            // can possibly read.
            for (int y = 0; y < (int)sprite_height + (ref_prepass_fix ? 1 : 0); y++) {
                // calculate the addresses for this line
                uint32_t readaddr     = shadowaddr;
                uint32_t writeaddr    = shadowaddr + pitch;
                uint32_t shadow_found = 0;
                if (spriterom_flipped[readaddr] == 0xffffffff) {
                    // first time we've seen this row
                    for (int x = 0; x < pitch; x++) {
                        uint32_t pixels = spriterom[readaddr++];
                        uint8_t  px[8];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0)
                            shadow_found = 0x11;
                        // process eight pixels
                        px[0] = (pixels >> 28) & 0xf;
                        px[1] = (pixels >> 24) & 0xf;
                        px[2] = (pixels >> 20) & 0xf;
                        px[3] = (pixels >> 16) & 0xf;
                        px[4] = (pixels >> 12) & 0xf;
                        px[5] = (pixels >>  8) & 0xf;
                        px[6] = (pixels >>  4) & 0xf;
                        px[7] = (pixels >>  0) & 0xf;

                        spriterom_flipped[--writeaddr] =
                            (px[0] <<  0) |
                            (px[1] <<  4) |
                            (px[2] <<  8) |
                            (px[3] << 12) |
                            (px[4] << 16) |
                            (px[5] << 20) |
                            (px[6] << 24) |
                            (px[7] << 28);
                    };
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        } else {
            // not flipped
            for (int y = 0; y < (int)sprite_height + (ref_prepass_fix ? 1 : 0); y++) {
                if (spriterom_shadowinfo[shadowaddr] == 0xff) {
                    // first time seeing this sprite line
                    uint32_t readaddr = shadowaddr;
                    uint32_t shadow_found = 0;
                    for (int i = 0; i < pitch; ++i) {
                        uint32_t pixels = spriterom[readaddr++];
                        // check for shadows
                        uint32_t pxt = pixels ^ 0xAAAAAAAAu;
                        if (((pxt - 0x11111111u) & ~pxt & 0x88888888u) != 0) {
                            shadow_found = 0x11;
                            break;
                        }
                    }
                    spriterom_shadowinfo[shadowaddr] = shadow_found;
                }
                shadowaddr += pitch;
            }
        }

        // adjust x-position with pre-determined offset for hi-res sprite rendering
        if (config.video.hiresprites == 1)
            xpos += offset;

        // choose which ROM to read from - flipped or non-flipped
        const uint32_t* spritedata;
        if (flip) {
            addr = addr - pitch + 1; // set pointer to start of sprite row
            spritedata = &spriterom_flipped[0];
        } else {
            spritedata = &spriterom[0];
        }

        const uint16_t scrn_width = config.s16_width;
        const unsigned span = (unsigned)(x2 - x1);

        uint32_t jump_key = 3;
        // drawing loop
        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < config.s16_height)
            {
                uint16_t* pPix1 = pixels + ((y+0) * scrn_width) + xpos;
                uint32_t spriteaddr = addr;
                int32_t xacc = 0;

                // determine how many rows will be written on this pass
                uint16_t count = 1;
                uint32_t frac = yacc + zoom;
                uint16_t countmax = std::min( (uint16_t)(ytarget - y),
                                              std::min<uint16_t>(3, config.s16_height - y));
                while (frac < 0x200 && count < countmax) {
                    frac += zoom;
                    count++;
                }

                bool shadowfound = shadow && (spriterom_shadowinfo[addr] == 0x11);

                count = (count - 1) & 3; // 0..3
                jump_key =   (count                   ) |
                             ((clip)         ? 0u : 4u) |
                             ((shadowfound)  ? 8u : 0u);

                switch (jump_key) {

                    case 0:
                    {
                        // no shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 1:
                    {
                        // no shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 2:
                    {
                        // no shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_ns_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 3: break; // count==4 - not used as <1%
                    case 4:
                    {
                        // no shadows, not clipped, count==1, not flipped
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 5:
                    {
                        // no shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 6:
                    {
                        // no shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc_ns_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc_ns_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 7: break; // count==4 - not used as <1%
                    case 8:
                    {
                        // shadows, clipped, count==1, not flipped
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        break;
                    }
                    case 9:
                    {
                        // shadows, clipped, count==2, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 10:
                    {
                        // shadows, clipped, count==3, not-flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_ref();
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 11: break; // count==4 - not used as <1%
                    case 12:
                    {
                        // shadows, not clipped, count==1, not flipped
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        break;
                    }
                    case 13:
                    {
                        // shadows, not clipped, count==2, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        yacc += zoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                        break;
                    }
                    case 14:
                    {
                        // shadows, not clipped, count==3, not flipped
                        uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                        uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc_ref();
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc_ref();
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                        for (int i=1; i<2; i++) {
                            yacc += zoom;
                            addr += pitch * (yacc >> 9);
                            yacc &= 0x1ff;
                            y++;
                        }
                        break;
                    }
                    case 15: break; // count==4 - not used as <1%
                }
            }
            // accumulate zoom factors; if we carry into the high bit, skip an extra row
            yacc += zoom;
            addr += pitch * (yacc >> 9);
            yacc &= 0x1ff;
        }
    }
}

#undef draw_pixel_1row_ref
#undef draw_pixel_2row_ref
#undef draw_pixel_3row_ref
#undef draw_pixel_1row_nc_ref
#undef draw_pixel_2row_nc_ref
#undef draw_pixel_3row_nc_ref
#undef draw_pixel_1row_ns_ref
#undef draw_pixel_2row_ns_ref
#undef draw_pixel_3row_ns_ref
#undef draw_pixel_1row_nc_ns_ref
#undef draw_pixel_2row_nc_ns_ref
#undef draw_pixel_3row_nc_ns_ref

#endif // CB_RENDER_TEST
