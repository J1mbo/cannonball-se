/***************************************************************************
    Front End Menu System.

    This file is part of Cannonball.
    Copyright (c) Chris White.
    See license.txt for more details.

    Modifications for CannonBall-SE Copyright (c) 2025, James Pearce
***************************************************************************/

// Boost string prediction
#include <boost/algorithm/string/predicate.hpp>

#include "main.hpp"
#include "menu.hpp"
#include "menulabels.hpp"
#include "../utils.hpp"

#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/osprites.hpp"
#include "engine/ologo.hpp"
#include "engine/omusic.hpp"
#include "engine/opalette.hpp"
#include "engine/otiles.hpp"

#include "frontend/cabdiag.hpp"
#include "frontend/ttrial.hpp"

#include <iostream>

// Logo Y Position
const static int16_t LOGO_Y = -60;

// Columns and rows available
const static uint16_t COLS = 40;
const static uint16_t ROWS = 28;

// Horizon Destination Position
const static uint16_t HORIZON_DEST = 0x3A0;


Menu::Menu()
{
    cabdiag = new CabDiag();
    ttrial  = new TTrial(config.ttrial.best_times);
}


Menu::~Menu(void)
{
    delete cabdiag;
    delete ttrial;
}

void Menu::populate()
{
    // Create Menus
    if (config.smartypi.enabled)
        populate_for_cabinet();
    else
        populate_for_pc();

    menu_handling.push_back(ENTRY_GRIP);
    menu_handling.push_back(ENTRY_OFFROAD);
    menu_handling.push_back(ENTRY_BUMPER);
    menu_handling.push_back(ENTRY_TURBO);
    menu_handling.push_back(ENTRY_COLOR);
    menu_handling.push_back(ENTRY_BACK);

    menu_cont.push_back(ENTRY_START_CONT);
    menu_cont.push_back(ENTRY_TRAFFIC);
    menu_cont.push_back(ENTRY_BACK);

    menu_timetrial.push_back(ENTRY_START);
    menu_timetrial.push_back(ENTRY_LAPS);
    menu_timetrial.push_back(ENTRY_TRAFFIC);
    menu_timetrial.push_back(ENTRY_BACK);

    menu_musictest.push_back(ENTRY_MUSIC1);
    menu_musictest.push_back(ENTRY_MUSIC2);
    menu_musictest.push_back(ENTRY_WAVEVOLUME);
    menu_musictest.push_back(ENTRY_CALLBACK_RATE);
    menu_musictest.push_back(ENTRY_BACK);

    menu_about.push_back(std::string("CANNONBALL-SE ") + CANNONBALL_SE_VERSION);
    menu_about.push_back("");
    menu_about.push_back("CANNONBALL IS FREE AND MAY NOT BE SOLD");
    menu_about.push_back("COPYRIGHT 2012-2024 CHRIS WHITE");
    menu_about.push_back("");
    menu_about.push_back("SE BUILD COPYRIGHT 2025 JAMES PEARCE");
    //menu_about.push_back(ENTRY_TOTAL_PLAYS);
    //menu_about.push_back(ENTRY_RUN_TIME);

    // Redefine menu text
    text_redefine.push_back("PRESS UP");
    text_redefine.push_back("PRESS DOWN");
    text_redefine.push_back("PRESS LEFT");
    text_redefine.push_back("PRESS RIGHT");
    text_redefine.push_back("PRESS ACCELERATE");
    text_redefine.push_back("PRESS BRAKE");
    text_redefine.push_back("PRESS GEAR OR GEAR LOW");
    text_redefine.push_back("PRESS GEAR HIGH");
    text_redefine.push_back("PRESS START");
    text_redefine.push_back("PRESS COIN IN");
    text_redefine.push_back("PRESS MENU");
    text_redefine.push_back("PRESS VIEW CHANGE");
}

// ------------------------------------------------------------------------------------------------
// Populate Menus for PC Setup
// ------------------------------------------------------------------------------------------------

void Menu::populate_for_pc()
{
    menu_main.push_back(ENTRY_PLAYGAME);
    menu_main.push_back(ENTRY_GAMEMODES);
    menu_main.push_back(ENTRY_SETTINGS);
    menu_main.push_back(ENTRY_ABOUT);
    menu_main.push_back(ENTRY_EXIT);

    menu_gamemodes.push_back(ENTRY_ENHANCED);
    menu_gamemodes.push_back(ENTRY_ORIGINAL);
    menu_gamemodes.push_back(ENTRY_CONT);
    menu_gamemodes.push_back(ENTRY_TIMETRIAL);
    menu_gamemodes.push_back(ENTRY_BACK);

    menu_settings.push_back(ENTRY_VIDEO);
#ifdef COMPILE_SOUND_CODE
    menu_settings.push_back(ENTRY_SOUND);
#endif
    menu_settings.push_back(ENTRY_CONTROLS);
    menu_settings.push_back(ENTRY_ENGINE);
    menu_settings.push_back(ENTRY_SCORES);
    menu_settings.push_back(ENTRY_SAVE);

    // FPS not now needed - frame rate now automatically switches
    menu_video.push_back(ENTRY_FPS_COUNTER);
    // always run fullscreen
    menu_video.push_back(ENTRY_WIDESCREEN);
    // scale not needed - image is expanded on GPU
    menu_video.push_back(ENTRY_X_OFFSET);
    menu_video.push_back(ENTRY_Y_OFFSET);
    menu_video.push_back(ENTRY_CRT_SHADER1); // JJP
    menu_video.push_back(ENTRY_BLARGG_FILTER); // JJP
    menu_video.push_back(ENTRY_BACK);

    // JJP - the following settings configure the CRT shader
    menu_crt_shader1.push_back(ENTRY_CRT_S16_EMU);
    menu_crt_shader1.push_back(ENTRY_CRT_SHADER_MODE);
    menu_crt_shader1.push_back(ENTRY_CRT_SHAPE_SETTINGS);
    menu_crt_shader1.push_back(ENTRY_MASK_SETTINGS);
    menu_crt_shader1.push_back(ENTRY_CRT_SHADER2);
    menu_crt_shader1.push_back(ENTRY_BACK);

    // jjp - the following settings provide adjustment of shape-mask, warp and vignette
    menu_crt_shape_settings.push_back(ENTRY_CRT_SHAPE);
    menu_crt_shape_settings.push_back(ENTRY_VIGNETTE);
    menu_crt_shape_settings.push_back(ENTRY_WARPX);
    menu_crt_shape_settings.push_back(ENTRY_WARPY);
    menu_crt_shape_settings.push_back(ENTRY_BACK);

    // JJP - the following settings configure the shadow-mask effect
    menu_crt_mask_settings.push_back(ENTRY_SHADOW_MASK);
    menu_crt_mask_settings.push_back(ENTRY_MASK_DIM);
    menu_crt_mask_settings.push_back(ENTRY_MASK_BOOST);
    menu_crt_mask_settings.push_back(ENTRY_MASK_SIZE);
    menu_crt_mask_settings.push_back(ENTRY_SCANLINES);
    menu_crt_mask_settings.push_back(ENTRY_BACK);

    // JJP - the following settings enable adjustment of colour curves
    menu_crt_shader2.push_back(ENTRY_NOISE);
    menu_crt_shader2.push_back(ENTRY_DESATURATE);
    menu_crt_shader2.push_back(ENTRY_DESATURATE_EDGES);
    menu_crt_shader2.push_back(ENTRY_BRIGHTNESS_BOOST);
    menu_crt_shader2.push_back(ENTRY_BACK);

    // JJP - the following settings configure the Blargg filter
    menu_blargg_filter.push_back(ENTRY_BLARGG);
    menu_blargg_filter.push_back(ENTRY_SATURATION);
    menu_blargg_filter.push_back(ENTRY_CONTRAST);
    menu_blargg_filter.push_back(ENTRY_BRIGHTNESS);
    menu_blargg_filter.push_back(ENTRY_SHARPNESS);
    menu_blargg_filter.push_back(ENTRY_RESOLUTION);
    menu_blargg_filter.push_back(ENTRY_GAMMA);
    menu_blargg_filter.push_back(ENTRY_HUE);
    menu_blargg_filter.push_back(ENTRY_BACK);

    menu_sound.push_back(ENTRY_MUTE);
    menu_sound.push_back(ENTRY_ADVERTISE);
    menu_sound.push_back(ENTRY_PREVIEWSND);
    menu_sound.push_back(ENTRY_FIXSAMPLES);
    menu_sound.push_back(ENTRY_MUSICTEST);
    menu_sound.push_back(ENTRY_BACK);

    menu_engine.push_back(ENTRY_TIME);
    menu_engine.push_back(ENTRY_TRAFFIC);
    menu_engine.push_back(ENTRY_TRACKS);
    menu_engine.push_back(ENTRY_FREEPLAY);
    menu_engine.push_back(ENTRY_SUB_ENHANCEMENTS);
    menu_engine.push_back(ENTRY_SUB_HANDLING);
    menu_engine.push_back(ENTRY_BACK);

    menu_enhancements.push_back(ENTRY_TIMER);
    menu_enhancements.push_back(ENTRY_ATTRACT);
    menu_enhancements.push_back(ENTRY_OBJECTS);
    menu_enhancements.push_back(ENTRY_PROTOTYPE);
    menu_enhancements.push_back(ENTRY_BACK);
}

// Split into own function to handle controllers being added/removed
void Menu::populate_controls()
{
    if (menu_controls.size() > 0)
        menu_controls.clear();

    menu_controls.push_back(ENTRY_GEAR);
    if (input.gamepad) menu_controls.push_back(ENTRY_CONFIGUREGP);
    menu_controls.push_back(ENTRY_REDEFKEY);
    menu_controls.push_back(ENTRY_DSTEER);
    menu_controls.push_back(ENTRY_DPEDAL);
    menu_controls.push_back(ENTRY_BACK);

    if (menu_controls_gp.size() > 0)
        menu_controls_gp.clear();

    menu_controls_gp.push_back(ENTRY_ANALOG);
    if (input.rumble_supported) menu_controls_gp.push_back(ENTRY_RUMBLE);
    menu_controls_gp.push_back(ENTRY_REDEFJOY);
    menu_controls_gp.push_back(ENTRY_BACK);
}

// ------------------------------------------------------------------------------------------------
// Populate Menus for Genuine Cabinet Setup (via SmartyPi interface)
// ------------------------------------------------------------------------------------------------

void Menu::populate_for_cabinet()
{
    menu_main.push_back(ENTRY_PLAYGAME);
    menu_main.push_back(ENTRY_GAMEMODES);
    menu_main.push_back(ENTRY_DIPS);
    menu_main.push_back(ENTRY_EXSETTINGS);
    menu_main.push_back(ENTRY_CABTESTS);
    menu_main.push_back(ENTRY_ABOUT);

    menu_gamemodes.push_back(ENTRY_CONT);
    menu_gamemodes.push_back(ENTRY_TIMETRIAL);
    menu_gamemodes.push_back(ENTRY_BACK);

    menu_s_dips.push_back(ENTRY_S_CAB);
    menu_s_dips.push_back(ENTRY_FREEPLAY);
    menu_s_dips.push_back(ENTRY_TIME);
    menu_s_dips.push_back(ENTRY_TRAFFIC);
    menu_s_dips.push_back(ENTRY_ADVERTISE);
    menu_s_dips.push_back(ENTRY_SAVE);

    menu_s_tests.push_back(ENTRY_S_INPUTS);
    menu_s_tests.push_back(ENTRY_S_OUTPUTS);
    menu_s_tests.push_back(ENTRY_S_MOTOR);
    menu_s_tests.push_back(ENTRY_S_CRT);
    menu_s_tests.push_back(ENTRY_MUSICTEST);
    menu_s_tests.push_back(ENTRY_BACK);

    menu_s_exsettings.push_back(ENTRY_TRACKS);
    menu_s_exsettings.push_back(ENTRY_GEAR);
#ifdef COMPILE_SOUND_CODE
    menu_s_exsettings.push_back(ENTRY_MUTE);
#endif
    menu_s_exsettings.push_back(ENTRY_ENHANCE);
    menu_s_exsettings.push_back(ENTRY_SCORES);
    menu_s_exsettings.push_back(ENTRY_SAVE);

    menu_s_enhance.push_back(ENTRY_SUB_HANDLING);
    menu_s_enhance.push_back(ENTRY_PREVIEWSND);
    menu_s_enhance.push_back(ENTRY_FIXSAMPLES);
    menu_s_enhance.push_back(ENTRY_ATTRACT);
    menu_s_enhance.push_back(ENTRY_OBJECTS);
    menu_s_enhance.push_back(ENTRY_TIMER);
    menu_s_enhance.push_back(ENTRY_S_BUGS);
    menu_s_enhance.push_back(ENTRY_BACK);
}

void Menu::init(bool init_main_menu)
{
    // If we got a new high score on previous time trial, then save it!
    if (outrun.ttrial.new_high_score)
    {
        outrun.ttrial.new_high_score = false;
        ttrial->update_best_time();
    }

    outrun.select_course(false, config.engine.prototype != 0);
    video.enabled = true;
    video.sprite_layer->set_x_clip(false); // Stop clipping in wide-screen mode.
    video.sprite_layer->reset();
    video.clear_text_ram();
    video.tile_layer->restore_tiles();
    ologo.enable(LOGO_Y);

    // Setup palette, road and colours for background
    oroad.stage_lookup_off = 9;
    oinitengine.init_road_seg_master();
    opalette.setup_sky_palette();
    opalette.setup_ground_color();
    opalette.setup_road_centre();
    opalette.setup_road_stripes();
    opalette.setup_road_side();
    opalette.setup_road_colour();
    otiles.setup_palette_hud();

    oroad.init();
    oroad.road_ctrl = ORoad::ROAD_R0;
    oroad.horizon_set = 1;
    oroad.horizon_base = HORIZON_DEST + 0x100;
    oinitengine.rd_split_state = OInitEngine::SPLIT_NONE;
    oinitengine.car_increment = 0;
    oinitengine.change_width = 0;

    outrun.game_state = GS_INIT;

    if (init_main_menu)
    {
        menu_stack.clear();
        set_menu(&menu_main);
        refresh_menu();
    }

    // Reset audio, so we can play tones
    osoundint.has_booted = true;
    osoundint.init();
    cannonball::audio.clear_wav();

    frame = 0;
    message_counter = 0;

    state = STATE_MENU;
}

void Menu::tick()
{
    switch (state)
    {
        case STATE_MENU:
        case STATE_REDEFINE_KEYS:
        case STATE_REDEFINE_JOY:
            tick_ui();
            break;

        case STATE_DIAGNOSTICS:
            if (cabdiag->tick())
            {
                init(false);
                menu_back();
                refresh_menu();
            }
            break;

        case STATE_TTRIAL:
            {
                int ttrial_state = ttrial->tick();

                if (ttrial_state == TTrial::INIT_GAME)
                {
                    cannonball::state = cannonball::STATE_INIT_GAME;
                    osoundint.queue_clear();
                }
                else if (ttrial_state == TTrial::BACK_TO_MENU)
                {
                    init();
                }
            }
            break;
    }
}

void Menu::tick_ui()
{
    // Skip odd frames at 60fps
    frame++;

    video.clear_text_ram();

    if (state == STATE_MENU)
    {
        tick_menu();
        draw_menu_options();
    }
    else if (state == STATE_REDEFINE_KEYS)
    {
        redefine_keyboard();
    }
    else if (state == STATE_REDEFINE_JOY)
    {
        redefine_joystick();
    }

    // Show messages
    if (message_counter > 0)
    {
        message_counter--;
        ohud.blit_text_new(0, 1, msg.c_str(), ohud.GREY);
    }

    // Shift horizon
    if (oroad.horizon_base > HORIZON_DEST)
    {
        oroad.horizon_base -= 60 / config.fps;
        if (oroad.horizon_base < HORIZON_DEST)
            oroad.horizon_base = HORIZON_DEST;
    }
    // Advance road
    else
    {
        uint32_t scroll_speed = (config.fps == 60) ? config.menu.road_scroll_speed : config.menu.road_scroll_speed << 1;

        if (oinitengine.car_increment < scroll_speed << 16)
            oinitengine.car_increment += (1 << 14);
        if (oinitengine.car_increment > scroll_speed << 16)
            oinitengine.car_increment = scroll_speed << 16;
        uint32_t result = 0x12F * (oinitengine.car_increment >> 16);
        oroad.road_pos_change = result;
        oroad.road_pos += result;
        if (oroad.road_pos >> 16 > ROAD_END) // loop to beginning of track data
            oroad.road_pos = 0;
        oinitengine.update_road();
        oinitengine.set_granular_position();
        oroad.road_width_bak = oroad.road_width >> 16; 
        oroad.car_x_bak = -oroad.road_width_bak; 
        oinitengine.car_x_pos = oroad.car_x_bak;
    }

    // Do Animations at 30 fps
    if (config.fps != 60 || (frame & 1) == 0)
    {
        ologo.tick();
        osprites.sprite_copy();
        osprites.update_sprites();
    }

    // Draw FPS
    if (config.video.fps_count)
        ohud.draw_fps_counter(cannonball::fps_counter);

    oroad.tick();
}

void Menu::draw_menu_options()
{
    int8_t x = 0;

    // Find central column in screen.
    int8_t y = 13 + ((ROWS - 13) >> 1) - (((int)menu_selected->size() * 2) >> 1);

    for (int i = 0; i < (int) menu_selected->size(); i++)
    {
        std::string s = menu_selected->at(i);

        // Centre the menu option
        x = 20 - ((int)s.length() >> 1);
        ohud.blit_text_new(x, y, s.c_str(), ohud.GREEN);

        if (!is_text_menu)
        {
            // Draw minicar
            if (i == cursor)
                video.write_text32(ohud.translate(x - 3, y), roms.rom0.read32(TILES_MINICARS1));
            // Erase minicar from this position
            else
                video.write_text32(ohud.translate(x - 3, y), 0x20202020);
        }
        y += 2;
    }
}

// Draw a single line of text
void Menu::draw_text(std::string s)
{
    // Centre text
    int8_t x = 20 - ((int)s.length() >> 1);

    // Find central column in screen.
    int8_t y = 13 + ((ROWS - 13) >> 1) - 1;

    ohud.blit_text_new(x, y, s.c_str(), ohud.GREEN);
}

#define SELECTED(string) boost::starts_with(OPTION, string)

void Menu::tick_menu()
{
    // Tick Controls
    if (input.has_pressed(Input::DOWN) || oinputs.is_analog_l())
    {
        osoundint.queue_sound(sound::BEEP1);

        if (++cursor >= (int16_t) menu_selected->size())
            cursor = 0;
    }
    else if (input.has_pressed(Input::UP) || oinputs.is_analog_r())
    {
        osoundint.queue_sound(sound::BEEP1);

        if (--cursor < 0)
            cursor = (int)menu_selected->size() - 1;
    }
    else if (select_pressed())
    {
        // Get option that was selected
        const char* OPTION = menu_selected->at(cursor).c_str();

        if (SELECTED(ENTRY_SAVE))
        {
            display_message(config.save() ? "SETTINGS SAVED" : "ERROR SAVING SETTINGS!");
            menu_back();
        }
        else if (SELECTED(ENTRY_FIXSAMPLES))
        {
            int rom_type = !config.sound.fix_samples;

            if (roms.load_pcm_rom(rom_type == 1) == 0)
            {
                config.sound.fix_samples = rom_type;
                display_message(rom_type == 1 ? "FIXED SAMPLES LOADED" : "ORIGINAL SAMPLES LOADED");
            }
            else
            {
                display_message(rom_type == 1 ? "CANT LOAD FIXED SAMPLES" : "CANT LOAD ORIGINAL SAMPLES");
            }
        }

        if (menu_selected == &menu_main)
        {
            if (SELECTED(ENTRY_PLAYGAME))
            {
                start_game(Outrun::MODE_ORIGINAL);
                //cabdiag->set(CabDiag::STATE_MOTORT);
                //state = STATE_DIAGNOSTICS;
                return;
            }
            else if (SELECTED(ENTRY_GAMEMODES))     set_menu(&menu_gamemodes);
            else if (SELECTED(ENTRY_SETTINGS))      set_menu(&menu_settings);
            else if (SELECTED(ENTRY_ABOUT))         set_menu(&menu_about);
            else if (SELECTED(ENTRY_EXIT))          cannonball::state = cannonball::STATE_QUIT;
            else if (SELECTED(ENTRY_DIPS))          set_menu(&menu_s_dips);
            else if (SELECTED(ENTRY_CABTESTS))      set_menu(&menu_s_tests);
            else if (SELECTED(ENTRY_EXSETTINGS))    set_menu(&menu_s_exsettings);
        }
        else if (menu_selected == &menu_gamemodes)
        {
            if (SELECTED(ENTRY_ENHANCED))           start_game(Outrun::MODE_ORIGINAL, 1);
            else if (SELECTED(ENTRY_ORIGINAL))      start_game(Outrun::MODE_ORIGINAL, 2);
            else if (SELECTED(ENTRY_CONT))          set_menu(&menu_cont);
            else if (SELECTED(ENTRY_TIMETRIAL))     set_menu(&menu_timetrial);
            else if (SELECTED(ENTRY_BACK))          menu_back();
        }
        else if (menu_selected == &menu_cont)
        {
            if (SELECTED(ENTRY_START_CONT))
            {
                config.save();
                outrun.custom_traffic = config.cont_traffic;
                start_game(Outrun::MODE_CONT);
            }
            else if (SELECTED(ENTRY_TRAFFIC))
            {
                if (++config.cont_traffic > TTrial::MAX_TRAFFIC)
                    config.cont_traffic = 0;
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_timetrial)
        {
            if (SELECTED(ENTRY_START))
            {
                if (check_jap_roms())
                {
                    config.save();
                    state = STATE_TTRIAL;
                    ttrial->init();
                }
            }
            else if (SELECTED(ENTRY_LAPS))
            {
                if (++config.ttrial.laps > TTrial::MAX_LAPS)
                    config.ttrial.laps = 1;
            }
            else if (SELECTED(ENTRY_TRAFFIC))
            {
                if (++config.ttrial.traffic > TTrial::MAX_TRAFFIC)
                    config.ttrial.traffic = 0;
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_about)
        {
            menu_back();
        }
        else if (menu_selected == &menu_settings)
        {
            if (SELECTED(ENTRY_VIDEO))              set_menu(&menu_video);
            else if (SELECTED(ENTRY_SOUND))         set_menu(&menu_sound);
            else if (SELECTED(ENTRY_ENGINE))        set_menu(&menu_engine);
            else if (SELECTED(ENTRY_SCORES))        display_message(config.clear_scores() ? "SCORES CLEARED" : "NO SAVED SCORES FOUND!");
            else if (SELECTED(ENTRY_CONTROLS))
            {
                display_message(input.gamepad ? "GAMEPAD FOUND" : "NO GAMEPAD FOUND!");
                populate_controls();
                set_menu(&menu_controls);
            }
        }
        // Extra Settings Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_exsettings)
        {
            if (SELECTED(ENTRY_TRACKS))        config.engine.jap ^= 1;
            else if (SELECTED(ENTRY_GEAR))          config.controls.gear = config.controls.gear == config.controls.GEAR_PRESS ? config.controls.GEAR_AUTO : config.controls.GEAR_PRESS;
            else if (SELECTED(ENTRY_ENHANCE))       set_menu(&menu_s_enhance);
            else if (SELECTED(ENTRY_SCORES))        display_message(config.clear_scores() ? "SCORES CLEARED" : "NO SAVED SCORES FOUND!");
            else if (SELECTED(ENTRY_MUTE))
            {
                config.sound.enabled ^= 1;
                if (config.sound.enabled)
                    cannonball::audio.start_audio();
                else
                    cannonball::audio.stop_audio();
            }

        }
        // Test Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_tests)
        {
            if (SELECTED(ENTRY_S_MOTOR))
            {
                set_menu(&menu_s_tests); // dummy (just store cursor for menu_back)
                cabdiag->set(CabDiag::STATE_MOTORT);
                state = STATE_DIAGNOSTICS; return;
            }
            else if (SELECTED(ENTRY_S_INPUTS))
            {
                set_menu(&menu_s_tests); // dummy (just store cursor for menu_back)
                cabdiag->set(CabDiag::STATE_INPUT);
                state = STATE_DIAGNOSTICS; return;
            }
            else if (SELECTED(ENTRY_S_OUTPUTS))
            {
                set_menu(&menu_s_tests); // dummy (just store cursor for menu_back)
                cabdiag->set(CabDiag::STATE_OUTPUT);
                state = STATE_DIAGNOSTICS; return;
            }
            else if (SELECTED(ENTRY_S_CRT))
            {
                set_menu(&menu_s_tests); // dummy (just store cursor for menu_back)
                cabdiag->set(CabDiag::STATE_CRT);
                state = STATE_DIAGNOSTICS; return;
            }
            else if (SELECTED(ENTRY_MUSICTEST))
            {
                music_track = 0;
                set_menu(&menu_musictest);
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        // DIP Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_dips)
        {
            if (SELECTED(ENTRY_S_CAB))
            {
                if (++config.smartypi.cabinet > config.CABINET_MINI)
                    config.smartypi.cabinet = config.CABINET_MOVING;
            }
            else if (SELECTED(ENTRY_FREEPLAY))      config.engine.freeplay = !config.engine.freeplay;
            else if (SELECTED(ENTRY_TIME))          config.inc_time();
            else if (SELECTED(ENTRY_TRAFFIC))       config.inc_traffic();
            else if (SELECTED(ENTRY_ADVERTISE))     config.sound.advertise ^= 1;
        }
        // Enahnce Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_enhance)
        {
            if (SELECTED(ENTRY_SUB_HANDLING))       set_menu(&menu_handling);
            else if (SELECTED(ENTRY_PREVIEWSND))    config.sound.preview ^= 1;
            else if (SELECTED(ENTRY_ATTRACT))       config.engine.new_attract ^= 1;
            else if (SELECTED(ENTRY_OBJECTS))       config.engine.level_objects ^= 1;
            else if (SELECTED(ENTRY_PROTOTYPE))     config.engine.prototype ^= 1;
            else if (SELECTED(ENTRY_S_BUGS))        config.engine.fix_bugs ^= 1;
            else if (SELECTED(ENTRY_TIMER))         config.engine.fix_timer ^= 1;
            else if (SELECTED(ENTRY_BACK))          menu_back();
        }

        // Video Settings
        else if (menu_selected == &menu_video)
        {
            if (SELECTED(ENTRY_SCALE))
            {
                if (++config.video.scale == 4)
                    config.video.scale = 1; // 1..3
                //restart_video();
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_FPS_COUNTER))
            {
                config.video.fps_count ^= 1; // cycle between 0 and 1
            }
            else if (SELECTED(ENTRY_FULLSCREEN))
            {
                if (++config.video.mode > video_settings_t::MODE_STRETCH)
                    config.video.mode = video.supports_window() ? video_settings_t::MODE_WINDOW : video_settings_t::MODE_WINDOW + 1;
                // restart_video();
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_WIDESCREEN))
            {
                config.video.widescreen ^= 1;
                // restart_video();
                config.videoRestartRequired = true;
            }
            // JJP - X and Y position control
            else if (SELECTED(ENTRY_X_OFFSET))
            {
                if ((config.video.x_offset+=5) > 100)
                    config.video.x_offset = -100;
            }
            else if (SELECTED(ENTRY_Y_OFFSET))
            {
                if ((config.video.y_offset+=5) > 100)
                    config.video.y_offset = -100;
            }
            // JJP - CRT emulation related sub-menus
            else if (SELECTED(ENTRY_CRT_SHADER1))
                set_menu(&menu_crt_shader1);
            else if (SELECTED(ENTRY_BLARGG_FILTER))
                set_menu(&menu_blargg_filter);
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader settings (page 1)
        else if (menu_selected == &menu_crt_shader1)
        {
            if (SELECTED(ENTRY_CRT_S16_EMU))
            {
                // s16accuracy. 0 = fast, 1 = accurate
                // affects glowy edges around sprites in shadow
                config.video.s16accuracy ^= 1;
            }
            else if (SELECTED(ENTRY_CRT_SHADER_MODE))
            {
                // 0 = off (actually pass-through), 1 = Fast, 2 = Full
                config.video.shader_mode += 1;
                switch (config.video.shader_mode) {
                    case 1 :    // user has enabled 'Fast' shader. If shadow_mask is enabled, set it to 'Overlay'
                                if (config.video.shadow_mask == 2) config.video.shadow_mask = 1;
                                break;
                    case 2 :    // user has enabled 'Full' shader. If shadow_mask is enabled, set to 'Shader'
                                if (config.video.shadow_mask == 1) config.video.shadow_mask = 1;
                                break;
                    case 3 :    // wrap
                                config.video.shader_mode = 0;
                                // user has set shader to 'None' - If shadow_mask is enabled, set this 'Overlay'
                                if (config.video.shadow_mask != 0) config.video.shadow_mask = 1;
                                break;
                }
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_CRT_SHAPE_SETTINGS))
                set_menu(&menu_crt_shape_settings);
            else if (SELECTED(ENTRY_MASK_SETTINGS))
                set_menu(&menu_crt_mask_settings);
            else if (SELECTED(ENTRY_CRT_SHADER2))
                set_menu(&menu_crt_shader2);
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader shadown mask settings
        else if (menu_selected == &menu_crt_mask_settings) {
            // JJP - CRT shader related settings and options
            if (SELECTED(ENTRY_SHADOW_MASK))
            {
                // defines the type of mask shown
                config.video.shadow_mask += 1;
                switch (config.video.shadow_mask) {
                    case 1 :    // user has enabled 'overlay' shadow mask.
                                // Can be applied with any shader option so nothing to change.
                                break;
                    case 2 :    // user has enabled 'shader' shadow mask. Set shader to 'Full'
                                config.video.shader_mode = 2;
                                break;
                    case 3 :    //wrap (user has selected no shadow mask)
                                config.video.shadow_mask = 0;
                                break;
                }
                // restart is always needed as the mask overlay needs to be rebuilt regardless
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_MASK_DIM))
            {
                config.video.maskDim -= 5;
                if (config.video.maskDim < 0)
                    config.video.maskDim = 100;
                if (config.video.shadow_mask==1)
                    config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_MASK_BOOST))
            {
                config.video.maskBoost += 5;
                if (config.video.maskBoost > 150)
                    config.video.maskBoost = 100;
                if (config.video.shader_mode==0)
                    display_message("Mask Boost requires Full shader");
            }
            else if (SELECTED(ENTRY_MASK_SIZE))
            {
                config.video.mask_size += 1;
                if (config.video.mask_size > 3)
                    config.video.mask_size = 1;
                if (config.video.shader_mode!=2)
                    display_message("Mask Size requires Full shader");
            }
            else if (SELECTED(ENTRY_SCANLINES))
            {
                config.video.scanlines += 1;
                if (config.video.scanlines > 3)
                    config.video.scanlines = 0;
                else if (config.video.shader_mode==0) {
                    // enable fast shader, looks terrible without it
                    config.video.shader_mode = 1;
                    config.videoRestartRequired = true;
                }
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader shadown mask settings
        else if (menu_selected == &menu_crt_shape_settings) {
            if (SELECTED(ENTRY_CRT_SHAPE))
            {
                // Curved edge effect. Actually applied as an SDL texture but from a user perspect fits better here.
                config.video.crt_shape ^= 1;
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_VIGNETTE))
            {
                config.video.vignette += 5;
                if (config.video.vignette > 40)
                    config.video.vignette = 0;
                if (config.video.shader_mode < 2)
                    config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_WARPX))
            {
                config.video.warpX += 1;
                if (config.video.warpX > 10)
                    config.video.warpX = 0;
                if (config.video.shader_mode==0)
                    display_message("Warp requires shader");
            }
            else if (SELECTED(ENTRY_WARPY))
            {
                config.video.warpY += 1;
                if (config.video.warpY > 10)
                    config.video.warpY = 0;
                if (config.video.shader_mode==0)
                    display_message("Warp requires shader");
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader settings (noise/desaturation/brightness boost)
        else if (menu_selected == &menu_crt_shader2) {
            if (SELECTED(ENTRY_NOISE))
            {
                config.video.noise += 1;
                if (config.video.noise > 20)
                    config.video.noise = 0;
                if (config.video.shader_mode==0)
                    display_message("Noise requires shader");
            }
            else if (SELECTED(ENTRY_DESATURATE))
            {
                config.video.desaturate += 1;
                if (config.video.desaturate > 10)
                    config.video.desaturate = 0;
                if (config.video.shader_mode!=2)
                    display_message("Desaturate requires Full shader");
            }
            else if (SELECTED(ENTRY_DESATURATE_EDGES))
            {
                config.video.desaturate_edges += 1;
                if (config.video.desaturate_edges > 10)
                    config.video.desaturate_edges = 0;
                if (config.video.shader_mode!=2)
                    display_message("Desaturate requires Full shader");
            }
            else if (SELECTED(ENTRY_BRIGHTNESS_BOOST))
            {
                config.video.brightboost += 1;
                if (config.video.brightboost > 10)
                    config.video.brightboost = 0;
                if (config.video.shader_mode==0)
                    display_message("Boost requires shader");
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - Blargg CRT filtering settings
        else if (menu_selected == &menu_blargg_filter)
        {
            if (SELECTED(ENTRY_BLARGG)) // off/componsite/s-video/rgb/mono
            {
                config.video.blargg++;
                if (config.video.blargg > video_settings_t::BLARGG_RGB) {
                    // disable filter; requires video restart
                    config.video.blargg = video_settings_t::BLARGG_DISABLE;
                    // restart_video();
                    config.videoRestartRequired = true;
                }
                if (config.video.blargg == video_settings_t::BLARGG_COMPOSITE)
                    // enable filter; requires video restart
                    // restart_video();
                    config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_SATURATION))
            {
                config.video.saturation += 10;
                if (config.video.saturation > 50)
                    config.video.saturation = -50;
            }
            else if (SELECTED(ENTRY_CONTRAST))
            {
                config.video.contrast += 10;
                if (config.video.contrast > 50)
                    config.video.contrast = -50;
            }
            else if (SELECTED(ENTRY_BRIGHTNESS))
            {
                config.video.brightness += 10;
                if (config.video.brightness > 50)
                    config.video.brightness = -50;
            }
            else if (SELECTED(ENTRY_SHARPNESS))
            {
                config.video.sharpness += 10;
                if (config.video.sharpness > 50)
                    config.video.sharpness = -50;
            }
            else if (SELECTED(ENTRY_RESOLUTION))
            {
                config.video.resolution += 10;
                if (config.video.resolution > 0)
                    config.video.resolution = -100;
            }
            else if (SELECTED(ENTRY_GAMMA))
            {
                config.video.gamma += 1;
                if (config.video.gamma > 10)
                    config.video.gamma = -20;
            }
            else if (SELECTED(ENTRY_HUE))
            {
                config.video.hue += 1;
                if (config.video.hue > 10)
                    config.video.hue = -10;
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        else if (menu_selected == &menu_sound)
        {
            if (SELECTED(ENTRY_MUTE))
            {
                config.sound.enabled ^= 1;
                if (config.sound.enabled)
                    cannonball::audio.start_audio();
                else
                    cannonball::audio.stop_audio();
            }
            else if (SELECTED(ENTRY_ADVERTISE))
                config.sound.advertise ^= 1;
            else if (SELECTED(ENTRY_PREVIEWSND))
                config.sound.preview ^= 1;
            else if (SELECTED(ENTRY_MUSICTEST))
            {
                music_track = 0;
                set_menu(&menu_musictest);
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_controls)
        {
            if (SELECTED(ENTRY_GEAR))
            {
                if (++config.controls.gear > config.controls.GEAR_AUTO)
                    config.controls.gear = config.controls.GEAR_BUTTON;
            }
            else if (SELECTED(ENTRY_CONFIGUREGP))
                set_menu(&menu_controls_gp);
            else if (SELECTED(ENTRY_REDEFKEY))
            {
                display_message("PRESS MENU TO END AT ANY STAGE");
                state = STATE_REDEFINE_KEYS;
                redef_state = 0;
                input.key_press = -1;
            }
            else if (SELECTED(ENTRY_DSTEER))
            {
                if (++config.controls.steer_speed > 9)
                    config.controls.steer_speed = 1;
            }
            else if (SELECTED(ENTRY_DPEDAL))
            {
                if (++config.controls.pedal_speed > 9)
                    config.controls.pedal_speed = 1;
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_controls_gp)
        {
            if (SELECTED(ENTRY_ANALOG))
            {
                if (++config.controls.analog == 3)
                    config.controls.analog = 0;
                input.analog = config.controls.analog;
            }
            else if (SELECTED(ENTRY_RUMBLE))
            {
                config.controls.rumble += 0.25f;
                if (config.controls.rumble > 1.0f) config.controls.rumble = 0;
            }
            else if (SELECTED(ENTRY_REDEFJOY))
            {
                //display_message("PRESS MENU TO END AT ANY STAGE");
                state = STATE_REDEFINE_JOY;
                redef_state = 0;
                input.joy_button = -1;
                input.reset_axis_config();
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_engine)
        {
            if (SELECTED(ENTRY_TRACKS))                 config.engine.jap ^= 1;
            else if (SELECTED(ENTRY_TIME))              config.inc_time();
            else if (SELECTED(ENTRY_TRAFFIC))           config.inc_traffic();
            else if (SELECTED(ENTRY_FREEPLAY))          config.engine.freeplay = !config.engine.freeplay;
            else if (SELECTED(ENTRY_SUB_ENHANCEMENTS))  set_menu(&menu_enhancements);
            else if (SELECTED(ENTRY_SUB_HANDLING))      set_menu(&menu_handling);
            else if (SELECTED(ENTRY_BACK))              menu_back();
        }
        else if (menu_selected == &menu_enhancements)
        {
            if (SELECTED(ENTRY_ATTRACT))                config.engine.new_attract ^= 1;
            else if (SELECTED(ENTRY_OBJECTS))           config.engine.level_objects ^= 1;
            else if (SELECTED(ENTRY_PROTOTYPE))         config.engine.prototype ^= 1;
            else if (SELECTED(ENTRY_TIMER))             config.engine.fix_timer ^= 1;
            else if (SELECTED(ENTRY_BACK))              menu_back();
        }
        else if (menu_selected == &menu_handling)
        {
            if (SELECTED(ENTRY_GRIP))                   config.engine.grippy_tyres ^= 1;
            else if (SELECTED(ENTRY_OFFROAD))           config.engine.offroad ^= 1;
            else if (SELECTED(ENTRY_BUMPER))            config.engine.bumper ^= 1;
            else if (SELECTED(ENTRY_TURBO))             config.engine.turbo ^= 1;
            else if (SELECTED(ENTRY_COLOR))             { if (++config.engine.car_pal > 4) config.engine.car_pal = 0; }
            else if (SELECTED(ENTRY_BACK))              menu_back();
        }
        else if (menu_selected == &menu_musictest)
        {
            if (SELECTED(ENTRY_MUSIC1))
            {
                osoundint.queue_sound(sound::FM_RESET);

                // Last Wave
                if (music_track == config.sound.music.size())
                {
                    cannonball::audio.clear_wav();
                    osoundint.queue_sound(sound::MUSIC_LASTWAVE);
                }
                // Everything Else
                else
                    omusic.play_music(music_track);
            }
            else if (SELECTED(ENTRY_MUSIC2))
            {
                if (++music_track > config.sound.music.size()) music_track = 0;
            }
            else if (SELECTED(ENTRY_WAVEVOLUME))
            {
                // gain applied to .wav file playback, 1-8 in 2dB steps where 5=0dB (as recorded)
                if (++config.sound.wave_volume > 8) config.sound.wave_volume = 1;
            }
            else if (SELECTED(ENTRY_CALLBACK_RATE))
            {
                // SDL Audio callback rate. 0=8ms (default), 1=16ms (which enables smooth audio under WSL)
                if (++config.sound.callback_rate > 1) config.sound.callback_rate = 0;
                // changing this setting requires re-initialising the audio subsystem
                cannonball::audio.stop_audio();
                cannonball::audio.init();
            }
            else if (SELECTED(ENTRY_BACK))
            {
                cannonball::audio.clear_wav();
                osoundint.queue_sound(sound::FM_RESET);
                menu_back();
            }
        }
        else
            set_menu(&menu_main);

        osoundint.queue_sound(sound::BEEP1);
    }
    refresh_menu();
}

bool Menu::select_pressed()
{
    // On a real cabinet, use START button or Accelerator to select
    if (config.smartypi.enabled)
        return input.has_pressed(Input::START) || oinputs.is_analog_select();
    // On a Joystick, use START, Digital Accelerate or Gear (as it's probably mapped to Button A)
    else
        return input.has_pressed(Input::START) || input.has_pressed(Input::ACCEL) || input.has_pressed(Input::GEAR1);
}

void Menu::set_menu(std::vector<std::string> *menu)
{
    menu_pair m;
    m.cursor = cursor;
    m.menu   = menu_selected;
    menu_stack.push_back(m);

    menu_selected = menu;
    cursor = 0;
    is_text_menu = (menu == &menu_about);
}

void Menu::menu_back()
{
    if (!menu_stack.empty())
    {
        menu_pair m = menu_stack.back();
        cursor = m.cursor;
        menu_selected = m.menu;
        menu_stack.pop_back();
    }
    is_text_menu = (menu_selected == &menu_about);
}


// ------------------------------------------------------------------------------------------------
// Refresh menu options with latest config data
// ------------------------------------------------------------------------------------------------
void Menu::refresh_menu()
{
    int16_t cursor_backup = cursor;
    std::string s; // JJP

    for (cursor = 0; cursor < (int) menu_selected->size(); cursor++)
    {
        // Get option that was selected
        const char* OPTION = menu_selected->at(cursor).c_str();

        if (menu_selected == &menu_about)  // JJP - include machine stats in about menu
        {
            auto message = "     " + Utils::to_string(config.stats.playcount) + " PLAYS ,  " +
                           Utils::to_string((config.stats.runtime / 60)) + " MACHINE HOURS";
            display_message(message);
            //if (SELECTED(ENTRY_TOTAL_PLAYS))        set_menu_text(ENTRY_TOTAL_PLAYS, Utils::to_string(config.stats.playcount));
            //else if (SELECTED(ENTRY_RUN_TIME))      set_menu_text(ENTRY_RUN_TIME, Utils::to_string((config.stats.runtime / 60)));
        }
        else if (menu_selected == &menu_timetrial)
        {
            if (SELECTED(ENTRY_LAPS))               set_menu_text(ENTRY_LAPS, Utils::to_string(config.ttrial.laps));
            else if (SELECTED(ENTRY_TRAFFIC))       set_menu_text(ENTRY_TRAFFIC, config.ttrial.traffic == 0 ? "DISABLED" : Utils::to_string(config.ttrial.traffic));
        }
        else if (menu_selected == &menu_cont)
        {
            if (SELECTED(ENTRY_TRAFFIC))            set_menu_text(ENTRY_TRAFFIC, config.cont_traffic == 0 ? "DISABLED" : Utils::to_string(config.cont_traffic));
        }
        else if (menu_selected == &menu_video)
        {
            if (SELECTED(ENTRY_FPS_COUNTER))        set_menu_text(ENTRY_FPS_COUNTER, config.video.fps_count ? "ON" : "OFF");
            else if (SELECTED(ENTRY_FULLSCREEN))    set_menu_text(ENTRY_FULLSCREEN, VIDEO_LABELS[config.video.mode]);
            else if (SELECTED(ENTRY_WIDESCREEN))    set_menu_text(ENTRY_WIDESCREEN, config.video.widescreen ? "ON" : "OFF");
            else if (SELECTED(ENTRY_SCALE))         set_menu_text(ENTRY_SCALE, Utils::to_string(config.video.scale) + "X");
            else if (SELECTED(ENTRY_X_OFFSET))      set_menu_text(ENTRY_X_OFFSET, Utils::to_string(config.video.x_offset));
            else if (SELECTED(ENTRY_Y_OFFSET))      set_menu_text(ENTRY_Y_OFFSET, Utils::to_string(config.video.y_offset));
        }
        else if (menu_selected == &menu_sound)
        {
            if (SELECTED(ENTRY_MUTE))               set_menu_text(ENTRY_MUTE, config.sound.enabled ? "ON" : "OFF");
            else if (SELECTED(ENTRY_ADVERTISE))     set_menu_text(ENTRY_ADVERTISE, config.sound.advertise ? "ON" : "OFF");
            else if (SELECTED(ENTRY_PREVIEWSND))    set_menu_text(ENTRY_PREVIEWSND, config.sound.preview ? "ON" : "OFF");
            else if (SELECTED(ENTRY_FIXSAMPLES))    set_menu_text(ENTRY_FIXSAMPLES, config.sound.fix_samples ? "ON" : "OFF");
        }
        // JJP CRT Emulation related menus
        // Note - no entry here for menu_crt_shader1 as it only lines to other menus
        else if (menu_selected == &menu_crt_shader1)
        {
            // Screen feedback for selected options
            if (SELECTED(ENTRY_CRT_S16_EMU))
            {
                if (config.video.s16accuracy == 0)
                    set_menu_text(ENTRY_CRT_S16_EMU, "FAST");
                else
                    set_menu_text(ENTRY_CRT_S16_EMU, "ACCURATE");
            }
            else if (SELECTED(ENTRY_CRT_SHADER_MODE))
            {
                if (config.video.shader_mode == 0)
                    set_menu_text(ENTRY_CRT_SHADER_MODE, "NONE");
                else if (config.video.shader_mode == 1)
                    set_menu_text(ENTRY_CRT_SHADER_MODE, "FAST");
                else
                    set_menu_text(ENTRY_CRT_SHADER_MODE, "FULL");
            }
        }
        else if (menu_selected == &menu_crt_shape_settings)
        {
            if (SELECTED(ENTRY_CRT_SHAPE))
                set_menu_text(ENTRY_CRT_SHAPE, config.video.crt_shape ? "ON" : "OFF");
            else if (SELECTED(ENTRY_VIGNETTE))
                set_menu_text(ENTRY_VIGNETTE, config.video.vignette ? Utils::to_string(config.video.vignette) +"%": "OFF");
            else if (SELECTED(ENTRY_WARPX))
                set_menu_text(ENTRY_WARPX, Utils::to_string(config.video.warpX) + "%");
            else if (SELECTED(ENTRY_WARPY))
                set_menu_text(ENTRY_WARPY, Utils::to_string(config.video.warpY) + "%");
        }
        else if (menu_selected == &menu_crt_mask_settings)
        {
            if (SELECTED(ENTRY_SHADOW_MASK))
            {
                if (config.video.shadow_mask == 0)
                    set_menu_text(ENTRY_SHADOW_MASK, "OFF");
                else if (config.video.shadow_mask == 1)
                    set_menu_text(ENTRY_SHADOW_MASK, "OVERLAY");
                else
                    set_menu_text(ENTRY_SHADOW_MASK, "SHADER");
            }
            else if (SELECTED(ENTRY_MASK_DIM))
                set_menu_text(ENTRY_MASK_DIM, Utils::to_string(config.video.maskDim) + "%");
            else if (SELECTED(ENTRY_MASK_BOOST))
                set_menu_text(ENTRY_MASK_BOOST, Utils::to_string(config.video.maskBoost) + "%");
            else if (SELECTED(ENTRY_MASK_SIZE))
                set_menu_text(ENTRY_MASK_SIZE, Utils::to_string(config.video.mask_size) + "px");
            else if (SELECTED(ENTRY_SCANLINES)) {
                std::string s;
                switch (config.video.scanlines) {
                    case 0: s = "OFF";      break;
                    case 1: s = "LOW";      break;
                    case 2: s = "MEDIUM";   break;
                    case 3: s = "HIGH";     break;
                }
                set_menu_text(ENTRY_SCANLINES, s);
            }
        }
        else if (menu_selected == &menu_crt_shader2)
        {
            if (SELECTED(ENTRY_NOISE))
                set_menu_text(ENTRY_NOISE, Utils::to_string(config.video.noise));
            else if (SELECTED(ENTRY_DESATURATE))
                set_menu_text(ENTRY_DESATURATE,
                    config.video.desaturate ? Utils::to_string(config.video.desaturate) + "%" : "OFF");
            else if (SELECTED(ENTRY_DESATURATE_EDGES))
                set_menu_text(ENTRY_DESATURATE_EDGES,
                    config.video.desaturate_edges ? Utils::to_string(config.video.desaturate_edges) + "%" : "OFF");
            else if (SELECTED(ENTRY_BRIGHTNESS_BOOST))
                set_menu_text(ENTRY_BRIGHTNESS_BOOST,
                    config.video.brightboost ? Utils::to_string(config.video.brightboost) + "%" : "OFF");
        }
        else if (menu_selected == &menu_blargg_filter)
        {
            // Screen feedback for selected options for the blargg filtering
            if (SELECTED(ENTRY_BLARGG))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)        s = "OFF";
                else if (config.video.blargg == video_settings_t::BLARGG_COMPOSITE) s = "COMPOSITE";
                else if (config.video.blargg == video_settings_t::BLARGG_SVIDEO)    s = "S-VIDEO";
                else if (config.video.blargg == video_settings_t::BLARGG_RGB)       s = "ARCADE RGB";
                set_menu_text(ENTRY_BLARGG, s);
            }
            else if (SELECTED(ENTRY_SATURATION))
                set_menu_text(ENTRY_SATURATION, Utils::to_string(config.video.saturation));
            else if (SELECTED(ENTRY_CONTRAST))
                set_menu_text(ENTRY_CONTRAST, Utils::to_string(config.video.contrast));
            else if (SELECTED(ENTRY_BRIGHTNESS))
                set_menu_text(ENTRY_BRIGHTNESS, Utils::to_string(config.video.brightness));
            else if (SELECTED(ENTRY_SHARPNESS))
                set_menu_text(ENTRY_SHARPNESS, Utils::to_string(config.video.sharpness));
            else if (SELECTED(ENTRY_RESOLUTION))
                set_menu_text(ENTRY_RESOLUTION, Utils::to_string(config.video.resolution));
            else if (SELECTED(ENTRY_GAMMA)) {
                if (config.video.gamma >= 30)
                    set_menu_text(ENTRY_GAMMA, "3." + Utils::to_string(config.video.gamma - 30));
                else if (config.video.gamma >= 20)
                    set_menu_text(ENTRY_GAMMA, "2." + Utils::to_string(config.video.gamma - 20));
                else if (config.video.gamma >= 10)
                    set_menu_text(ENTRY_GAMMA, "1." + Utils::to_string(config.video.gamma - 10));
                else if (config.video.gamma >= 0)
                    set_menu_text(ENTRY_GAMMA, "0." + Utils::to_string(config.video.gamma));
                else if (config.video.gamma > -10)
                    set_menu_text(ENTRY_GAMMA, "-0." + Utils::to_string((config.video.gamma * -1)));
                else if (config.video.gamma > -20)
                    set_menu_text(ENTRY_GAMMA, "-1." + Utils::to_string((config.video.gamma * -1) - 10));
                else if (config.video.gamma > -30)
                    set_menu_text(ENTRY_GAMMA, "-2." + Utils::to_string((config.video.gamma * -1) - 20));
                else
                    set_menu_text(ENTRY_GAMMA, "-3." + Utils::to_string((config.video.gamma * -1) - 30));
            }
            else if (SELECTED(ENTRY_HUE)) {
                if (config.video.hue >= 10)
                    set_menu_text(ENTRY_HUE, "0.1" + Utils::to_string(config.video.hue - 10));
                else if (config.video.hue >= 0)
                    set_menu_text(ENTRY_HUE, "0.0" + Utils::to_string(config.video.hue));
                else if (config.video.hue > -10)
                    set_menu_text(ENTRY_HUE, "-0.0" + Utils::to_string((config.video.hue * -1)));
                else // (config.video.hue > -20)
                    set_menu_text(ENTRY_HUE, "-0.1" + Utils::to_string((config.video.hue * -1) - 10));
            }
        }
        // JJP end of insert

        else if (menu_selected == &menu_controls)
        {
            if (SELECTED(ENTRY_GEAR))               set_menu_text(ENTRY_GEAR, GEAR_LABELS[config.controls.gear]);
            else if (SELECTED(ENTRY_DSTEER))        set_menu_text(ENTRY_DSTEER, Utils::to_string(config.controls.steer_speed));
            else if (SELECTED(ENTRY_DPEDAL))        set_menu_text(ENTRY_DPEDAL, Utils::to_string(config.controls.pedal_speed));
        }
        else if (menu_selected == &menu_controls_gp)
        {
            if (SELECTED(ENTRY_ANALOG))             set_menu_text(ENTRY_ANALOG, ANALOG_LABELS[config.controls.analog]);
            else if (SELECTED(ENTRY_RUMBLE))        set_menu_text(ENTRY_RUMBLE, RUMBLE_LABELS[(int)(config.controls.rumble / 0.25f)]);
        }
        else if (menu_selected == &menu_engine || menu_selected == &menu_s_dips)
        {
            if (SELECTED(ENTRY_TRACKS))             set_menu_text(ENTRY_TRACKS, config.engine.jap ? "JAPAN" : "WORLD");
            else if (SELECTED(ENTRY_TIME))          set_menu_text(ENTRY_TIME, config.engine.freeze_timer? "DISABLED" : DIP_DIFFICULTY[config.engine.dip_time]);
            else if (SELECTED(ENTRY_TRAFFIC))       set_menu_text(ENTRY_TRAFFIC, config.engine.disable_traffic ? "DISABLED" : DIP_DIFFICULTY[config.engine.dip_traffic]);
            else if (SELECTED(ENTRY_OBJECTS))       set_menu_text(ENTRY_OBJECTS, config.engine.level_objects ? "ENHANCED" : "ORIGINAL");
            else if (SELECTED(ENTRY_PROTOTYPE))     set_menu_text(ENTRY_PROTOTYPE, config.engine.prototype ? "ON" : "OFF");
            else if (SELECTED(ENTRY_ATTRACT))       set_menu_text(ENTRY_ATTRACT, config.engine.new_attract ? "ON" : "OFF");
            else if (SELECTED(ENTRY_S_CAB))         set_menu_text(ENTRY_S_CAB, CAB_LABELS[config.smartypi.cabinet]);
            else if (SELECTED(ENTRY_FREEPLAY))      set_menu_text(ENTRY_FREEPLAY, config.engine.freeplay ? "ON" : "OFF");
            else if (SELECTED(ENTRY_ADVERTISE))     set_menu_text(ENTRY_ADVERTISE, config.sound.advertise ? "ON" : "OFF");
        }
        else if (menu_selected == &menu_s_exsettings)
        {
            if (SELECTED(ENTRY_TRACKS))        set_menu_text(ENTRY_TRACKS, config.engine.jap ? "JAPAN" : "WORLD");
            else if (SELECTED(ENTRY_GEAR))          set_menu_text(ENTRY_GEAR, GEAR_LABELS[config.controls.gear]);
            else if (SELECTED(ENTRY_MUTE))          set_menu_text(ENTRY_MUTE, config.sound.enabled ? "ON" : "OFF");
        }
        else if (menu_selected == &menu_enhancements || menu_selected == &menu_s_enhance)
        {
            if (SELECTED(ENTRY_PREVIEWSND))         set_menu_text(ENTRY_PREVIEWSND, config.sound.preview ? "ON" : "OFF");
            else if (SELECTED(ENTRY_FIXSAMPLES))    set_menu_text(ENTRY_FIXSAMPLES, config.sound.fix_samples ? "ON" : "OFF");
            else if (SELECTED(ENTRY_ATTRACT))       set_menu_text(ENTRY_ATTRACT, config.engine.new_attract ? "ON" : "OFF");
            else if (SELECTED(ENTRY_OBJECTS))       set_menu_text(ENTRY_OBJECTS, config.engine.level_objects ? "ENHANCED" : "ORIGINAL");
            else if (SELECTED(ENTRY_PROTOTYPE))     set_menu_text(ENTRY_PROTOTYPE, config.engine.prototype ? "ON" : "OFF");
            else if (SELECTED(ENTRY_S_BUGS))        set_menu_text(ENTRY_S_BUGS, config.engine.fix_bugs ? "ON" : "OFF");
            else if (SELECTED(ENTRY_TIMER))         set_menu_text(ENTRY_TIMER, config.engine.fix_timer ? "ON" : "OFF");
        }
        else if (menu_selected == &menu_handling)
        {
            if (SELECTED(ENTRY_GRIP))               set_menu_text(ENTRY_GRIP, config.engine.grippy_tyres ? "ON" : "OFF");
            else if (SELECTED(ENTRY_OFFROAD))       set_menu_text(ENTRY_OFFROAD, config.engine.offroad ? "ON" : "OFF");
            else if (SELECTED(ENTRY_BUMPER))        set_menu_text(ENTRY_BUMPER, config.engine.bumper ? "ON" : "OFF");
            else if (SELECTED(ENTRY_TURBO))         set_menu_text(ENTRY_TURBO, config.engine.turbo ? "ON" : "OFF");
            else if (SELECTED(ENTRY_COLOR))         set_menu_text(ENTRY_COLOR, COLOR_LABELS[config.engine.car_pal]);
        }
        else if (menu_selected == &menu_musictest)
        {
            if (SELECTED(ENTRY_MUSIC2))             set_menu_text(ENTRY_MUSIC2, music_track >= config.sound.music.size() ? ENTRY_MUSIC3 : config.sound.music.at(music_track).title);
            else if (SELECTED(ENTRY_WAVEVOLUME)) {
                // gain applied to .wav file playback, 1-8 in 2dB steps where 5=0dB (as recorded)
                // present dB values in the UI
                set_menu_text(ENTRY_WAVEVOLUME,
                    Utils::to_string(config.sound.wave_volume*2-10) + "dB"
                );
            }
            else if (SELECTED(ENTRY_CALLBACK_RATE)) {
                // Allows the user to switch being 8ms (default) and 16ms callback rate.
                set_menu_text(ENTRY_CALLBACK_RATE, (config.sound.callback_rate==0 ? "8ms" : "16ms"));
            }
        }
    }
    cursor = cursor_backup;
}

// Append Menu Text For A Particular Menu Entry
void Menu::set_menu_text(std::string s1, std::string s2)
{
    s1.append(s2);
    menu_selected->erase(menu_selected->begin() + cursor);
    menu_selected->insert(menu_selected->begin() + cursor, s1);
}

void Menu::redefine_keyboard()
{
    if (redef_state == 7 && config.controls.gear != config.controls.GEAR_SEPARATE) // Skip redefine of second gear press
        redef_state++;

    switch (redef_state)
    {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
            if (input.has_pressed(Input::MENU))
            {
                message_counter = 0;
                state = STATE_MENU;
            }
            else
            {
                draw_text(text_redefine.at(redef_state));
                if (input.key_press != -1)
                {
                    config.controls.keyconfig[redef_state] = input.key_press;
                    redef_state++;
                    input.key_press = -1;
                }
            }
            break;

        case 12:
            state = STATE_MENU;
            break;
    }
}

void Menu::redefine_joystick()
{
    if (redef_state == 3 && config.controls.gear != config.controls.GEAR_SEPARATE) // Skip redefine of second gear press
        redef_state++;

    switch (redef_state)
    {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            draw_text(text_redefine.at(redef_state + 4));
            // Analog controls enabled (Accelerator & Brake): Read axis being pressed
            if (config.controls.analog == 1 && (redef_state == 0 || redef_state == 1))
            {
                int last_axis = input.get_axis_config();

                if (last_axis != -1)
                {
                    config.controls.axis[redef_state + 1] = last_axis;
                    redef_state++;
                }
            }
            else if (input.joy_button != -1)
            {
                config.controls.padconfig[redef_state] = input.joy_button;
                redef_state++;
                input.joy_button = -1;
            }
            break;

        case 8:
            state = STATE_MENU;
            break;
    }
}

// Display a contextual message in the top left of the screen
void Menu::display_message(std::string s)
{
    msg = s;
    message_counter = MESSAGE_TIME * config.fps;
}

bool Menu::check_jap_roms()
{
    if (config.engine.jap && !roms.load_japanese_roms())
    {
        display_message("JAPANESE ROMSET NOT FOUND");
        return false;
    }
    return true;
}

// Reinitalize Video, and stop audio to avoid crackles
void Menu::restart_video()
{
//  Note: this MUST be called from the SDL rendering thread

//    if (config.sound.enabled)
//        cannonball::audio.stop_audio();

    video.disable();
    video.init(&roms, &config.video);
    config.videoRestartRequired = false;

//    osoundint.init();
//    if (config.sound.enabled)
//        cannonball::audio.start_audio();
}

void Menu::start_game(int mode, int settings)
{
    // Enhanced Settings
    if (settings == 1)
    {
        if (!config.video.hires)
        {
            if (config.video.scale > 1)
                config.video.scale >>= 1;
        }

        if (!config.sound.fix_samples)
        {
            if (roms.load_pcm_rom(true) == 0)
                config.sound.fix_samples = 1;
        }

        config.set_fps(config.video.fps = 2);
        config.video.widescreen     = 1;
        config.video.hires          = 1;
        config.engine.level_objects = 1;
        config.engine.new_attract   = 1;
        config.engine.fix_bugs      = 1;
        config.sound.preview        = 1;

        // restart_video();
        config.videoRestartRequired = true;
    }
    // Original Settings
    else if (settings == 2)
    {
        if (config.video.hires)
        {
            config.video.scale <<= 1;
        }

        if (config.sound.fix_samples)
        {
            if (roms.load_pcm_rom(false) == 0)
                config.sound.fix_samples = 0;
        }

        config.set_fps(config.video.fps = 1);
        config.video.widescreen     = 0;
        //config.video.hires          = 0;
        config.engine.level_objects = 0;
        config.engine.new_attract   = 0;
        config.engine.fix_bugs      = 0;
        config.sound.preview        = 0;

        // restart_video();
        config.videoRestartRequired = true;
    }
    // Otherwise, use whatever is already setup...
    else
    {
        config.engine.fix_bugs = config.engine.fix_bugs_backup;
    }

    if (check_jap_roms())
    {
        outrun.cannonball_mode = mode;
        cannonball::state = cannonball::STATE_INIT_GAME;
        osoundint.queue_clear();
    }
}
