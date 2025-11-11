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

/*******************************************************************************************
*  Out Run/X-Board-style sprites
*
*      Offs  Bits               Usage
*       +0   e------- --------  Signify end of sprite list
*       +0   -h-h---- --------  Hide this sprite if either bit is set
*       +0   --c----- --------  Clip required
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

// Enable for hardware pixel accuracy, where sprite shadowing delayed by 1 clock cycle (slower)
#define PIXEL_ACCURACY 0

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

#if PIXEL_ACCURACY

// Reproduces glowy edge around sprites on top of shadows as seen on Hardware.
// Believed to be caused by shadowing being out by one clock cycle / pixel.
//
// 1/ Sprites Drawn on top of Shadow clears the shadow flags for its opaque pixels.
// 2/ Either the flag clear or the sprite itself is offset by one pixel horizontally.
//
// Thanks to Alex B. for this implementation.

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

#else

#define IF_IS_DRAWABLE_() if (x >= x1 && x < x2 && pix != 0 && pix != 15)
#define IF_IS_DRAWABLE() if (((unsigned)(pix - 1) < 14u) && ((unsigned)(x - x1) < span))
#define IF_IS_DRAWABLE_NO_CLIP() if ((unsigned)(pix - 1) < 14u)

#define draw_pixel_1row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; x += xdelta; xacc += hzoom;            \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        x += xdelta; xacc += hzoom;                             \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        pPix3 += xdelta;                                        \
        x += xdelta; xacc += hzoom;                             \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_4row()                                       \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE() {                                      \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
                *pPix4 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
                *pPix4 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        pPix3 += xdelta; pPix4 += xdelta;                       \
        x += xdelta; xacc += hzoom;                             \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_1row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; xacc += hzoom;                         \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_2row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        xacc += hzoom;                                          \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_3row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        pPix3 += xdelta;                                        \
        xacc += hzoom;                                          \
    }                                                           \
    xacc -= 0x200;                                              \
}

#define draw_pixel_4row_nc()                                    \
{                                                               \
    while (xacc < 0x200) {                                      \
        IF_IS_DRAWABLE_NO_CLIP() {                              \
            if (shadow && pix == 0xa) {                         \
                *pPix1 |= S16_PALETTE_ENTRIES;                  \
                *pPix2 |= S16_PALETTE_ENTRIES;                  \
                *pPix3 |= S16_PALETTE_ENTRIES;                  \
                *pPix4 |= S16_PALETTE_ENTRIES;                  \
            } else {                                            \
                *pPix1 = (pix | color);                         \
                *pPix2 = (pix | color);                         \
                *pPix3 = (pix | color);                         \
                *pPix4 = (pix | color);                         \
            }                                                   \
        }                                                       \
        pPix1 += xdelta; pPix2 += xdelta;                       \
        pPix3 += xdelta; pPix4 += xdelta;                       \
        xacc += hzoom;                                          \
    }                                                           \
    xacc -= 0x200;                                              \
}

#endif

void hwsprites::render(uint16_t* pixels, const uint8_t priority)
{
    const uint32_t numbanks = SPRITES_LENGTH / 0x10000;

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

        int16_t bank      =  (ramBuff[data+0] >> 9) & 7;
        int32_t top       =  (ramBuff[data+0] & 0x1ff) - 0x100;
        bool     clip     =  (ramBuff[data+0] & 0x2000);
        uint32_t addr     =   ramBuff[data+1];
        int32_t pitch     = ((ramBuff[data+2] >> 1) | ((ramBuff[data+4] & 0x1000) << 3)) >> 8;
        uint8_t shadow    =  (ramBuff[data+3] >> 14) & 1;
        int32_t vzoom     =   ramBuff[data+3] & 0x7ff;
        int32_t ydelta    = ((ramBuff[data+4] & 0x8000) != 0) ? 1 : -1;
        int32_t flip      = (~ramBuff[data+4] >> 14) & 1;
        int32_t xdelta    = ((ramBuff[data+4] & 0x2000) != 0) ? 1 : -1;
        int32_t hzoom     =   ramBuff[data+4] & 0x7ff;
        int32_t color     = COLOR_BASE +
                            ((ramBuff[data+5] & 0x7f) << 4);
        int32_t xpos      =   ramBuff[data+6]; // moved from original structure to accomodate widescreen
        int32_t y, ytarget, yacc = 0;

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

        const uint32_t* spritedata = sprites + 0x10000 * bank;

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

        const uint16_t scrn_width = config.s16_width;
        const unsigned span = (unsigned)(x2 - x1);

//if (ydelta != 1) std::cout << "ydelta: " << ydelta << ", ytarget: " << ytarget << "\n";
        for (y = top; y != ytarget; y += ydelta)
        {
            // skip drawing if not within the cliprect
            if (y >= 0 && y < config.s16_height)
            {
                uint16_t* pPix1 = pixels + ((y+0) * scrn_width) + xpos;
                uint16_t* pPix2 = pixels + ((y+1) * scrn_width) + xpos;
                uint16_t* pPix3 = pixels + ((y+2) * scrn_width) + xpos;
                uint16_t* pPix4 = pixels + ((y+3) * scrn_width) + xpos;
                uint32_t spriteaddr = addr;
                int32_t xacc = 0;

                // determine how many rows will be written on this pass
                uint16_t count = 1;
                uint32_t frac = yacc + vzoom;
                uint16_t countmax = std::min<uint16_t>(4, config.s16_height - y);
                countmax = std::min(countmax,(uint16_t)(ytarget - y));
                while (frac < 0x200 && count < countmax) {
                    frac += vzoom;
                    count++;
                }
                // count now contains a value between 1 and 4

                if (clip) {
                if (count==1) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row();
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x0f000000) == 0x0f000000)
                                break;
                        }
                    }
                }
                if (count==2) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row();
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x0f000000) == 0x0f000000)
                                break;
                        }
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    yacc += vzoom;
                    addr += pitch * (yacc >> 9);
                    yacc &= 0x1ff;
                    y++;
                }
                if (count==3) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row();
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x0f000000) == 0x0f000000)
                                break;
                        }
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    for (int i=1; i<2; i++) {
                        yacc += vzoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                    }
                }
                if (count==4) {
                    // non-flipped case
                    if (flip == 0) {
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_4row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_4row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_4row();
                            pix = (pixels >>  0) & 0xf; draw_pixel_4row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x000000f0) == 0x000000f0)
                                break;
                        }
                    } else { // flipped case
                        for (int32_t x = xpos; (xdelta > 0 && x < scrn_width) || (xdelta < 0 && x >= 0); ) {
                            uint32_t pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_4row();
                            pix = (pixels >>  4) & 0xf; draw_pixel_4row();
                            pix = (pixels >>  8) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 12) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 16) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 20) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 24) & 0xf; draw_pixel_4row();
                            pix = (pixels >> 28) & 0xf; draw_pixel_4row();
                            // stop if the second-to-last pixel in the group was 0xf
                            if ((pixels & 0x0f000000) == 0x0f000000)
                                break;
                        }
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    for (int i=1; i<4; i++) {
                        yacc += vzoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
	                    y++;
                    }
                }
                } else {
                // no-clip path; sprite is fully on-screen
                if (count==1) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_1row_nc();
                            pix = (pixels >> 28) & 0xf; draw_pixel_1row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x0f000000) != 0x0f000000);
                    }
                }
                if (count==2) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_2row_nc();
                            pix = (pixels >> 28) & 0xf; draw_pixel_2row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x0f000000) != 0x0f000000);
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    yacc += vzoom;
                    addr += pitch * (yacc >> 9);
                    yacc &= 0x1ff;
                    y++;
                }
                if (count==3) {
                    // non-flipped case
                    if (flip == 0) {
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
                    } else { // flipped case
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_3row_nc();
                            pix = (pixels >> 28) & 0xf; draw_pixel_3row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x0f000000) != 0x0f000000);
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    for (int i=1; i<2; i++) {
                        yacc += vzoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
                        y++;
                    }
                }
                if (count==4) {
                    // non-flipped case
                    if (flip == 0) {
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr++];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >> 28) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >>  0) & 0xf; draw_pixel_4row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x000000f0) != 0x000000f0);
                    } else { // flipped case
                        uint32_t pixels;
                        do {
                            pixels = spritedata[spriteaddr--];
                            uint32_t pix;
                            // draw eight pixels
                            pix = (pixels >>  0) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >>  4) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >>  8) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 12) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 16) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 20) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 24) & 0xf; draw_pixel_4row_nc();
                            pix = (pixels >> 28) & 0xf; draw_pixel_4row_nc();
                            // stop if the second-to-last pixel in the group was 0xf
                        } while ((pixels & 0x0f000000) != 0x0f000000);
                    }
                    // accumulate zoom factors; if we carry into the high bit, skip an extra row
                    for (int i=1; i<4; i++) {
                        yacc += vzoom;
                        addr += pitch * (yacc >> 9);
                        yacc &= 0x1ff;
	                    y++;
                    }
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
