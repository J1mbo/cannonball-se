/******************************************************************************
    SDL2 Video Rendering.

    Copyright (c) 2012,2020 Manuel Alfayate and Chris White.

    Threading, Blargg filter, GLSL filter and CRT masks Copyright (c)
    2020,2025 James Pearce.

    See license.txt for more details.

*******************************************************************************/

#pragma once

#include "renderbase.hpp"
#include "snes_ntsc.h"
#include <SDL.h>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "gl_backend.hpp"   // tiny ES2 backend

class RenderSurface : public RenderBase
{
public:
    RenderSurface();
    ~RenderSurface();

    // our GLES context
    SDL_GLContext glContext;

    bool init(int src_width, int src_height,
              int scale,
              int video_mode,
              int scanlines);
    void swap_buffers();
    void disable();
    bool start_frame();
    bool finalize_frame();
    void draw_frame(uint16_t* pixels, int fastpass);

private:
    // SDL2 window
    SDL_Window* window = 0;

    SDL_Surface* overlaySurface;
    SDL_Surface* GameSurface[2];

    int current_game_surface;
    uint32_t* GameSurfacePixels;
    uint32_t* overlaySurfacePixels;
    uint32_t FrameCounter = 0; // enough space for over 2 years of continuous operation at 60fps

    // SDL2 texture
    SDL_Texture *game_tx = 0;        // game image
    SDL_Texture *overlay = 0;        // CRT curved edge mask

    // SDL2 blitting rects for hw scaling
    // ratio correction using SDL_RenderCopy()
    SDL_Rect src_rect;
    SDL_Rect rgb_rect;
    SDL_Rect dst_rect;

    // image position control (0,0 = top left; calculated in image scaling routine)
    int anchor_x = 0;
    int anchor_y = 0;

    // internal functions
    void create_buffers();
    void destroy_buffers();
    void init_blargg_filter();
    void set_scaling();
    bool init_sdl(int video_mode);
    void init_overlay();
    void blargg_filter(uint16_t* pixels, uint32_t* outputPixels, int section);

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
    std::string vs;
    std::string fs;
/*
    float warpX = 0.0f, warpY = 0.0f, expandX = 0.0f, expandY = 0.0f;
    float brightboost1 = 1.0f, brightboost2 = 1.0f;
    float nois = 0.0f;
    float vignette = 0.0f;
    float desaturate = 0.0f;
    float desaturateEdges = 0.0f;
    float sharpX = 0.0f, sharpY = 0.0f;
    float Shadowmask = 0.0f;
    float maskDim    = 0.75f;
    float maskBoost  = 1.33f;
    float maskSize   = 1.0f;
*/
    // processing data
    int Alevel = 255;       // default alpha value for game image

    // working buffers for video processing
    uint32_t* game_pixels = 0;
    uint32_t* rgb_pixels = 0;            // used by Blargg filter

	// Locks due to threaded activity
	std::mutex drawFrameMutex, finalizeFrameMutex, gpuMutex;

    // Locks to enable safe disable() call
    std::atomic<int> activity_counter{0}; // track ongoing iterations
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> shutting_down{false};

    // Synchronize fastpass (top/bottom) halves per frame
    std::mutex                fastpassMutex;
    std::condition_variable   fastpassCV;
    int                       fastpassArrivals = 0;   // 0 → 1 → 2 then reset
    bool                      fastpassPostFxDone = false; // lets the first waiter proceed

};
