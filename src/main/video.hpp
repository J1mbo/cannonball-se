/***************************************************************************
    Video Rendering. 
    
    - Renders the System 16 Video Layers
    - Handles Reads and Writes to these layers from the main game code
    - Interfaces with platform specific rendering code

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "stdint.hpp"
#include "globals.hpp"
#include "roms.hpp"
#include "hwvideo/hwtiles.hpp"
#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwroad.hpp"

namespace shadow
{
    const static float ORIGINAL = 0.63f; // Hardware Intensity (63%)
    const static float MAME = 0.78f;     // Mame Intensity (78%)
};

class hwsprites;
class RenderBase;
struct video_settings_t;

class Video
{
public:
	hwsprites* sprite_layer;
    hwtiles* tile_layer;
    uint16_t* pixel_buffers[2];
    uint16_t* pixels;
    int current_pixel_buffer;

    bool enabled;

    // S3.2: out[yy] = 1 iff the road foreground will write lo-res line yy
    // this frame (all zero when the foreground pass is skipped). Computed
    // once per frame in prepare_frame() from the same ramBuff/road_control
    // the foreground itself reads; S5 uses it to skip tiles the foreground
    // is about to overwrite in full.
    uint8_t road_fg_rows[S16_HEIGHT];

    // --- measurement only (branch sprite-a53) ---------------------------------
    // Nanoseconds accumulated inside prepare_frame(), split by layer, plus the
    // number of sprite scanlines drawn.  Written by whichever thread runs
    // prepare_frame(), read by the main thread for the periodic report: these
    // are monotonic accumulators, so a torn read would only mis-state one
    // sample and cannot corrupt state.
    uint64_t prof_ns_road_tiles = 0;   // sum of the five below
    uint64_t prof_ns_tilevals   = 0;   // tile_layer->update_tile_values()
    uint64_t prof_ns_road_bg    = 0;   // hwroad render_background
    uint64_t prof_ns_tile_bg    = 0;   // render_tile_layer(pixels, 1, 0)
    uint64_t prof_ns_tile_fg    = 0;   // render_tile_layer(pixels, 0, 0)
    uint64_t prof_ns_road_fg    = 0;   // hwroad render_foreground
    uint64_t prof_ns_sprites    = 0;
    uint64_t prof_ns_text       = 0;
    uint64_t prof_frames        = 0;
    uint64_t prof_frame_cksum   = 1469598103934665603ULL; // CB_FRAME_CKSUM test aid

	Video();
    ~Video();
    
	int init(Roms* roms, video_settings_t* settings);
    void swap_buffers();
    void disable();
    int set_video_mode(video_settings_t* settings);
    void set_shadow_intensity(float);
    void prepare_frame();
    void render_frame(int fastpass);
    void present_frame();
    void flush_ntsc_palette();
    void advance_blargg_phase();
    bool supports_window();
    bool supports_vsync();

    void clear_text_ram();
    void write_text8(uint32_t, const uint8_t);
	void write_text16(uint32_t*, const uint16_t);
	void write_text16(uint32_t, const uint16_t);
    void write_text32(uint32_t*, const uint32_t);
    void write_text32(uint32_t, const uint32_t);
    uint8_t read_text8(uint32_t);

    void clear_tile_ram();
    void write_tile8(uint32_t, const uint8_t);
	void write_tile16(uint32_t*, const uint16_t);
	void write_tile16(uint32_t, const uint16_t);
    void write_tile32(uint32_t*, const uint32_t);
    void write_tile32(uint32_t, const uint32_t);
    uint8_t read_tile8(uint32_t);

	void write_sprite16(uint32_t*, const uint16_t);

	void write_pal8(uint32_t*, const uint8_t);
	void write_pal16(uint32_t*, const uint16_t);
	void write_pal32(uint32_t*, const uint32_t);
	void write_pal32(uint32_t, const uint32_t);
	uint8_t read_pal8(uint32_t);
	uint16_t read_pal16(uint32_t*);
	uint16_t read_pal16(uint32_t);
    uint32_t read_pal32(uint32_t*);

private:
    // SDL Renderer
    RenderBase* renderer;

    const int alignment = 64;
	alignas(64) uint8_t palette[S16_PALETTE_ENTRIES * 2]; // 2 Bytes Per Palette Entry
    void refresh_palette(uint32_t);

};

extern Video video;
