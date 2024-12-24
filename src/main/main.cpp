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

//Multi-threading to enable seperate audio and game threads, as on the S16 system board
#include <thread>
#include <mutex>
#include <time.h>

// ------------------------------------------------------------------------------------------------
// Initialize Shared Variables
// ------------------------------------------------------------------------------------------------
using namespace cannonball;

int    cannonball::state       = STATE_BOOT;
double cannonball::frame_ms    = 0;
int    cannonball::frame       = 0;
bool   cannonball::tick_frame  = true;
int    cannonball::fps_counter = 0;

std::mutex mainMutex; // used to sequence audio/game/sdl threads

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
                    state = STATE_QUIT;
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
                state = STATE_QUIT;
                break;
        }
    }
}

static void tick()
{
    frame++;

    // Non standard FPS: Determine whether to tick certain logic for the current frame.
    if (config.fps == 60)
        tick_frame = frame & 1;
    else if (config.fps == 120)
        tick_frame = (frame & 3) == 1;

    process_events();

    if (tick_frame)
    {
        oinputs.tick();           // Do Controls
        oinputs.do_gear();        // Digital Gear
    }
     
    switch (state)
    {
        case STATE_GAME:
        {
            if (tick_frame)
            {
                if (input.has_pressed(Input::TIMER)) outrun.freeze_timer = !outrun.freeze_timer;
                if (input.has_pressed(Input::PAUSE)) pause_engine = !pause_engine;
                if (input.has_pressed(Input::MENU))  state = STATE_INIT_MENU;
            }

            if (!pause_engine || input.has_pressed(Input::STEP))
            {
                outrun.tick(tick_frame);
                if (tick_frame) input.frame_done();
//                osoundint.tick();
            }
            else
            {                
                if (tick_frame) input.frame_done();
            }
        }
        break;

        case STATE_INIT_GAME:
            if (config.engine.jap && !roms.load_japanese_roms())
            {
                state = STATE_QUIT;
            }
            else
            {
                tick_frame = true;
                pause_engine = false;
                outrun.init();
                state = STATE_GAME;
            }
            break;

        case STATE_MENU:
            menu->tick();
            input.frame_done();
            osoundint.tick();
            break;

        case STATE_INIT_MENU:
            oinputs.init();
            outrun.outputs->init();
            menu->init();
            state = STATE_MENU;
            break;
    }

    // Map OutRun outputs to CannonBall devices (SmartyPi Interface / Controller Rumble)
    outrun.outputs->writeDigitalToConsole();
    if (tick_frame)
    {
         input.set_rumble(outrun.outputs->is_set(OOutputs::D_MOTOR), config.controls.rumble);
    }
}

static void main_sound_loop()
{
    // This thread basically does what the Z80 on the S16 board is responsible for: the sound.
    // osound.tick() is called 125 times per second with isolated access to the game engine
    // via the mutex lock. audio.tick() the outputs the samples via SDL outside of the lock.
    double targetupdatetime = 0; // our start point
    double interval = 1000.0 / 125.0; // this is the fixed playback rate.
    double waittime; // time to wait in ms
    double sleeptime = 0;
    struct timespec ts;
    int res; int audiotick = 0;
    long thislocktime = 0;

    audio.init(); // initialise the audio, but hold in paused state as nothing to play yet
    while ((state != STATE_MENU) && (state != STATE_GAME))
        SDL_Delay(long(interval)); // await game engine initialisation

    // here we go!
    targetupdatetime = double(SDL_GetTicks()); // our start point
    while (state != STATE_QUIT) {
        // check if another thread is running and wait if it is
        mainMutex.lock();
        targetupdatetime += interval;
        thislocktime = SDL_GetTicks();
        osoundint.tick(); // generate the game sounds, music etc
        mainMutex.unlock();
        //thislocktime = SDL_GetTicks() - thislocktime;
        //if (thislocktime > maxlocktime) maxlocktime = thislocktime;

        // now pass things over to SDL, on first pass this will enable the audio too
        audio.tick();

        // and sleep until the end of the cycle
        waittime = targetupdatetime - double(SDL_GetTicks());
        if (waittime > (interval+1)) {
            // we wait a maximum of one update interval, so in this case,
            // reset timer as likely SDL_GetTicks wrapped
            targetupdatetime = double(SDL_GetTicks());
            std::cout << "Audio resync due to SDL timer overflow" << std::endl;
        } else if (waittime > 0) {
            SDL_Delay(Uint32(waittime));
        } else if (waittime <= -40) {
            // we are more than 40ms behind; perhaps the video mode was reset
            // 40ms aligns with SDL2 SND_DELAY value also
            audio.resume_audio(); // clears buffers and re-establishes delay
            targetupdatetime = double(SDL_GetTicks()); // reset timer
            std::cout << "Audio resync (possible CPU saturation)" << std::endl;
        }
    }
    audio.stop_audio(); // we're done
}


static void main_loop()
{
    // FPS Counter (If Enabled)
    Timer fps_count;
    int frame = 0;
    fps_count.start();

    // General Frame Timing
    bool vsync = config.video.vsync == 1 && video.supports_vsync();
    Timer frame_time;
    int t;                              // Actual timing of tick in ms as measured by SDL (ms)
    double deltatime  = 0;              // Time we want an entire frame to take (ms)
    int deltaintegral = 0;              // Integer version of above

    while (state != STATE_QUIT)
    {
        // check if another thread is running and wait if it is
        mainMutex.lock();
        frame_time.start();
        // Tick Engine
        tick();
        mainMutex.unlock();

        // Draw SDL Video
        video.prepare_frame();
        video.render_frame();

        // Calculate Timings. Cap Frame Rate. Note this might be trumped by V-Sync
        if (!vsync)
        {
            deltatime += (frame_ms * audio.adjust_speed());
            deltaintegral = (int)deltatime;
            t = frame_time.get_ticks();
            
            if (t < deltatime)
                SDL_Delay((Uint32)(deltatime - t));

            deltatime -= deltaintegral;
        }

        if (config.video.fps_count)
        {
            frame++;
            // After one second has elapsed...
            if (fps_count.get_ticks() >= 1000)
            {
                fps_counter = frame;
                frame       = 0;
                fps_count.start();
            }
        }
    }
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

    // Load gamecontrollerdb.txt mappings
    if (SDL_GameControllerAddMappingsFromFile((config.data.res_path + "gamecontrollerdb.txt").c_str()) == -1)
        std::cout << "Unable to load controller mapping" << std::endl;

    // Initialize timer and video systems
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) == -1)
    {
        std::cerr << "SDL Initialization Failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Load patched widescreen tilemaps
    if (!omusic.load_widescreen_map(config.data.res_path))
        std::cout << "Unable to load widescreen tilemaps" << std::endl;

    // Initialize SDL Video
    config.set_fps(config.video.fps);
    if (!video.init(&roms, &config.video))
        quit_func(1);

    // Initialize SDL Audio
//    audio.init();

    state = config.menu.enabled ? STATE_INIT_MENU : STATE_INIT_GAME;

    // Initalize SDL Controls
    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig, 
               config.controls.analog,    config.controls.axis, config.controls.invert, config.controls.asettings);

    if (config.controls.haptic) 
        config.controls.haptic = forcefeedback::init(config.controls.max_force, config.controls.min_force, config.controls.force_duration);
        
    // Populate menus
    menu = new Menu();
    menu->populate();

    // start the game threads
    std::thread sound(main_sound_loop); // Z80 thread
    main_loop();  // Loop until we quit the app
    sound.join();
    quit_func(0);

    // Never Reached
    return 0;
}
