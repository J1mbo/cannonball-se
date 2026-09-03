#pragma once

#include <string>
#include <atomic>

#include "globals.hpp"
#include "sdl2/audio.hpp"

namespace cannonball
{
    extern Audio audio;

    // Frame counter
	extern int frame;

    // Tick Logic. Used when running at non-standard > 30 fps
    extern bool tick_frame;

    // Millisecond Time Per Frame
    extern double frame_ms;

    // FPS Counter
    extern int fps_counter;

    // Engine Master State
    extern int state;

    enum
    {
        STATE_BOOT,
        STATE_INIT_MENU,
        STATE_MENU,
        STATE_INIT_GAME,
        STATE_GAME,
        STATE_QUIT
    };

    // JJP
    extern int  fps_lock; // 0=no lock (auto), 30(fps), 60(fps)
    extern bool singlecore_detect;
    extern bool singlecore_mode;
    extern long fps_eval_period;
    extern int  game_threads;
    extern bool perftest;

    // -------------------------------------------------------------------
    // Frame-dump facility (-dump <dir> [interval] [count] command line option).
    // When dump_enabled is true, RenderSurface::draw_frame() writes the NTSC
    // filter's input for every dump_interval-th rendered frame to dump_dir,
    // until dump_count frames have been written. See rendersurface.cpp
    // (write_dump_frame) for the file format.
    // -------------------------------------------------------------------
    extern bool        dump_enabled;
    extern std::string dump_dir;
    extern int         dump_interval;
    extern int         dump_count;
    // Set (from a render thread) once dump_count frames have been written.
    // The main loop polls this and performs a normal, clean shutdown via
    // cannonball::state = STATE_QUIT, rather than quitting from a render thread.
    extern std::atomic<bool> dump_finished;
}
