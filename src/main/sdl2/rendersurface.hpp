/*******************************************************************************
    SDL2 Hardware Surface Video Rendering.

    Copyright (c) 2012,2020 Manuel Alfayate and Chris White.
    Threading, Blargg integration and CRT masks Copyright (c) 2020 James Pearce

    See license.txt for more details.

*******************************************************************************/

#pragma once

#include "renderbase.hpp"
#include "snes_ntsc.h"
#include <mutex>

class RenderSurface : public RenderBase
{
public:
    RenderSurface();
    ~RenderSurface();
    bool init(int src_width, int src_height,
              int scale,
              int video_mode,
              int scanlines);
    void swap_buffers();
    void disable();
    bool start_frame();
    bool finalize_frame();
    void draw_frame(uint16_t* pixels);

private:
    // SDL2 window
    SDL_Window* window = 0;

    // SDL2 renderer
    //SDL_Renderer *renderer = 0;
    GPU_Target* renderer;
    GPU_Target* pass1;
    GPU_Target* screen;
    GPU_Image* GameImage;
    GPU_Image* pass1Target;
    GPU_ShaderBlock block;
    SDL_Surface* overlaySurface;
    SDL_Surface* GameSurface[2];
    int current_game_surface;
    uint32_t* GameSurfacePixels;
    uint32_t* overlaySurfacePixels;
    GPU_Image* overlayImage;
    uint32_t FrameCounter = 0; // enough space for over 2 years of continuous operation at 60fps

    // SDL2 texture
    SDL_Texture *game_tx = 0;        // game image
    SDL_Texture *overlay = 0;        // CRT curved edge mask

    // SDL2 blitting rects for hw scaling
    // ratio correction using SDL_RenderCopy()
    SDL_Rect src_rect;
    SDL_Rect rgb_rect;
    SDL_Rect dst_rect;

    // internal functions
    void create_buffers();
    void destroy_buffers();
    void init_blargg_filter();
    bool init_sdl(int video_mode);
    void init_overlay();

    // constants
    const int BPP = 32;

    // Blargg filter related
    snes_ntsc_setup_t setup;
    snes_ntsc_t* ntsc = 0;
    int snes_src_width;
    int phase;
    int phaseframe;

    // currently configured video settings. These are stored so that a change can be actioned.
    int scale           = 0;
    int flags           = 0;  // SDL flags
    int blargg          = 0;  // current Blargg filter value

    // GLSL shader related settings
    float alloff = 1.0f; // all effects off
    float warpX = 0.0f, warpY = 0.0f, expandX = 0.0f, expandY = 0.0f;
    float brightboost1 = 1.0f, brightboost2 = 1.0f;
    float nois = 0.0f;
    float vignette = 0.0f;
    float desaturate = 0.0f;
    float desaturateEdges = 0.0f;
    float sharpX = 0.0f, sharpY = 0.0f;
    float Shadowmask = 0.0f;

    // GPU uniform locations
    int loc_alloff = 0;
    int loc_warpX = 0;
    int loc_warpY = 0;
    int loc_expandX = 0;
    int loc_expandY = 0;
    int loc_brightboost = 0;
    int loc_noiseIntensity = 0;
    int loc_vignette = 0;
    int loc_desaturate = 0;
    int loc_desaturateEdges = 0;
    int loc_Shadowmask = 0;
    int loc_u_Time = 0;
    int loc_OutputSize = 0;

    // processing data
    int Alevel = 255;       // default alpha value for game image

    // working buffers for video processing
    uint32_t* game_pixels = 0;
    uint32_t* rgb_pixels = 0;            // used by Blargg filter

	// Locks due to threaded activity
	std::mutex drawFrameMutex, finalizeFrameMutex;
};
