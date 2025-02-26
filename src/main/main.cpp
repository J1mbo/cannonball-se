/***************************************************************************
    Cannonball Main Entry Point.
    
    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <cstring>
#include <iostream>

// SDL Library
#include <SDL.h>

// SDL Specific Code
#include "sdl2/timer.hpp"
#include "sdl2/input.hpp"

#include "video.hpp"

#include "romloader.hpp"
#include "trackloader.hpp"
#include "stdint.hpp"
#include "main.hpp"
#include "engine/outrun.hpp"
#include "frontend/config.hpp"
#include "frontend/menu.hpp"

#include "engine/oinputs.hpp"
#include "engine/ooutputs.hpp"
#include "engine/omusic.hpp"

// Direct X Haptic Support.
// Fine to include on non-windows builds as dummy functions used.
#include "directx/ffeedback.hpp"

// Multi-threading support
// This implementation runs three parts of frame rendering in parallel to enable 60fps operation even on
// Raspberry Pi Zero 2W.
#include <thread>
#include <mutex>
#include <chrono>
#include <omp.h>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <atomic>


// ------------------------------------------------------------------------------------------------
// Initialize Shared Variables
// ------------------------------------------------------------------------------------------------
using namespace cannonball;

int    cannonball::state       = STATE_BOOT;
double cannonball::frame_ms    = 0;
int    cannonball::frame       = 0;
bool   cannonball::tick_frame  = true;
int    cannonball::fps_counter = 0;


// ------------------------------------------------------------------------------------------------
// Main Variables and Pointers
// ------------------------------------------------------------------------------------------------
Audio cannonball::audio;
Menu* menu;
bool pause_engine;


// ------------------------------------------------------------------------------------------------

static void quit_func(int code)
{
//    audio.stop_audio();
    input.close_joy();
    forcefeedback::close();
    delete menu;
    SDL_Quit();
    exit(code);
}

static void process_events(void)
{
    SDL_Event event;

    // Grab all events from the queue.
    while(SDL_PollEvent(&event))
    {
        switch(event.type)
        {
            case SDL_KEYDOWN:
                // Handle key presses.
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    cannonball::state = STATE_QUIT;
                else
                    input.handle_key_down(&event.key.keysym);
                break;

            case SDL_KEYUP:
                input.handle_key_up(&event.key.keysym);
                break;

            case SDL_JOYAXISMOTION:
                input.handle_joy_axis(&event.jaxis);
                break;

            case SDL_JOYBUTTONDOWN:
                input.handle_joy_down(&event.jbutton);
                break;

            case SDL_JOYBUTTONUP:
                input.handle_joy_up(&event.jbutton);
                break;

            case SDL_CONTROLLERAXISMOTION:
                input.handle_controller_axis(&event.caxis);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
                input.handle_controller_down(&event.cbutton);
                break;

            case SDL_CONTROLLERBUTTONUP:
                input.handle_controller_up(&event.cbutton);
                break;

            case SDL_JOYHATMOTION:
                input.handle_joy_hat(&event.jhat);
                break;

            case SDL_JOYDEVICEADDED:
                input.open_joy();
                break;

            case SDL_JOYDEVICEREMOVED:
                input.close_joy();
                break;

            case SDL_QUIT:
                // Handle quit requests (like Ctrl-c).
                cannonball::state = STATE_QUIT;
                break;
        }
    }
}

static void tick()
{
    frame++;

    // Determine whether to tick certain logic for the current frame.
    tick_frame = (config.fps != 60) || (frame & 1);

    process_events();

    if (tick_frame)
    {
        oinputs.tick();           // Do Controls
        oinputs.do_gear();        // Digital Gear
    }
     
    switch (cannonball::state)
    {
        case STATE_GAME:
        {
            if (tick_frame)
            {
                if (input.has_pressed(Input::TIMER)) outrun.freeze_timer = !outrun.freeze_timer;
                if (input.has_pressed(Input::PAUSE)) pause_engine = !pause_engine;
                if (input.has_pressed(Input::MENU))  cannonball::state = STATE_INIT_MENU;
            }

            if (!pause_engine || input.has_pressed(Input::STEP))
                outrun.tick(tick_frame);
            
            if (tick_frame) input.frame_done();
        }
        break;

        case STATE_INIT_GAME:
            if (config.engine.jap && !roms.load_japanese_roms())
            {
                std::cerr << "Japanese ROMs not loaded." << std::endl;
                cannonball::state = STATE_QUIT;
            }
            else
            {
                tick_frame = true;
                pause_engine = false;
                outrun.init();
                cannonball::state = STATE_GAME;
            }
            break;

        case STATE_MENU:
            menu->tick();
            input.frame_done();
            //osoundint.tick();
            break;

        case STATE_INIT_MENU:
            oinputs.init();
            outrun.outputs->init();
            menu->init();
            cannonball::state = STATE_MENU;
            break;
    }

    // Report output state for SmartyPi
    outrun.outputs->writeDigitalToConsole();

    if (tick_frame)
    {
        // Controller Rumble. Only rumble in-game.

        // JJP - Enhancement - high frequency rumble 'ticks' when skidding on road
        if (outrun.SkiddingOnRoad() && outrun.game_state == GS_INGAME)
            input.set_rumble(true, config.controls.rumble, 1);
        else
            input.set_rumble(outrun.outputs->is_set(OOutputs::D_MOTOR), config.controls.rumble, 0);
    }
}

static void main_sound_loop()
{
    // This thread basically does what the Z80 on the S16 board is responsible for: the sound.
    // osound.tick() is called 125 times per second.
    // audio.tick() outputs the samples via SDL.
    double targetupdatetime = 0; // our start point
    double interval = 1000.0 / 125.0; // this is the fixed playback rate.
    double waittime; // time to wait in ms
    double sleeptime = 0;
    int audiotick = 0;

    audio.init(); // initialise the audio, but hold in paused state as nothing to play yet

    while ((cannonball::state != STATE_MENU) && (cannonball::state != STATE_GAME))
        SDL_Delay(long(interval)); // await game engine initialisation

    // here we go!
    targetupdatetime = double(SDL_GetTicks()); // our start point

    while (cannonball::state != STATE_QUIT) {
        targetupdatetime += interval;
        osoundint.tick(); // generate the game sounds, music etc

        // now pass things over to SDL, on first pass this will enable the audio too
        audio.tick();

        // and sleep until the end of the cycle
        waittime = targetupdatetime - double(SDL_GetTicks());
        if (waittime > (interval+1)) {
            // we wait a maximum of one update interval, so in this case,
            // reset timer as likely SDL_GetTicks wrapped
            targetupdatetime = double(SDL_GetTicks());
            std::cout << "SDL timer overflow" << std::endl;
        } else if (waittime > 0) {
            SDL_Delay(Uint32(waittime));
        } else if (waittime <= -48) {
            // we are more than 48ms behind; perhaps the video mode was reset
            audio.resume_audio(); // clears buffers and re-establishes delay
            targetupdatetime = double(SDL_GetTicks()); // reset timer
            std::cout << "Audio resync (possible CPU saturation)" << std::endl;
        }
    }
    audio.stop_audio(); // we're done
}

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>
#endif

// Combined thread: updates play stats and kicks the watchdog (on linux)
static void play_stats_and_watchdog_updater()
{
    // this thread saves play stats periodically to the associated config file
    // e.g. runtime and number of plays
    // Updates every minute in a seperate thread since SD-card access can be slow
    // and we don't want to hold up the game engine.
    // Also kicks system watchdog when compiled for Linux

#ifdef __linux__
// Open the watchdog device. Note - device name is defined in globals.hpp
    int wd_fd = open(SYSTEM_WATCHDOG, O_WRONLY);
    if (wd_fd < 0)
    {
        perror("Failed to open " SYSTEM_WATCHDOG);
    }
    else
    {
        // Set the watchdog timeout (in seconds)
        int timeout = 15; // 15 seconds is the maximum supported on Raspberry Pi
        if (ioctl(wd_fd, WDIOC_SETTIMEOUT, &timeout) < 0)
        {
            perror("Could not set watchdog timeout");
            close(wd_fd);
            wd_fd = -1;
        }
        else
        {
            printf("Watchdog timeout set to %d seconds.\n", timeout);
        }
    }
#endif
    
    Timer run_time;
    run_time.start();
    while (cannonball::state != STATE_QUIT) {
        if ((run_time.get_ticks() >= 60000) &&
            (cannonball::state == STATE_GAME) )
        {
            config.stats.runtime++;  // increment machine run-time counter by 1 (minute)
            config.save_stats();     // save the stats to the file
            run_time.start();        // reset the timer
        }
        SDL_Delay(500); // wait 0.5 seconds before next check

#ifdef __linux__
        // Kick the watchdog device if it's open.
        if (wd_fd >= 0)
        {
            if (write(wd_fd, "\0", 1) < 0)
            {
                perror("Failed to write to " SYSTEM_WATCHDOG);
            }
        }
#endif
    }
#ifdef __linux__
    // Disable the watchdog on exit.
    if (wd_fd >= 0)
    {
        int disable = WDIOS_DISABLECARD;
        if (ioctl(wd_fd, WDIOC_SETOPTIONS, &disable) < 0)
        {
            perror("Could not disable watchdog");
        }
        close(wd_fd);
        printf("Watchdog disabled and closed.\n");
    }
#endif
}

/*
#pragma omp sections nowait
{
#pragma omp section
    {
        // draw the current frame n as the S16 would output.
        // This can be relatively heavy-weight e.g. in the arches section
        video.prepare_frame();
    }
#pragma omp section
    {
        // Convert the last frame (n-1) to RGB for presentation,
        // this includes Blargg NTSC filter if enabled.
        video.render_frame();
    }
} // no implicit barrier here
*/


enum class WorkerTask {
    None,
    PrepareFrame,
    RenderFrame,
    Quit
};

struct WorkerState {
    std::mutex mtx;
    std::condition_variable cv;

    // The current task for this worker
    WorkerTask task = WorkerTask::None;
    // Used to track if the worker has finished its current task
    bool taskDone = false;
};

void workerThreadFunc(WorkerState& state)
{
    while (true)
    {
        // 1. Wait for a task to be assigned
        {   
            std::unique_lock<std::mutex> lock(state.mtx);
            // Wait until we have a non-None task
            state.cv.wait(lock, [&state] {
                return state.task != WorkerTask::None;
                });

            if (state.task == WorkerTask::Quit) {
                return; // exit the while(true)
            }
        } // release lock before doing the actual work

        // 2. Perform the assigned task
        if (state.task == WorkerTask::PrepareFrame)
        {
            video.prepare_frame();
        }
        else if (state.task == WorkerTask::RenderFrame)
        {
            video.render_frame();
        }

        // 3. Mark that the task is done and go back to waiting
        {
            std::unique_lock<std::mutex> lock(state.mtx);
            state.taskDone = true;

            // Reset the task to None so we don't re-run it
            state.task = WorkerTask::None;
            state.cv.notify_one(); // signal that the task is done
        }
    }
}



static void main_loop()
{
    // Determine whether vsync is enabled (for future use).
    bool vsync = false;// (config.video.vsync == 1) && video.supports_vsync();

    // Get the configured FPS (should be either 30 or 60)
    int configured_fps = config.fps;
    double targetFPS = static_cast<double>(configured_fps);
    // Duration per frame (in seconds)
    auto frameDuration = std::chrono::duration<double>(1.0 / targetFPS);

    // Set the next frame time to now + frameDuration.
    auto nextFrameTime = std::chrono::high_resolution_clock::now() + frameDuration;

    // For diagnostics:
    int frameCounter = 0;
    int renderedFrames = 0;
    int droppedFrames = 0;
    auto fpsTimer = std::chrono::high_resolution_clock::now();

    // Performance check variables (10-second evaluation).
    auto performanceCheckStart = std::chrono::high_resolution_clock::now();
    int totalRenderedFramesForCheck = 0;
    // For 30 FPS, accumulate sleep time and frame count.
    std::chrono::duration<double> totalSleepTime(0);
    int frameCountForSleep = 0;

    // Launch worker threads.
    WorkerState worker1, worker2;
    std::thread t1(workerThreadFunc, std::ref(worker1));
    std::thread t2(workerThreadFunc, std::ref(worker2));

    while (cannonball::state != STATE_QUIT)
    {
        frameCounter++;
        bool forceRender = ((frameCounter % 4) == 0);
        auto now = std::chrono::high_resolution_clock::now();

        // If running at 30 FPS, count this frame for sleep evaluation.
        frameCountForSleep += (configured_fps == 30);


        // If we're behind schedule and not forcing a render, drop this frame.
        if (!forceRender && now > nextFrameTime)
        {
            auto delay = now - nextFrameTime;
            // If we're more than one frame behind, re-sync the next frame time.
            if (delay > frameDuration) {
                nextFrameTime = now + frameDuration;
            }
            else {
                ++droppedFrames;
                // Update game logic to keep simulation current.
                tick();
                // Advance by one frame.
                nextFrameTime += frameDuration;
                continue; // Skip heavy work this frame.
            }
        }

        if (!vsync) {
            // If we're ahead of schedule (and not forcing a render), sleep until it's time.
            if (!forceRender && now < nextFrameTime)
            {
                auto sleepDuration = nextFrameTime - now;
                // Only record sleep if we're at 30 FPS.
                totalSleepTime += sleepDuration * (configured_fps == 30);

                std::this_thread::sleep_for(sleepDuration);
                now = std::chrono::high_resolution_clock::now();
            }
        }

        // Update game logic.
        tick();

        // ---- LAUNCH WORKER TASKS & RENDERING ----

        // Signal worker1 for PrepareFrame.
        {
            std::unique_lock<std::mutex> lock(worker1.mtx);
            worker1.task = WorkerTask::PrepareFrame;
            worker1.taskDone = false;
            worker1.cv.notify_one();
        }

        // Signal worker2 for RenderFrame.
        {
            std::unique_lock<std::mutex> lock(worker2.mtx);
            worker2.task = WorkerTask::RenderFrame;
            worker2.taskDone = false;
            worker2.cv.notify_one();
        }

        // Run the GPU-bound work on the main thread.
        video.present_frame();
        renderedFrames++;
        totalRenderedFramesForCheck++;  // For performance evaluation

        // Wait for the worker threads to finish.
        {
            std::unique_lock<std::mutex> lock(worker1.mtx);
            worker1.cv.wait(lock, [&worker1] { return worker1.taskDone; });
        }
        {
            std::unique_lock<std::mutex> lock(worker2.mtx);
            worker2.cv.wait(lock, [&worker2] { return worker2.taskDone; });
        }

        // Swap the buffers for the next frame.
        video.swap_buffers();

        // Update the next frame time.
        nextFrameTime += frameDuration;

        // Record FPS info on console (every 2 seconds) and FPS on-screen if enabled
        auto elapsed = std::chrono::high_resolution_clock::now() - fpsTimer;
        if (elapsed >= std::chrono::seconds(2))
        {
            int fps = renderedFrames / 2;
            int totalFrames = renderedFrames + droppedFrames;
            int droppedPercent = (totalFrames > 0) ? (droppedFrames * 100 / totalFrames) : 0;
            printf("\r%i FPS (dropped: %i%%)    ", fps, droppedPercent);
            fflush(stdout);
            fps_counter = fps;
            renderedFrames = 0;
            droppedFrames = 0;
            fpsTimer = std::chrono::high_resolution_clock::now();
        }

        // ---- PERFORMANCE EVALUATION (every 10 seconds) ----
        auto performanceElapsed = std::chrono::high_resolution_clock::now() - performanceCheckStart;
        if (performanceElapsed >= std::chrono::seconds(10))
        {
            double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(performanceElapsed).count();
            if (configured_fps == 60)
            {
                // Evaluate average FPS at 60 FPS.
                double avgFPS = totalRenderedFramesForCheck / seconds;
                if (avgFPS < 50.0) {
                    printf("\nPerformance check: average FPS %.2f too low. Switching to 30 FPS.\n", avgFPS);
                    config.video.fps = 0; // 0 = 30 fps
                    config.set_fps(config.video.fps);
                }
            }
            else if (configured_fps == 30)
            {
                // Evaluate average sleep fraction at 30 FPS.
                double avgSleepFraction = 0.0;
                if (frameCountForSleep > 0) {
                    avgSleepFraction = totalSleepTime.count() / (frameDuration.count() * frameCountForSleep);
                }
                if (avgSleepFraction > 0.6) {
                    printf("\nPerformance check: average sleep fraction %.2f%%. Switching to 60 FPS.\n", avgSleepFraction * 100.0);
                    config.video.fps = 2; // 2 = 60 fps
                    config.set_fps(config.video.fps);
                }
            }
            // Reset performance evaluation counters.
            performanceCheckStart = std::chrono::high_resolution_clock::now();
            totalRenderedFramesForCheck = 0;
            totalSleepTime = std::chrono::duration<double>(0);
            frameCountForSleep = 0;
        }

        // Update control variables if there is an FPS change
        if (config.fps != configured_fps)
        {
            configured_fps = config.fps;
            targetFPS = static_cast<double>(configured_fps);
            frameDuration = std::chrono::duration<double>(1.0 / targetFPS);
            nextFrameTime = std::chrono::high_resolution_clock::now() + frameDuration;

            // Determine if we can rely on vsync (for 60fps mode)
            SDL_DisplayMode displayMode;
            if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
                // Can retrieve monitor refresh rate
                vsync = (displayMode.refresh_rate == configured_fps) && SDL_GL_GetSwapInterval();
                // printf("\nvsync: %i\n", vsync);
            }
        }
    }

    // Signal the worker threads to quit.
    {
        std::unique_lock<std::mutex> lock(worker1.mtx);
        worker1.task = WorkerTask::Quit;
        worker1.cv.notify_one();
    }
    {
        std::unique_lock<std::mutex> lock(worker2.mtx);
        worker2.task = WorkerTask::Quit;
        worker2.cv.notify_one();
    }
    t1.join();
    t2.join();
    printf("\n");
}


// Very (very) simple command line parser.
// Returns true if everything is ok to proceed with launching th engine.
static bool parse_command_line(int argc, char* argv[])
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "-cfgfile") == 0 && i+1 < argc)
        {
            config.set_config_file(argv[i+1]);
        }
        else if (strcmp(argv[i], "-file") == 0 && i+1 < argc)
        {
            if (!trackloader.set_layout_track(argv[i+1]))
                return false;
        }
        else if (strcmp(argv[i], "-help") == 0)
        {
            std::cout << "Command Line Options:\n\n" <<
                         "-cfgfile: Location and name of config.xml\n" <<
                         "-file   : LayOut Editor track data to load\n" << std::endl;
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[])
{
    // Parse command line arguments (config file location, LayOut data) 
    bool ok = parse_command_line(argc, argv);

    if (ok)
    {
        config.load(); // Load config.XML file
        ok = roms.load_revb_roms(config.sound.fix_samples);
    }
    if (!ok)
    {
        quit_func(1);
        return 0;
    }

    // Load machine stats e.g. playcount and runtime (defaults to 0 each if file not present)
    config.load_stats();

    // Load gamecontrollerdb.txt mappings
    if (SDL_GameControllerAddMappingsFromFile((config.data.res_path + "gamecontrollerdb.txt").c_str()) == -1)
        std::cout << "Warning: Unable to load game controller mapping file." << std::endl;

    // Initialize timer and video systems
    // On Linux with RPi4 in desktop environment, x11 is default but will cap full-screen to 30fps with vsync enabled.
    // Wayland allows 60fps full-screen.
#ifdef __linux__ 
    //SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
  #define SDL_HINT_QTWAYLAND_WINDOW_FLAGS "StaysOnTop BypassWindowManager"
    std::cout << "Cannonball requires wayland video driver for 60fps operation under desktop environment. Start cannonball like:" << std::endl;
    std::cout << "$ SDL_VIDEODRIVER=""wayland"" build/cannonball" << std::endl;
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "Cannonball");
    //SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) == -1)
    {
        std::cerr << "SDL Initialization Failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    printf("Available SDL video drivers:\n");
    int ndri = SDL_GetNumVideoDrivers();
    for (int i = 0; i < ndri; i++) {
        printf("   %s\n", SDL_GetVideoDriver(i));
    }

    // Load patched widescreen tilemaps
    if (!omusic.load_widescreen_map(config.data.res_path))
        std::cout << "Unable to load widescreen tilemaps" << std::endl;

    // Initialize SDL Video
    config.set_fps(config.video.fps);
    if (!video.init(&roms, &config.video))
        quit_func(1);

    cannonball::state = config.menu.enabled ? STATE_INIT_MENU : STATE_INIT_GAME;

    /* Initalize SDL Controls */

    // If a controller has Rumble feature that is supported by SDL, it will be enabled here.
    // If the controller has Rumble not supported by SDL, it can be used anyway via /dev/hidraw.
    // In that case, Input::set_rumble() needs to be amended for the specific controller.
    // Controller Rumble function is controlled in tick() above.

    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig, 
               config.controls.analog,    config.controls.axis, config.controls.invert, config.controls.asettings);

    // Regardless to rumble, if haptic is enabled in config.xml, this is handled via ffeedback.cpp using either
    // DirectX (Windows) or /dev/input/event on Linux. This also includes control of real cabinet hardware via
    // SmartyPi. Therefore, haptic takes priority over simple rumble.

    if (config.controls.haptic) 
        config.controls.haptic = forcefeedback::init(config.controls.max_force, config.controls.min_force, config.controls.force_duration);
        
    // Populate menus
    menu = new Menu();
    menu->populate();

    // start the game threads
	std::thread sound(main_sound_loop); // Sound thread (Z80 on S16)
    std::thread stats(play_stats_and_watchdog_updater); // Play stats file updater thread
    
    // Now start the main game loop, which includes SDL video and input
	main_loop();

	// Wait for threads to finish
    sound.join();
    stats.join();
    quit_func(0);

    // Never Reached
    return 0;
}
