#pragma once
/***************************************************************************
    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
***************************************************************************/

#include "../stdint.hpp"
#include "../globals.hpp"
#include "../setup.hpp"

#include <SDL.h>

#include <cstdint>
#include <cstddef> // For std::size_t
#include <atomic>
constexpr std::size_t ALIGNMENT = 16; // 16 for SSE, 32 for AVX
constexpr std::size_t LOOKUP_SIZE = (32 * 32 * 32 * 2); // 32 colours per channel plus shadow bit

// Abstract Rendering Class
class RenderBase
{
public:
    RenderBase();
    virtual ~RenderBase() = default;   // deleted through RenderBase* in Video's dtor

    virtual bool init(int src_width, int src_height,
                      int scale,
                      int video_mode,
                      int scanlines)          = 0;
    virtual void swap_buffers()               = 0;
    virtual void disable()                    = 0;
    virtual bool start_frame()                = 0;
    virtual bool finalize_frame()             = 0;
    virtual void draw_frame(uint16_t* pixels, int fastpass) = 0;
    void convert_palette(uint32_t adr, uint32_t r1, uint32_t g1, uint32_t b1);
    void set_shadow_intensity(float f);
    void init_palette(int red_curve, int green_curve, int blue_curve);
    virtual bool supports_window() { return true; }
    virtual bool supports_vsync() { return false; }

    // Regenerates any NTSC filter kernel slots marked dirty by convert_palette() since the
    // last call. Called from the main loop at a point where no render thread is active (see
    // main.cpp), so implementations may rewrite their kernel table without locking. Default
    // no-op for renderers without an NTSC filter table (e.g. non-Blargg paths).
    virtual void flush_ntsc_palette() {}

    // Advances the NTSC filter's burst phase for the next frame. Called from the main loop
    // at the same safe point as flush_ntsc_palette() (see there for why that point has no
    // active render thread), so an implementation can update its phase state without a race
    // against the render threads reading it during draw_frame()/blargg_filter(). Default
    // no-op for renderers without an NTSC filter.
    virtual void advance_blargg_phase() {}

    // S16 video hardware ladder DAC values
    alignas(ALIGNMENT) uint32_t rgb_lookup[LOOKUP_SIZE];

protected:
    SDL_Surface *surface;

    // Palette Lookup (note - hilights are not used in Outrun)
    // x2 allows for base colours and shadow colours
    alignas(ALIGNMENT) uint16_t s16_rgb555[S16_PALETTE_ENTRIES * 2];
    // This palette avoids re-calculating for the Blargg filter for every pixel
    alignas(ALIGNMENT) uint16_t rgb_blargg[S16_PALETTE_ENTRIES * 2];

    // Dirty-slot tracking for incremental NTSC filter kernel regeneration (Part A of the
    // NTSC filter re-architecture). Set by convert_palette() (called synchronously on the
    // game-logic thread for every palette RAM write); drained by flush_ntsc_palette() at a
    // point in the main loop where no render thread is active. One byte per slot rather
    // than a bitset so convert_palette()'s hot path is a plain flag test-and-set.
    alignas(ALIGNMENT) uint8_t ntsc_slot_dirty[S16_PALETTE_ENTRIES * 2];
    std::atomic<uint32_t> ntsc_dirty_count{0};

    uint32_t *screen_pixels;

    // Original Screen Width & Height
    uint16_t orig_width, orig_height;

    // --------------------------------------------------------------------------------------------
    // Screen setup properties. Example below:
    // ________________________
    // |  |                |  | <- screen size      (e.g. 1280 x 720)
    // |  |                |  |
    // |  |                |<-|--- destination size (e.g. 1027 x 720) to maintain aspect ratio
    // |  |                |  | 
    // |  |                |  |    source size      (e.g. 320  x 224) System 16 proportions
    // |__|________________|__|
    //
    // --------------------------------------------------------------------------------------------

    // Source texture / pixel array that we are going to manipulate
    int src_width, src_height;

    // Destination window width and height
    int dst_width, dst_height;

    // Screen width and height
    int scn_width, scn_height;

    // Full-Screen, Stretch, Window
    int video_mode;

    // Scanline density. 0 = Off, 1 = Full
    int scanlines;

    // Offsets (for full-screen mode, where x/y resolution isn't a multiple of the original height)
    uint32_t screen_xoff, screen_yoff;

    // SDL Pixel Format Codes. These differ between platforms.
    uint8_t  Rshift, Gshift, Bshift, Ashift;
    uint32_t Rmask, Gmask, Bmask, Amask;

    // Shadow intensity multiplier
    int shadow_multi;

    bool sdl_screen_size();
};
