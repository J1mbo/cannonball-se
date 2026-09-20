/***************************************************************************
    Front End Menu System.

    This file is part of Cannonball.
    Copyright (c) Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
***************************************************************************/

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

#include "sdl2/rumble.hpp"

#include <iostream>
#include <cstdio>   // snprintf (Stage 4b ADVANCED HID SETUP editor row formatting)
#include <cstring>  // strlen (Stage 4b editor byte-row cursor offset)


// Drop Boost string predicates; use lightweight std helpers instead
#include <string>
#include <string_view>
#include <cctype>
#include <algorithm>

namespace detail_menu_predicates {
    // ASCII‑only, case‑insensitive compare of two characters
    constexpr unsigned char tolow(unsigned char c) { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c; }

    // Case‑insensitive equals for string views (ASCII). Keeps previous behaviour of Boost's iequals used here.
    inline bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolow(static_cast<unsigned char>(a[i])) != tolow(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // Case‑insensitive starts_with (ASCII). Mirrors prior usage of boost::starts_with in SELECTED().
    inline bool istarts_with(std::string_view s, std::string_view prefix) {
        if (prefix.size() > s.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (tolow(static_cast<unsigned char>(s[i])) != tolow(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    }
}

// Many call sites use a SELECTED(x) macro that previously relied on boost::starts_with.
// Re‑define it here to avoid touching call sites.
#ifdef SELECTED
#undef SELECTED
#endif
#define SELECTED(X) detail_menu_predicates::istarts_with(OPTION, X)

// ohud.blit_text_new only renders A-Z (lowercase is uppercased), 0-9, space, '-', '.', '%', '!',
// ',' and '/'; any other character renders as a garbage tile. Used to sanitise text built from
// SDL device/key names before it is drawn. Uppercases, drops unsupported characters (rather than
// substituting them), collapses runs of spaces, and trims the ends.
static std::string font_safe(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        char u = static_cast<char>(toupper(c));
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
            u == ' ' || u == '-' || u == '.' || u == '%' || u == '!' || u == ',' || u == '/')
            out += u;
    }
    // Collapse runs of spaces to one.
    std::string collapsed;
    collapsed.reserve(out.size());
    for (char c : out)
    {
        if (c == ' ' && !collapsed.empty() && collapsed.back() == ' ') continue;
        collapsed += c;
    }
    // Trim leading/trailing spaces.
    size_t start = collapsed.find_first_not_of(' ');
    if (start == std::string::npos) return std::string();
    size_t end = collapsed.find_last_not_of(' ');
    return collapsed.substr(start, end - start + 1);
}

// ALSA/PulseAudio device names are often "card, long description" (or just
// long). For the OUTPUT DEVICE tile: cut at the first space from the 10th
// character onward (short names like "USB AUDIO" survive untouched), else
// hard-crop long names to 15 chars with a "..." tail (the tile font has no
// ellipsis glyph), then drop any trailing space/comma the cut left behind.
static std::string short_device_label(const std::string& name)
{
    std::string s = name;
    size_t cut = std::string::npos;
    for (size_t i = 9; i < s.size(); ++i)
    {
        if (s[i] == ' ') { cut = i; break; }
    }
    if (cut != std::string::npos)
        s = s.substr(0, cut);
    else if (s.size() > 15)
        s = s.substr(0, 15) + "...";

    size_t last = s.find_last_not_of(" ,");
    return (last == std::string::npos) ? std::string() : s.substr(0, last + 1);
}


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
    menu_musictest.push_back(ENTRY_BACK);

    menu_about.push_back(std::string("CANNONBALL-SE ") + CANNONBALL_SE_VERSION);
    menu_about.push_back("");
    menu_about.push_back("CANNONBALL IS FREE AND MAY NOT BE SOLD");
    menu_about.push_back("COPYRIGHT 2012-2024 CHRIS WHITE");
    menu_about.push_back("");
    menu_about.push_back("SE COPYRIGHT 2025-2026 JAMES PEARCE");

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
    menu_settings.push_back(ENTRY_MASTER_BREAK);
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
    menu_sound.push_back(ENTRY_OUTPUT_DEVICE);
    menu_sound.push_back(ENTRY_CALLBACK_RATE);
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

    menu_enhancements.push_back(ENTRY_HIRES);
    menu_enhancements.push_back(ENTRY_SPRITERES);
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

    // Input binding matrix (Stage 3, tools/INPUT_BINDING_DESIGN.md A.4.1): CONFIGURE INPUTS (the matrix
    // editor) is always shown - the keyboard column is always editable - and replaces the old
    // input.gamepad-gated CONFIGURE GAMEPAD entry and its menu_controls_gp submenu.
    menu_controls.push_back(ENTRY_GEAR);
    menu_controls.push_back(ENTRY_INPUT_MODE);
    menu_controls.push_back(ENTRY_CONFIGINPUTS);
    menu_controls.push_back(ENTRY_REDEFKEY);
    menu_controls.push_back(ENTRY_DSTEER);
    menu_controls.push_back(ENTRY_DPEDAL);
    menu_controls.push_back(ENTRY_FEEDBACK);
    menu_controls.push_back(ENTRY_BACK);

    // Rumble subsystem (Stage 4a/4b, tools/INPUT_BINDING_DESIGN.md B.6.1).
    if (menu_feedback.size() > 0)
        menu_feedback.clear();

    menu_feedback.push_back(ENTRY_RUMBLE);
    menu_feedback.push_back(ENTRY_RUMBLE_BACKEND);
    // HID PROFILE (Stage 4b, B.6.1): shown only when the backend is explicitly HID, or AUTO has a
    // matching (suggested) profile for the current target device - otherwise there is nothing
    // meaningful to cycle.
    if (rumble::configured_backend() == rumble::HID_REPORT ||
        (rumble::configured_backend() == rumble::AUTO && rumble::hid_selected_profile_index() >= 0 &&
         rumble::hid_profile_matches_target(rumble::hid_selected_profile_index())))
        menu_feedback.push_back(ENTRY_RUMBLE_PROFILE);
    menu_feedback.push_back(ENTRY_RUMBLE_TEST);
    menu_feedback.push_back(ENTRY_RUMBLE_ADVANCED);
    // Stage 4c (tools/INPUT_BINDING_DESIGN.md B.2.2): optional richer effects, OFF by default.
    menu_feedback.push_back(ENTRY_RUMBLE_EXTRAS);
    menu_feedback.push_back(ENTRY_BACK);
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

    // Stop any wav/mp3 playback
    cannonball::audio.clear_wav();
    osoundint.init();
    osoundint.has_booted = true;
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
        case STATE_RUMBLE_EDITOR:
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
    else if (state == STATE_RUMBLE_EDITOR)
    {
        rumble_editor();
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
            if (SELECTED(ENTRY_VIDEO))       set_menu(&menu_video);
            else if (SELECTED(ENTRY_SOUND))  set_menu(&menu_sound);
            else if (SELECTED(ENTRY_ENGINE)) set_menu(&menu_engine);
            else if (SELECTED(ENTRY_SCORES)) {
                display_message(config.clear_scores() ? "SCORES CLEARED" : "NO SAVED SCORES FOUND!");
            } else if (SELECTED(ENTRY_MASTER_BREAK)) {
                if (config.master_break_key == SDLK_ESCAPE)
                    config.master_break_key = SDLK_F10;
                else
                    config.master_break_key = SDLK_ESCAPE;
            } else if (SELECTED(ENTRY_CONTROLS)) {
                display_message(input.gamepad ? "GAMEPAD FOUND" : "NO GAMEPAD FOUND!");
                populate_controls();
                set_menu(&menu_controls);
            }
        }
        // Extra Settings Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_exsettings)
        {
            if (SELECTED(ENTRY_TRACKS))
                config.engine.jap ^= 1;
            else if (SELECTED(ENTRY_GEAR))
                config.controls.gear =
                config.controls.gear == config.controls.GEAR_PRESS ? config.controls.GEAR_AUTO : config.controls.GEAR_PRESS;
            else if (SELECTED(ENTRY_ENHANCE))
                set_menu(&menu_s_enhance);
            else if (SELECTED(ENTRY_SCORES))
                display_message(config.clear_scores() ? "SCORES CLEARED" : "NO SAVED SCORES FOUND!");
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
            else if (SELECTED(ENTRY_ADVERTISE))     config.sound.advertise      ^= 1;
        }
        // Enahnce Menu (SmartyPi Only)
        else if (menu_selected == &menu_s_enhance)
        {
            if (SELECTED(ENTRY_SUB_HANDLING))       set_menu(&menu_handling);
            else if (SELECTED(ENTRY_PREVIEWSND))    config.sound.preview        ^= 1;
            else if (SELECTED(ENTRY_ATTRACT))       config.engine.new_attract   ^= 1;
            else if (SELECTED(ENTRY_OBJECTS))       config.engine.level_objects ^= 1;
            else if (SELECTED(ENTRY_PROTOTYPE))     config.engine.prototype     ^= 1;
            else if (SELECTED(ENTRY_S_BUGS))        config.engine.fix_bugs      ^= 1;
            else if (SELECTED(ENTRY_TIMER))         config.engine.fix_timer     ^= 1;
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
            if (SELECTED(ENTRY_CRT_SHADER_MODE))
            {
                if (++config.video.shader_mode > video_settings_t::SHADER_FULL)
                    config.video.shader_mode = video_settings_t::SHADER_OFF;
                switch (config.video.shader_mode) {
                    case video_settings_t::SHADER_OFF:
                        display_message("MOST EFFECTS WILL BE UNAVAILABLE");
                        break;
                    case video_settings_t::SHADER_FAST:
                        // user has enabled 'Fast' shader.
                        display_message("NOISE AND DESAT. WILL BE UNAVAILBLE");
                        break;
                    case video_settings_t::SHADER_FULL:
                        // user has enabled 'Full' shader.
                        display_message("ALL EFFECTS ENABLED");
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
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else if (config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF)
                    config.video.shadow_mask = video_settings_t::SHADOW_MASK_SHADER;
                else
                    config.video.shadow_mask = video_settings_t::SHADOW_MASK_OFF;
            }
            else if (SELECTED(ENTRY_MASK_DIM))
            {
                if (config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF)
                    display_message("ENABLE MASK FIRST");
                else if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    config.video.maskDim -= 5;
                    if (config.video.maskDim < 0)
                        config.video.maskDim = 100;
                }
            }
            else if (SELECTED(ENTRY_MASK_BOOST))
            {
                if (config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF)
                    display_message("ENABLE MASK FIRST");
                else if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    config.video.maskBoost += 5;
                    if (config.video.maskBoost > 160)
                        config.video.maskBoost = 100;
                }
            }
            else if (SELECTED(ENTRY_MASK_SIZE))
            {
                if (config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF)
                    display_message("ENABLE MASK FIRST");
                else if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    config.video.mask_size += 1;
                    if (config.video.mask_size > 6)
                        config.video.mask_size = 3;
                }
            }
            else if (SELECTED(ENTRY_SCANLINES))
            {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    if (++config.video.scanlines > 3)
                        config.video.scanlines = 0;
                }
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader shadown mask settings
        else if (menu_selected == &menu_crt_shape_settings) {
            if (SELECTED(ENTRY_CRT_SHAPE))
            {
                // Curved bezel effect is applied in overlay along with the vignette, which is supported in all three shaders
                // ('none' is actually two textures stacked) - hence no restart required.
                config.video.crt_shape ^= 1;
            }
            else if (SELECTED(ENTRY_VIGNETTE))
            {
                // as above - no restart required.
                config.video.vignette += 5;
                if (config.video.vignette > 75)
                    config.video.vignette = 0;
            }
            else if (SELECTED(ENTRY_WARPX))
            {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    if (++config.video.warpX > 10)
                        config.video.warpX = 0;
                }
            }
            else if (SELECTED(ENTRY_WARPY))
            {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    if (++config.video.warpY > 10)
                        config.video.warpY = 0;
                }
            }
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }

        // JJP - CRT shader settings (noise/desaturation/brightness boost)
        else if (menu_selected == &menu_crt_shader2) {
            if (SELECTED(ENTRY_NOISE))
            {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    display_message("ENABLE FULL SHADER FIRST");
                else {
                    if (++config.video.noise > 20)
                        config.video.noise = 0;
                }
            }
            else if (SELECTED(ENTRY_DESATURATE))
            {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    display_message("ENABLE FULL SHADER FIRST");
                else {
                    if (++config.video.desaturate > 10)
                        config.video.desaturate = 0;
                }
            }
            else if (SELECTED(ENTRY_DESATURATE_EDGES))
            {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    display_message("ENABLE FULL SHADER FIRST");
                else {
                    if (++config.video.desaturate_edges > 10)
                        config.video.desaturate_edges = 0;
                }
            }
            else if (SELECTED(ENTRY_BRIGHTNESS_BOOST))
            {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    display_message("ENABLE SHADER FIRST");
                else {
                    if (++config.video.brightboost > 10)
                        config.video.brightboost = 0;
                }
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
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.saturation += 10;
                    if (config.video.saturation > 50)
                        config.video.saturation = -50;
                }
            }
            else if (SELECTED(ENTRY_CONTRAST))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.contrast += 10;
                    if (config.video.contrast > 50)
                        config.video.contrast = -50;
                }
            }
            else if (SELECTED(ENTRY_BRIGHTNESS))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.brightness += 10;
                    if (config.video.brightness > 50)
                        config.video.brightness = -50;
                }
            }
            else if (SELECTED(ENTRY_SHARPNESS))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.sharpness += 10;
                    if (config.video.sharpness > 50)
                        config.video.sharpness = -50;
                }
            }
            else if (SELECTED(ENTRY_RESOLUTION))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.resolution += 10;
                    if (config.video.resolution > 0)
                        config.video.resolution = -100;
                }
            }
            else if (SELECTED(ENTRY_GAMMA))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.gamma += 1;
                    if (config.video.gamma > 10)
                        config.video.gamma = -20;
                }
            }
            else if (SELECTED(ENTRY_HUE))
            {
                if (config.video.blargg == video_settings_t::BLARGG_DISABLE)
                    display_message("ENABLE BLARGG FILTER FIRST");
                else {
                    config.video.hue += 1;
                    if (config.video.hue > 10)
                        config.video.hue = -10;
                }
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
            else if (SELECTED(ENTRY_OUTPUT_DEVICE))
            {
                // Candidate list is {DEFAULT} + the enumerated SDL devices. The
                // list is normally already populated by start_audio(); refresh
                // once here if it isn't (sound may never have been enabled, so
                // start_audio() never ran).
                if (cannonball::audio.playback_devices().empty())
                    cannonball::audio.refresh_playback_devices();
                const auto& devices = cannonball::audio.playback_devices();

                // Find where the configured (or, failing that, actually open)
                // device sits in the candidate list, so we advance from there.
                int current = 0; // 0 = DEFAULT
                const std::string& want = !config.sound.playback_device_name.empty()
                                         ? config.sound.playback_device_name
                                         : cannonball::audio.active_device_name();
                if (!want.empty()) {
                    for (size_t i = 0; i < devices.size(); i++)
                        if (devices[i] == want) { current = int(i) + 1; break; }
                }

                int next = current + 1;
                if (next > int(devices.size())) next = 0; // wrap to DEFAULT

                if (next == 0) {
                    config.sound.playback_device      = -1;
                    config.sound.playback_device_name = "";
                } else {
                    config.sound.playback_device      = next - 1;
                    config.sound.playback_device_name = devices[next - 1];
                }

                // Apply live, mirroring ENTRY_MUTE above - config.save() (SAVE
                // AND RETURN) persists the choice, this just makes it audible now.
                if (config.sound.enabled) {
                    cannonball::audio.stop_audio();
                    cannonball::audio.start_audio();
                    if (!cannonball::audio.device_open())
                        display_message("AUDIO DEVICE FAILED TO OPEN");
                }
            }
            else if (SELECTED(ENTRY_CALLBACK_RATE))
            {
                // SDL audio callback interval: 0=8ms, 1=16ms, 2=AUTO (resolved from the
                // detected CPU tier - see cputier.hpp / Audio::resolve_callback_rate()).
                config.sound.callback_rate = (config.sound.callback_rate + 1) % 3;
                // Apply live, mirroring OUTPUT DEVICE above - config.save() (SAVE AND
                // RETURN) persists the choice, this just makes it audible now.
                if (config.sound.enabled) {
                    cannonball::audio.stop_audio();
                    cannonball::audio.start_audio();
                    if (!cannonball::audio.device_open())
                        display_message("AUDIO DEVICE FAILED TO OPEN");
                }
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
            else if (SELECTED(ENTRY_INPUT_MODE))
            {
                // Input binding matrix (Stage 2, tools/INPUT_BINDING_DESIGN.md A.3.2/A.4.1): cycle the
                // active GAMEPAD/WHEEL group, drop any stuck key from the group that just went
                // inactive, and rebuild the per-device resolved cache (also re-derives analog).
                config.controls.input_mode ^= 1;
                input.reset_keys();
                input.rebuild_binding_cache();
                // Rumble subsystem (Stage 4a, tools/INPUT_BINDING_DESIGN.md B.7.5): the target device
                // depends on the active input_mode group, so re-resolve it now (this always
                // stop_all()s first, forcing OFF before anything might change underneath it).
                rumble::on_devices_changed();
            }
            else if (SELECTED(ENTRY_CONFIGINPUTS))
            {
                // Input binding matrix (Stage 3, tools/INPUT_BINDING_DESIGN.md A.4.1/A.4.2): open the
                // matrix editor. Snapshot the editable state first so CANCEL can restore it (this
                // also covers undoing the first-open legacy migration below), then on first open of
                // a legacy config, migrate the old arrays so nothing is lost (A.2.2), then reset the
                // editor cursor/phase.
                bind_snap_bindings = config.controls.device_bindings;
                memcpy(bind_snap_keyconfig, config.controls.keyconfig, sizeof(bind_snap_keyconfig));
                bind_snap_active = config.controls.bindings_active;
                if (!config.controls.bindings_active)
                    migrate_legacy_to_bindings();
                bind_row       = 0;
                bind_col       = 0;
                bind_phase     = 0; // BROWSE
                bind_wait      = Input::Captured();
                bind_wait_key  = -1;
                bind_msg.clear();
                bind_msg_timer = 0;
                input.key_press = -1; // don't let the RETURN that opened this entry self-activate a cell
                state = STATE_REDEFINE_JOY;
            }
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
            else if (SELECTED(ENTRY_FEEDBACK))
                set_menu(&menu_feedback);
            else if (SELECTED(ENTRY_BACK))
                menu_back();
        }
        else if (menu_selected == &menu_feedback)
        {
            // Rumble subsystem (Stage 4a/4b, tools/INPUT_BINDING_DESIGN.md B.6.1).
            if (SELECTED(ENTRY_RUMBLE))
            {
                config.controls.rumble += 0.25f;
                if (config.controls.rumble > 1.0f) config.controls.rumble = 0;
                rumble::set_master(config.controls.rumble);
            }
            else if (SELECTED(ENTRY_RUMBLE_BACKEND))
            {
                int next = (static_cast<int>(rumble::configured_backend()) + 1) % 6; // AUTO..OFF wrap
                rumble::set_backend(static_cast<rumble::Backend>(next));
            }
            else if (SELECTED(ENTRY_RUMBLE_PROFILE))
            {
                // Cycle through profiles matching the current target device + CUSTOM (B.3.3);
                // wraps past any that don't match so this never silently arms an unrelated profile.
                int count = rumble::hid_profile_count();
                int cur = rumble::hid_selected_profile_index();
                for (int tries = 0; tries < count; tries++)
                {
                    cur = (cur + 1) % count;
                    if (rumble::hid_profile_matches_target(cur) || rumble::hid_profile_name(cur) == std::string("CUSTOM"))
                    {
                        rumble::set_hid_profile_index(cur);
                        break;
                    }
                }
            }
            else if (SELECTED(ENTRY_RUMBLE_TEST))
            {
                bool started = rumble::test(rumble::TEST, 400);
                display_message(started ? (std::string("TESTING - ") + rumble::active_backend_name())
                                         : "NO RUMBLE DEVICE");
            }
            else if (SELECTED(ENTRY_RUMBLE_EXTRAS))
            {
                // Stage 4c (tools/INPUT_BINDING_DESIGN.md B.2.2): toggle the optional CRASH/GEAR/START_REV
                // effects polled inside rumble::tick(). rumble.cpp clears any stuck continuous
                // START_REV level itself on the on->off transition.
                config.controls.rumble_extras = config.controls.rumble_extras ? 0 : 1;
            }
            else if (SELECTED(ENTRY_RUMBLE_ADVANCED))
            {
                rumble::stop_all(); // B.6.2: only TEST writes while the editor is open
                int suggested = rumble::hid_selected_profile_index();
                if (suggested < 0)
                {
                    // Nothing configured/matching yet - start from the first built-in that matches
                    // the target device, else plain "thrustmaster3" as a harmless default shape.
                    suggested = 0;
                    for (int i = 0; i < rumble::hid_profile_count(); i++)
                        if (rumble::hid_profile_matches_target(i)) { suggested = i; break; }
                }
                hid_edit = rumble::hid_profile_snapshot(suggested);
                rumble::HidTargetInfo t = rumble::hid_target_info();
                if (t.present) { hid_edit.vid = t.vid; hid_edit.pid = t.pid; } // arm for TEST (B.3.2)
                hid_edit_row = 0;
                hid_edit_field = 0;
                state = STATE_RUMBLE_EDITOR;
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
            if (SELECTED(ENTRY_HIRES)) {
                // historically in video but fits better here
                // we flag to the main program loop that a change has been requested, which
                // will be done (via video subsystem restart) once all workers for this frame are complete.
                config.video.hires_next = config.video.hires ^ 1;
                if (config.video.hires_next == 0)
                    config.video.hiresprites = 0; // disable hires sprites also
                //start_game(Outrun::MODE_ORIGINAL);
                config.videoRestartRequired = true;
            }
            else if (SELECTED(ENTRY_SPRITERES)) {
                if (config.video.hires == 0) {
                    display_message("Set game engine to hires first");
                } else {
                    config.video.hiresprites ^= 1;
                    if (config.video.hiresprites == 1)
                        display_message("Enabled. Toggle in game with F7.");
                    else
                        display_message("Disabled. Toggle in game with F7.");
                }
            }
            else if (SELECTED(ENTRY_ATTRACT))           config.engine.new_attract ^= 1;
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

        if (menu_selected == &menu_settings)
        {
            // JJP - added master break key here
            if (SELECTED(ENTRY_MASTER_BREAK))
                set_menu_text(ENTRY_MASTER_BREAK, config.master_break_key == SDLK_ESCAPE ? "ESC" : "F10");
        }
        else if (menu_selected == &menu_about)  // JJP - include machine stats in about menu
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
            else if (SELECTED(ENTRY_OUTPUT_DEVICE))
            {
                // Shows the CONFIGURED device, not necessarily the one actually
                // open - if it couldn't be honoured at startup, the console
                // warning from start_audio() covers that.
                std::string text = config.sound.playback_device_name.empty()
                                  ? "DEFAULT" : short_device_label(font_safe(config.sound.playback_device_name));
                size_t max_len = strlen(ENTRY_OUTPUT_DEVICE) < 40 ? 40 - strlen(ENTRY_OUTPUT_DEVICE) : 0;
                if (text.size() > max_len) text = text.substr(0, max_len);
                set_menu_text(ENTRY_OUTPUT_DEVICE, text);
            }
            else if (SELECTED(ENTRY_CALLBACK_RATE))
            {
                // Shows the CONFIGURED interval; AUTO also appends the resolved value
                // (e.g. "AUTO - 16MS") so it's clear what's actually in effect, without
                // needing sound enabled/an Audio instance to have started audio yet.
                std::string text = CALLBACK_RATE_LABELS[config.sound.callback_rate];
                if (config.sound.callback_rate == sound_settings_t::CALLBACK_AUTO) {
                    int resolved = Audio::resolve_callback_rate(config.sound.callback_rate);
                    text += " - ";
                    text += (resolved == sound_settings_t::CALLBACK_16MS ? "16MS" : "8MS");
                }
                set_menu_text(ENTRY_CALLBACK_RATE, text);
            }
            else if (SELECTED(ENTRY_ADVERTISE))     set_menu_text(ENTRY_ADVERTISE, config.sound.advertise ? "ON" : "OFF");
            else if (SELECTED(ENTRY_PREVIEWSND))    set_menu_text(ENTRY_PREVIEWSND, config.sound.preview ? "ON" : "OFF");
            else if (SELECTED(ENTRY_FIXSAMPLES))    set_menu_text(ENTRY_FIXSAMPLES, config.sound.fix_samples ? "ON" : "OFF");
        }
        // JJP CRT Emulation related menus
        // Note - no entry here for menu_crt_shader1 as it only lines to other menus
        else if (menu_selected == &menu_crt_shader1)
        {
            // Screen feedback for selected options
            if (SELECTED(ENTRY_CRT_SHADER_MODE))
            {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    set_menu_text(ENTRY_CRT_SHADER_MODE, "OFF");
                else if (config.video.shader_mode == video_settings_t::SHADER_FAST)
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
            else if (SELECTED(ENTRY_WARPX)) {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    set_menu_text(ENTRY_WARPX, "OFF");
                else
                    set_menu_text(ENTRY_WARPX, Utils::to_string(config.video.warpX) + "%");
            }
            else if (SELECTED(ENTRY_WARPY)) {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    set_menu_text(ENTRY_WARPY, "OFF");
                else
                    set_menu_text(ENTRY_WARPY, Utils::to_string(config.video.warpY) + "%");
            }
        }
        else if (menu_selected == &menu_crt_mask_settings)
        {
            if (SELECTED(ENTRY_SHADOW_MASK))
            {
                if (config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF)
                    set_menu_text(ENTRY_SHADOW_MASK, "OFF");
                else
                    set_menu_text(ENTRY_SHADOW_MASK, "ON");
            }
            else if (SELECTED(ENTRY_MASK_DIM)) {
                if ((config.video.shadow_mask == video_settings_t::SHADOW_MASK_OFF) ||
                    (config.video.shader_mode == video_settings_t::SHADER_OFF))
                    set_menu_text(ENTRY_MASK_DIM, "OFF");
                else
                    set_menu_text(ENTRY_MASK_DIM, Utils::to_string(config.video.maskDim) + "%");
            }
            else if (SELECTED(ENTRY_MASK_BOOST)) {
                if ((config.video.shadow_mask != video_settings_t::SHADOW_MASK_SHADER) ||
                    (config.video.shader_mode == video_settings_t::SHADER_OFF))
                    set_menu_text(ENTRY_MASK_BOOST, "OFF");
                else
                    set_menu_text(ENTRY_MASK_BOOST, Utils::to_string(config.video.maskBoost) + "%");
            }
            else if (SELECTED(ENTRY_MASK_SIZE)) {
                if ((config.video.shadow_mask != video_settings_t::SHADOW_MASK_SHADER) ||
                    (config.video.shader_mode == video_settings_t::SHADER_OFF))
                    set_menu_text(ENTRY_MASK_SIZE, "OFF");
                else
                    set_menu_text(ENTRY_MASK_SIZE, Utils::to_string(config.video.mask_size) + "px");
            }
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
            if (SELECTED(ENTRY_NOISE)) {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    set_menu_text(ENTRY_NOISE, "OFF");
                else
                    set_menu_text(ENTRY_NOISE, Utils::to_string(config.video.noise));
            }
            else if (SELECTED(ENTRY_DESATURATE)) {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    set_menu_text(ENTRY_DESATURATE, "OFF");
                else
                    set_menu_text(ENTRY_DESATURATE,
                        config.video.desaturate ? Utils::to_string(config.video.desaturate) + "%" : "OFF");
            }
            else if (SELECTED(ENTRY_DESATURATE_EDGES)) {
                if (config.video.shader_mode != video_settings_t::SHADER_FULL)
                    set_menu_text(ENTRY_DESATURATE_EDGES, "OFF");
                else
                    set_menu_text(ENTRY_DESATURATE_EDGES,
                        config.video.desaturate_edges ? Utils::to_string(config.video.desaturate_edges) + "%" : "OFF");
            }
            else if (SELECTED(ENTRY_BRIGHTNESS_BOOST)) {
                if (config.video.shader_mode == video_settings_t::SHADER_OFF)
                    set_menu_text(ENTRY_BRIGHTNESS_BOOST, "OFF");
                else
                    set_menu_text(ENTRY_BRIGHTNESS_BOOST,
                        config.video.brightboost ? Utils::to_string(config.video.brightboost) + "%" : "OFF");
            }
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
            else if (SELECTED(ENTRY_INPUT_MODE))    set_menu_text(ENTRY_INPUT_MODE, INPUT_MODE_LABELS[config.controls.input_mode]);
            else if (SELECTED(ENTRY_DSTEER))        set_menu_text(ENTRY_DSTEER, Utils::to_string(config.controls.steer_speed));
            else if (SELECTED(ENTRY_DPEDAL))        set_menu_text(ENTRY_DPEDAL, Utils::to_string(config.controls.pedal_speed));
        }
        else if (menu_selected == &menu_feedback)
        {
            if (SELECTED(ENTRY_RUMBLE))
                set_menu_text(ENTRY_RUMBLE, RUMBLE_LABELS[std::min(4, (int)(config.controls.rumble / 0.25f))]);
            else if (SELECTED(ENTRY_RUMBLE_BACKEND))
            {
                int idx = config.controls.rumble_backend;
                if (idx < 0 || idx > 5) idx = 0;
                std::string label = RUMBLE_BACKEND_LABELS[idx];
                if (idx == 0) // AUTO: also show what it actually resolved to (40-col budget - only
                {             // shown for AUTO; an explicit choice already names itself)
                    label += " - ";
                    label += rumble::active_backend_name();
                }
                set_menu_text(ENTRY_RUMBLE_BACKEND, label);
            }
            else if (SELECTED(ENTRY_RUMBLE_PROFILE))
            {
                // B.3.3: name the selected profile, and flag it as auto-suggested when the user
                // hasn't explicitly forced HID_REPORT (i.e. we're under AUTO and it merely happens
                // to be the best match for the current target device).
                int idx = rumble::hid_selected_profile_index();
                std::string label = (idx >= 0) ? rumble::hid_profile_name(idx) : "NONE";
                if (idx >= 0 && rumble::configured_backend() == rumble::AUTO)
                    label += " - SUGGESTED";
                set_menu_text(ENTRY_RUMBLE_PROFILE, label);
            }
            else if (SELECTED(ENTRY_RUMBLE_EXTRAS))
                set_menu_text(ENTRY_RUMBLE_EXTRAS, config.controls.rumble_extras ? "ON" : "OFF");
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
            if (SELECTED(ENTRY_TRACKS))             set_menu_text(ENTRY_TRACKS, config.engine.jap ? "JAPAN" : "WORLD");
            else if (SELECTED(ENTRY_GEAR))          set_menu_text(ENTRY_GEAR, GEAR_LABELS[config.controls.gear]);
            else if (SELECTED(ENTRY_MUTE))          set_menu_text(ENTRY_MUTE, config.sound.enabled ? "ON" : "OFF");
        }
        else if (menu_selected == &menu_enhancements || menu_selected == &menu_s_enhance)
        {
            if (SELECTED(ENTRY_HIRES))              set_menu_text(ENTRY_HIRES, config.video.hires ? "HI-RES" : "ORIGINAL");
            else if (SELECTED(ENTRY_SPRITERES))     set_menu_text(ENTRY_SPRITERES, config.video.hiresprites ? "HI-RES" : "ORIGINAL");
            else if (SELECTED(ENTRY_PREVIEWSND))    set_menu_text(ENTRY_PREVIEWSND, config.sound.preview ? "ON" : "OFF");
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

// ------------------------------------------------------------------------------------------------
// Stage 3 (tools/INPUT_BINDING_DESIGN.md A.4.2): binding-matrix editor. Reached via STATE_REDEFINE_JOY and
// dispatched from tick_ui(). 13 data rows + SAVE + CANCEL; three columns (KEYBOARD / GAMEPAD / WHEEL). All
// editor state is held in the bind_* Menu members. SAVE keeps the edits live (the settings menu SAVE entry persists them);
// CANCEL restores the snapshot taken on entry and discards them. Leaving the editor (either way, or
// via MENU) rebuilds the binding cache so edits are live immediately. ESC still quits the program
// and is not repurposed here.
// ------------------------------------------------------------------------------------------------
void Menu::redefine_joystick()
{
    enum { PH_BROWSE, PH_WAIT_TO_CAPTURE, PH_CAPTURE, PH_WAIT_TO_BROWSE };

    struct RowDef { const char* label; int kslot; int target; };
    static const RowDef ROWS[] = {
        { "STEERING",    -1, device_binding_t::TARGET_STEER },
        { "ACCELERATE",   4, device_binding_t::TARGET_ACCEL },
        { "BRAKE",        5, device_binding_t::TARGET_BRAKE },
        { "GEAR LOW",     6, device_binding_t::TARGET_GEAR1 },
        { "GEAR HIGH",    7, device_binding_t::TARGET_GEAR2 },
        { "START",        8, device_binding_t::TARGET_START },
        { "COIN",         9, device_binding_t::TARGET_COIN  },
        { "MENU",        10, device_binding_t::TARGET_MENU  },
        { "VIEW CHANGE", 11, device_binding_t::TARGET_VIEW  },
        { "UP",           0, device_binding_t::TARGET_UP    },
        { "DOWN",         1, device_binding_t::TARGET_DOWN  },
        { "LEFT",         2, device_binding_t::TARGET_LEFT  },
        { "RIGHT",        3, device_binding_t::TARGET_RIGHT },
    };
    const int DATA_ROWS  = 13;
    const int SAVE_ROW   = DATA_ROWS;      // 13
    const int CANCEL_ROW = DATA_ROWS + 1;  // 14

    // Cursor normalisation - also snaps the spec's bind_row=0/bind_col=0 init off STEERING's
    // non-selectable keyboard cell, and forces col 0 on the SAVE/CANCEL rows.
    if (bind_row < 0) bind_row = 0;
    if (bind_row > CANCEL_ROW) bind_row = CANCEL_ROW;
    if (bind_col < 0) bind_col = 0;
    if (bind_col > 2) bind_col = 2;
    if (bind_row >= DATA_ROWS) bind_col = 0;
    else if (bind_row == 0 && bind_col == 0) bind_col = 1; // STEERING has no keyboard cell

    if (bind_msg_timer > 0 && --bind_msg_timer == 0) bind_msg.clear();

    auto group_of = [](int col) {
        return col == 1 ? device_binding_t::GROUP_GAMEPAD : device_binding_t::GROUP_WHEEL;
    };

    // ---- Input by phase --------------------------------------------------------------------
    switch (bind_phase)
    {
        case PH_BROWSE:
        {
            if (input.has_pressed(Input::MENU)) { leave_editor(); return; }

            if (input.key_press == SDLK_DELETE || input.key_press == SDLK_BACKSPACE)
            {
                input.key_press = -1;
                if (bind_row < DATA_ROWS)
                {
                    if (bind_col == 0)
                    {
                        if (ROWS[bind_row].kslot >= 0) config.controls.keyconfig[ROWS[bind_row].kslot] = -1;
                    }
                    else
                        config.clear_binding(group_of(bind_col), ROWS[bind_row].target);
                    input.rebuild_binding_cache();
                }
                break;
            }

            if (input.has_pressed(Input::UP))
            {
                bind_row = (bind_row == 0) ? CANCEL_ROW : bind_row - 1;
                if (bind_row >= DATA_ROWS) bind_col = 0;
                else if (bind_row == 0 && bind_col == 0) bind_col = 1;
                break;
            }
            if (input.has_pressed(Input::DOWN))
            {
                bind_row = (bind_row == CANCEL_ROW) ? 0 : bind_row + 1;
                if (bind_row >= DATA_ROWS) bind_col = 0;
                else if (bind_row == 0 && bind_col == 0) bind_col = 1;
                break;
            }
            if (bind_row < DATA_ROWS)
            {
                int col_min = (bind_row == 0) ? 1 : 0; // STEERING skips the keyboard column
                if (input.has_pressed(Input::LEFT))  { bind_col = (bind_col <= col_min) ? 2 : bind_col - 1; break; }
                if (input.has_pressed(Input::RIGHT)) { bind_col = (bind_col >= 2) ? col_min : bind_col + 1; break; }
            }

            if (select_pressed() || input.key_press == SDLK_RETURN)
            {
                input.key_press = -1;
                if (bind_row == SAVE_ROW)
                {
                    // Keep the edits (already live in config.controls and the binding cache);
                    // persisting them is the settings menu SAVE entry, so remind the user. The
                    // message is shown after leave_editor() switches back to STATE_MENU, as
                    // display_message is used elsewhere in this menu.
                    display_message("SAVE SETTINGS TO USE UPDATED CONFIG");
                    leave_editor();
                    return;
                }
                if (bind_row == CANCEL_ROW)
                {
                    // Restore the snapshot taken on entry, undoing any edits made in this session
                    // (including a first-open legacy migration).
                    config.controls.device_bindings = bind_snap_bindings;
                    memcpy(config.controls.keyconfig, bind_snap_keyconfig, sizeof(bind_snap_keyconfig));
                    config.controls.bindings_active = bind_snap_active;
                    input.rebuild_binding_cache();
                    display_message("CHANGES DISCARDED");
                    leave_editor();
                    return;
                }
                // Wait for the activating control to be released before capturing, so the button
                // that opened the cell cannot be captured as its binding.
                bind_phase = PH_WAIT_TO_CAPTURE;
            }
            break;
        }

        case PH_WAIT_TO_CAPTURE:
        {
            const Uint8* kb = SDL_GetKeyboardState(NULL);
            bool released = !input.is_pressed(Input::START) && !input.is_pressed(Input::ACCEL) &&
                            !input.is_pressed(Input::GEAR1) && kb[SDL_SCANCODE_RETURN] == 0;
            if (released)
            {
                if (bind_col == 0) input.key_press = -1;              // keyboard column
                else               input.begin_capture(group_of(bind_col));
                bind_phase = PH_CAPTURE;
            }
            break;
        }

        case PH_CAPTURE:
        {
            const RowDef& row = ROWS[bind_row];
            if (bind_col == 0)
            {
                if (input.has_pressed(Input::MENU)) { input.key_press = -1; bind_phase = PH_BROWSE; }
                else if (input.key_press != -1)
                {
                    if (row.kslot >= 0)
                    {
                        config.controls.keyconfig[row.kslot] = input.key_press;
                        input.rebuild_binding_cache();
                        bind_wait_key  = SDL_GetScancodeFromKey(input.key_press);
                        bind_wait      = Input::Captured();
                        bind_msg       = "BOUND KEY";
                        bind_msg_timer = 60;
                    }
                    input.key_press = -1;
                    bind_phase = PH_WAIT_TO_BROWSE;
                }
            }
            else
            {
                if (input.has_pressed(Input::MENU)) { input.end_capture(); bind_phase = PH_BROWSE; break; }

                Input::Captured c = input.poll_capture();
                if (c.kind != Input::Captured::NONE)
                {
                    bool accept =
                        (row.target == device_binding_t::TARGET_STEER) ? (c.kind == Input::Captured::AXIS) :
                        (row.target == device_binding_t::TARGET_ACCEL ||
                         row.target == device_binding_t::TARGET_BRAKE) ? true :
                        (c.kind == Input::Captured::BUTTON || c.kind == Input::Captured::HAT);

                    if (accept)
                    {
                        device_binding_t b;
                        b.group  = group_of(bind_col);
                        b.target = row.target;
                        b.type   = (c.kind == Input::Captured::AXIS) ? device_binding_t::TYPE_AXIS :
                                   (c.kind == Input::Captured::HAT)  ? device_binding_t::TYPE_HAT  :
                                                                       device_binding_t::TYPE_BUTTON;
                        b.index  = c.index;
                        b.value  = (c.kind == Input::Captured::HAT) ? c.hat_value : 0;

                        // Derive the per-binding invert flag from the captured direction (c.delta) so
                        // pedals/wheels that report backwards work without hand-editing config.xml:
                        // ACCEL/BRAKE - pressing must increase the value; STEER - the prompt asks for
                        // a LEFT turn, which is negative in SDL's convention (the same convention
                        // apply_axis maps to 0x40 = hard left). Every other target is digital-only.
                        if (c.kind == Input::Captured::AXIS)
                        {
                            if (row.target == device_binding_t::TARGET_ACCEL ||
                                row.target == device_binding_t::TARGET_BRAKE)
                                b.invert = c.delta < 0;
                            else if (row.target == device_binding_t::TARGET_STEER)
                                b.invert = c.delta > 0;
                            else
                                b.invert = false;
                        }
                        else
                            b.invert = false;

                        const InputDevice* d = input.find_device(c.device);
                        if (d) { b.device = d->signature; b.vid = d->vid; b.pid = d->pid; b.name = d->name; }
                        config.set_binding(b);
                        input.end_capture();
                        input.rebuild_binding_cache();
                        bind_msg = std::string("BOUND ") + (d ? d->name : std::string());
                        if (bind_msg.size() > 34) bind_msg.resize(34);
                        bind_msg_timer = 60;
                        bind_wait     = c;
                        bind_wait_key = -1;
                        bind_phase    = PH_WAIT_TO_BROWSE;
                    }
                    // A non-accepted kind is ignored; keep waiting for a control the row accepts.
                }
            }
            break;
        }

        case PH_WAIT_TO_BROWSE:
        {
            bool released;
            if (bind_wait_key >= 0)
            {
                const Uint8* kb = SDL_GetKeyboardState(NULL);
                released = kb[bind_wait_key] == 0;
            }
            else
                released = input.control_released(bind_wait);
            if (released) bind_phase = PH_BROWSE;
            break;
        }
    }

    // ---- Drawing (40x28 text grid, A.4.2) --------------------------------------------------
    auto compact = [](const char* s, size_t n) -> std::string {
        std::string out;
        if (s)
            for (const char* p = s; *p && out.size() < n; ++p)
                if (*p != ' ') out += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        out = font_safe(out);
        return out.empty() ? std::string("-") : out;
    };
    auto hat_dir = [](int v) -> const char* {
        switch (v)
        {
            case SDL_HAT_UP:        return "U";
            case SDL_HAT_DOWN:      return "D";
            case SDL_HAT_LEFT:      return "L";
            case SDL_HAT_RIGHT:     return "R";
            case SDL_HAT_RIGHTUP:   return "RU";
            case SDL_HAT_RIGHTDOWN: return "RD";
            case SDL_HAT_LEFTUP:    return "LU";
            case SDL_HAT_LEFTDOWN:  return "LD";
            default:                return "";
        }
    };
    auto device_present = [&](const device_binding_t* b) -> bool {
        if (!b) return false;
        for (const auto& d : input.get_devices())
        {
            if (!b->device.empty() && b->device == d.signature) return true;
            if (b->vid != 0 && b->vid == d.vid && b->pid == d.pid) return true;
        }
        return false;
    };

    ohud.blit_text_new(12, 2, "CONFIGURE INPUTS", ohud.GREEN);

    ohud.blit_text_new(1,  4, "CONTROL", ohud.GREEN);
    ohud.blit_text_new(14, 4, "KEYBOARD", (bind_row < DATA_ROWS && bind_col == 0) ? ohud.PINK : ohud.GREEN);
    ohud.blit_text_new(24, 4, "GAMEPAD",  (bind_row < DATA_ROWS && bind_col == 1) ? ohud.PINK : ohud.GREEN);
    ohud.blit_text_new(33, 4, "WHEEL",    (bind_row < DATA_ROWS && bind_col == 2) ? ohud.PINK : ohud.GREEN);

    for (int r = 0; r < DATA_ROWS; r++)
    {
        const RowDef& row = ROWS[r];
        const int y = 6 + r;
        ohud.blit_text_new(1, y, row.label, (r == bind_row) ? ohud.PINK : ohud.GREEN);

        // Keyboard column.
        {
            std::string txt;
            if (row.kslot < 0) txt = "-";
            else
            {
                int kc = config.controls.keyconfig[row.kslot];
                txt = (kc < 0) ? std::string("-") : compact(SDL_GetKeyName(kc), 8);
            }
            ohud.blit_text_new(14, y, txt.c_str(), (r == bind_row && bind_col == 0) ? ohud.PINK : ohud.GREEN);
        }

        // GAMEPAD (col 1) and WHEEL (col 2) columns.
        for (int c = 1; c <= 2; c++)
        {
            const int gx  = (c == 1) ? 24 : 33;
            const int grp = group_of(c);
            const device_binding_t* b = config.find_binding(grp, row.target);
            std::string txt;
            if (!b) txt = "-";
            else if (c == 1)
            {
                if (b->type == device_binding_t::TYPE_AXIS)
                    txt = compact(SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(b->index)), 8);
                else if (b->type == device_binding_t::TYPE_BUTTON)
                    txt = compact(SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b->index)), 8);
                else
                    txt = "H" + std::to_string(b->index) + hat_dir(b->value);
            }
            else
            {
                if (b->type == device_binding_t::TYPE_AXIS)        txt = "AX" + std::to_string(b->index);
                else if (b->type == device_binding_t::TYPE_BUTTON) txt = "B"  + std::to_string(b->index);
                else                                               txt = "H"  + std::to_string(b->index) + hat_dir(b->value);
            }

            uint16_t col;
            if (r == bind_row && bind_col == c)      col = ohud.PINK;
            else if (b && !device_present(b))        col = ohud.GREY; // bound, but the device is absent
            else                                     col = ohud.GREEN;
            ohud.blit_text_new(gx, y, txt.c_str(), col);
        }
    }

    ohud.blit_text_new(18, 20, "SAVE",   (bind_row == SAVE_ROW)   ? ohud.PINK : ohud.GREEN);
    ohud.blit_text_new(17, 21, "CANCEL", (bind_row == CANCEL_ROW) ? ohud.PINK : ohud.GREEN);

    ohud.blit_text_new(1, 22, "ARROWS - SELECT   ENTER - CHANGE", ohud.GREY);
    ohud.blit_text_new(1, 23, "DEL/BSP - CLEAR   MENU  - EXIT", ohud.GREY);

    // Dynamic prompt / result line.
    std::string dyn;
    if (bind_msg_timer > 0 && !bind_msg.empty())
        dyn = bind_msg;
    else if (bind_phase == PH_WAIT_TO_CAPTURE || bind_phase == PH_WAIT_TO_BROWSE)
        dyn = "RELEASE CONTROL";
    else if (bind_phase == PH_CAPTURE)
    {
        if (bind_col == 0)                                           dyn = "PRESS A KEY";
        else if (ROWS[bind_row].target == device_binding_t::TARGET_STEER) dyn = "TURN WHEEL LEFT";
        else if (ROWS[bind_row].target == device_binding_t::TARGET_ACCEL ||
                 ROWS[bind_row].target == device_binding_t::TARGET_BRAKE) dyn = "MOVE AXIS OR PRESS BUTTON";
        else                                                        dyn = "PRESS BUTTON OR HAT";
    }
    if (!dyn.empty())
    {
        dyn = font_safe(dyn);
        ohud.blit_text_new(1, 26, dyn.c_str(), ohud.PINK);
    }
}

// ------------------------------------------------------------------------------------------------
// Stage 3 (A.4.2): leave the matrix editor. Ends any active capture, rebuilds the resolved binding
// cache so edits take effect immediately, and returns to the controls menu.
// ------------------------------------------------------------------------------------------------
void Menu::leave_editor()
{
    input.end_capture();
    input.rebuild_binding_cache();
    state = STATE_MENU;
    refresh_menu();
}

// ------------------------------------------------------------------------------------------------
// Stage 3 first-open migration (tools/INPUT_BINDING_DESIGN.md A.2.2): when the editor is opened on a legacy
// config (bindings_active == false), pre-populate cells from the legacy arrays for the pad_id
// device so nothing is lost, then switch bindings on. The legacy arrays are left untouched.
// ------------------------------------------------------------------------------------------------
void Menu::migrate_legacy_to_bindings()
{
    if (config.controls.bindings_active) return;

    const InputDevice* legacy = nullptr;
    for (const auto& d : input.get_devices())
        if (d.legacy_device) { legacy = &d; break; }

    if (legacy != nullptr)
    {
        // A recognised GameController migrates into the GAMEPAD group (padconfig/axis hold GC enums
        // after bind_* defaulting); any other device into the WHEEL group (raw indices).
        const int group = (legacy->controller != nullptr) ? device_binding_t::GROUP_GAMEPAD
                                                           : device_binding_t::GROUP_WHEEL;

        auto add = [&](int target, int type, int index, bool invert = false)
        {
            if (index < 0) return;
            device_binding_t b;
            b.group  = group;
            b.target = target;
            b.type   = type;
            b.index  = index;
            b.value  = 0;
            b.device = legacy->signature;
            b.vid    = legacy->vid;
            b.pid    = legacy->pid;
            b.name   = legacy->name;
            b.invert = invert;
            config.set_binding(b);
        };

        // Digital button cells - padconfig[0..11], same target order as config.cpp.
        static const int pad_targets[12] = {
            device_binding_t::TARGET_ACCEL, device_binding_t::TARGET_BRAKE,
            device_binding_t::TARGET_GEAR1, device_binding_t::TARGET_GEAR2,
            device_binding_t::TARGET_START, device_binding_t::TARGET_COIN,
            device_binding_t::TARGET_MENU,  device_binding_t::TARGET_VIEW,
            device_binding_t::TARGET_UP,    device_binding_t::TARGET_DOWN,
            device_binding_t::TARGET_LEFT,  device_binding_t::TARGET_RIGHT,
        };
        for (int i = 0; i < 12; i++)
            add(pad_targets[i], device_binding_t::TYPE_BUTTON, config.controls.padconfig[i]);

        // Analog axis cells - axis[0..2], honouring the legacy ANALOG setting so behaviour is not
        // silently changed: STEER whenever analog is on, ACCEL/BRAKE only in full-analog mode.
        // These replace the matching button cell (set_binding is keyed on group+target). invert
        // seeds from the legacy controls.invert[1]/[2] flags for ACCEL/BRAKE; the legacy path never
        // inverted steering, so STEER always migrates with invert = false.
        if (config.controls.analog != 0)
            add(device_binding_t::TARGET_STEER, device_binding_t::TYPE_AXIS, config.controls.axis[0], false);
        if (config.controls.analog == 1)
        {
            add(device_binding_t::TARGET_ACCEL, device_binding_t::TYPE_AXIS, config.controls.axis[1], config.controls.invert[1] != 0);
            add(device_binding_t::TARGET_BRAKE, device_binding_t::TYPE_AXIS, config.controls.axis[2], config.controls.invert[2] != 0);
        }
    }

    config.controls.bindings_active = true;
    input.rebuild_binding_cache();
}

// ------------------------------------------------------------------------------------------------
// Stage 4b (tools/INPUT_BINDING_DESIGN.md B.6.2): ADVANCED HID SETUP editor - STATE_RUMBLE_EDITOR, entered
// from menu_feedback's ENTRY_RUMBLE_ADVANCED (which populates hid_edit and calls rumble::stop_all()
// before switching state, so only the TEST actions below ever write while this screen is open) and
// dispatched from tick_ui() the same way STATE_REDEFINE_JOY dispatches to redefine_joystick().
//
// Edits live in `hid_edit` (a Menu member, not a config-backed field) until SAVE CUSTOM; MENU
// discards it. Every editable row exposes one uniform interaction (per B.6.2): ENTER (select_pressed(),
// i.e. START/ACCEL/GEAR1 - same activation used everywhere else in this menu) = +1, ACCEL = +16
// (0x10), BRAKE = -1; UP/DOWN move between rows, LEFT/RIGHT between fields within a row (a byte
// index for the ON/OFF/PULSE/TEMPLATE rows, or one of several actions for the DEVICE/TEST/ACTIONS
// rows, where ENTER activates the selected action instead of adjusting a number).
// ------------------------------------------------------------------------------------------------
void Menu::rumble_editor()
{
    enum RowKind
    {
        ROW_DEVICE, ROW_LEN, ROW_REPORTID, ROW_MODE,
        ROW_ON, ROW_OFF, ROW_PULSE,                          // PRESETS mode rows
        ROW_TEMPLATE, ROW_LOWBYTE, ROW_HIGHBYTE, ROW_SCALE,  // INTENSITY mode rows
        ROW_KEEPALIVE, ROW_TEST, ROW_ACTIONS
    };

    // 10 bytes at 3 columns each is the most that fits the 40-column row after the "TEMPLATE  " label.
    constexpr int HID_UI_MAX_LEN = 10;
    // Keepalive re-sends the last non-OFF report for devices whose rumble times out; periods
    // beyond two seconds are not useful.
    constexpr int HID_UI_MAX_KEEPALIVE_MS = 2000;

    // Row list depends on hid_edit.mode - PRESETS shows ON/OFF/PULSE, INTENSITY shows
    // TEMPLATE/LOW BYTE/HIGH BYTE/SCALE instead (B.6.2).
    std::vector<RowKind> rows;
    rows.push_back(ROW_DEVICE);
    rows.push_back(ROW_LEN);
    rows.push_back(ROW_REPORTID);
    rows.push_back(ROW_MODE);
    if (hid_edit.mode == rumble::HID_PRESETS)
    {
        rows.push_back(ROW_ON);
        rows.push_back(ROW_OFF);
        rows.push_back(ROW_PULSE);
    }
    else
    {
        rows.push_back(ROW_TEMPLATE);
        rows.push_back(ROW_LOWBYTE);
        rows.push_back(ROW_HIGHBYTE);
        rows.push_back(ROW_SCALE);
    }
    rows.push_back(ROW_KEEPALIVE);
    rows.push_back(ROW_TEST);
    rows.push_back(ROW_ACTIONS);

    const int row_count = static_cast<int>(rows.size());
    if (hid_edit_row >= row_count) hid_edit_row = row_count - 1;
    if (hid_edit_row < 0) hid_edit_row = 0;

    const int len = std::max(1, std::min<int>(HID_UI_MAX_LEN, hid_edit.report_len));

    auto field_count = [&](RowKind k) -> int
    {
        switch (k)
        {
            case ROW_ON: case ROW_OFF: case ROW_PULSE: case ROW_TEMPLATE: return len;
            case ROW_TEST:    return 3; // TEST ON / TEST OFF / TEST PULSE
            case ROW_ACTIONS: return 3; // SAVE CUSTOM / RESET / BACK
            default:          return 1;
        }
    };

    RowKind cur = rows[hid_edit_row];
    int fcount = field_count(cur);
    if (hid_edit_field >= fcount) hid_edit_field = fcount - 1;
    if (hid_edit_field < 0) hid_edit_field = 0;

    // ---- Input ----------------------------------------------------------------------------
    if (input.has_pressed(Input::MENU))
    {
        state = STATE_MENU; // discard hid_edit - only SAVE CUSTOM below persists anything
        refresh_menu();
        return;
    }

    if (input.has_pressed(Input::DOWN) || oinputs.is_analog_l())
    {
        hid_edit_row = (hid_edit_row + 1) % row_count;
        hid_edit_field = 0;
    }
    else if (input.has_pressed(Input::UP) || oinputs.is_analog_r())
    {
        hid_edit_row = (hid_edit_row - 1 + row_count) % row_count;
        hid_edit_field = 0;
    }
    else if (input.has_pressed(Input::RIGHT))
    {
        hid_edit_field = (hid_edit_field + 1) % fcount;
    }
    else if (input.has_pressed(Input::LEFT))
    {
        hid_edit_field = (hid_edit_field - 1 + fcount) % fcount;
    }
    else
    {
        bool press_brake = input.has_pressed(Input::BRAKE);
        bool press_accel = input.has_pressed(Input::ACCEL);
        bool press_enter = select_pressed(); // START/ACCEL/GEAR1 (or cabinet START/analog-select)
        int  delta = press_brake ? -1 : press_accel ? 16 : (press_enter ? 1 : 0);

        if (cur == ROW_DEVICE && press_enter)
        {
            rumble::HidTargetInfo t = rumble::hid_target_info();
            if (t.present)
            {
                hid_edit.vid = t.vid;
                hid_edit.pid = t.pid;
                display_message("VID/PID SET FROM DEVICE");
            }
            else
                display_message("NO RUMBLE DEVICE ATTACHED");
        }
        else if (cur == ROW_TEST && press_enter)
        {
            rumble::HidTestAction action =
                hid_edit_field == 0 ? rumble::HID_TEST_ON :
                hid_edit_field == 1 ? rumble::HID_TEST_OFF : rumble::HID_TEST_PULSE;
            display_message(rumble::hid_test(hid_edit, action) ? "TEST SENT" : "TEST FAILED - CHECK VID/PID");
        }
        else if (cur == ROW_ACTIONS && press_enter)
        {
            if (hid_edit_field == 0) // SAVE AS CUSTOM (B.6.2) - only this writes config
            {
                rumble::save_custom_hid_profile(hid_edit);
                display_message("SAVED - USE SAVE AND RETURN TO KEEP");
            }
            else if (hid_edit_field == 1) // RESET TO SUGGESTED
            {
                int suggested = 0;
                for (int i = 0; i < rumble::hid_profile_count(); i++)
                {
                    if (rumble::hid_profile_matches_target(i)) { suggested = i; break; }
                }
                hid_edit = rumble::hid_profile_snapshot(suggested);
                rumble::HidTargetInfo t = rumble::hid_target_info();
                if (t.present) { hid_edit.vid = t.vid; hid_edit.pid = t.pid; }
                hid_edit_field = 0;
                display_message("RESET TO SUGGESTED PROFILE");
            }
            else // BACK
            {
                state = STATE_MENU;
                refresh_menu();
                return;
            }
        }
        else if (delta != 0)
        {
            switch (cur)
            {
                case ROW_LEN:
                {
                    // Wraps 1..HID_UI_MAX_LEN instead of clamping (+1 past the top rolls to 1,
                    // -1 below the bottom rolls to the top); modular so ACCEL's +16 step works.
                    int v = ((hid_edit.report_len - 1 + delta) % HID_UI_MAX_LEN + HID_UI_MAX_LEN) % HID_UI_MAX_LEN;
                    hid_edit.report_len = v + 1;
                    break;
                }
                case ROW_REPORTID:
                    hid_edit.report_id = std::max(-1, std::min(255, hid_edit.report_id + delta));
                    break;
                case ROW_MODE:
                    hid_edit.mode = (hid_edit.mode == rumble::HID_PRESETS) ? rumble::HID_INTENSITY : rumble::HID_PRESETS;
                    hid_edit_row = 0; hid_edit_field = 0; // row layout just changed shape
                    break;
                case ROW_ON:       hid_edit.on[hid_edit_field]    = static_cast<uint8_t>(hid_edit.on[hid_edit_field]    + delta); break;
                case ROW_OFF:      hid_edit.off[hid_edit_field]   = static_cast<uint8_t>(hid_edit.off[hid_edit_field]   + delta); break;
                case ROW_PULSE:    hid_edit.pulse[hid_edit_field] = static_cast<uint8_t>(hid_edit.pulse[hid_edit_field] + delta); break;
                case ROW_TEMPLATE: hid_edit.tmpl[hid_edit_field]  = static_cast<uint8_t>(hid_edit.tmpl[hid_edit_field]  + delta); break;
                case ROW_LOWBYTE:
                {
                    // Wraps -1..len-1 (len+1 values) instead of clamping, both directions;
                    // modular so ACCEL's +16 step works.
                    int span = len + 1;
                    hid_edit.pos_low = ((hid_edit.pos_low + 1 + delta) % span + span) % span - 1;
                    break;
                }
                case ROW_HIGHBYTE:
                {
                    int span = len + 1;
                    hid_edit.pos_high = ((hid_edit.pos_high + 1 + delta) % span + span) % span - 1;
                    break;
                }
                case ROW_SCALE:
                    hid_edit.scale = (hid_edit.scale == rumble::HID_NIBBLE_HI) ? rumble::HID_BYTE : rumble::HID_NIBBLE_HI;
                    break;
                case ROW_KEEPALIVE:
                {
                    // Wraps 0..HID_UI_MAX_KEEPALIVE_MS instead of clamping; modular so ACCEL's
                    // +16 step works.
                    int span = HID_UI_MAX_KEEPALIVE_MS + 1;
                    hid_edit.keepalive_ms = ((hid_edit.keepalive_ms + delta) % span + span) % span;
                    break;
                }
                default: break;
            }
        }
    }

    // ---- Drawing (40x28 text grid; see report for exact column/row map) -------------------
    auto hex2 = [](uint8_t b) { char buf[3]; std::snprintf(buf, sizeof(buf), "%02X", b); return std::string(buf); };
    auto row_colour = [&](RowKind k) { return static_cast<uint16_t>(rows[hid_edit_row] == k ? ohud.PINK : ohud.GREEN); };

    ohud.blit_text_new(7, 2, "HID RUMBLE PROFILE EDITOR", ohud.GREEN);

    rumble::HidTargetInfo t = rumble::hid_target_info();
    {
        char buf[40];
        if (t.present)
            std::snprintf(buf, sizeof(buf), "DEVICE - %.20s %04X %04X", t.name.c_str(), t.vid, t.pid);
        else
            std::snprintf(buf, sizeof(buf), "DEVICE - NONE ATTACHED");
        ohud.blit_text_new(1, 4, buf, ohud.GREY);
    }

    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "VID PID %04X %04X   USE DEVICE", hid_edit.vid, hid_edit.pid);
        ohud.blit_text_new(1, 6, buf, row_colour(ROW_DEVICE));
    }
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "REPORT LENGTH  %d", hid_edit.report_len);
        ohud.blit_text_new(1, 7, buf, row_colour(ROW_LEN));
    }
    {
        char buf[32];
        if (hid_edit.report_id < 0) std::snprintf(buf, sizeof(buf), "REPORT ID      NONE");
        else                        std::snprintf(buf, sizeof(buf), "REPORT ID      %d", hid_edit.report_id);
        ohud.blit_text_new(1, 8, buf, row_colour(ROW_REPORTID));
    }
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "MODE           %s", HID_MODE_LABELS[hid_edit.mode == rumble::HID_INTENSITY ? 1 : 0]);
        ohud.blit_text_new(1, 9, buf, row_colour(ROW_MODE));
    }

    auto draw_byte_row = [&](int y, const char* label, const uint8_t* bytes, RowKind kind)
    {
        std::string line = label;
        for (int i = 0; i < len; i++)
        {
            line += hex2(bytes[i]);
            if (i != len - 1) line += ' ';
        }
        ohud.blit_text_new(1, y, line.c_str(), ohud.GREEN);
        if (rows[hid_edit_row] == kind) // selected row: overlay ONLY the current byte in PINK
        {                                // (label and other bytes stay GREEN so the field stands out)
            int x = 1 + static_cast<int>(std::strlen(label)) + hid_edit_field * 3;
            ohud.blit_text_new(x, y, hex2(bytes[hid_edit_field]).c_str(), ohud.PINK);
        }
    };

    if (hid_edit.mode == rumble::HID_PRESETS)
    {
        draw_byte_row(11, "ON     ", hid_edit.on,    ROW_ON);
        draw_byte_row(12, "OFF    ", hid_edit.off,   ROW_OFF);
        draw_byte_row(13, "PULSE  ", hid_edit.pulse, ROW_PULSE);
    }
    else
    {
        draw_byte_row(11, "TEMPLATE  ", hid_edit.tmpl, ROW_TEMPLATE);
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "LOW BYTE   %d", hid_edit.pos_low);
            ohud.blit_text_new(1, 12, buf, row_colour(ROW_LOWBYTE));
        }
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "HIGH BYTE  %d", hid_edit.pos_high);
            ohud.blit_text_new(1, 13, buf, row_colour(ROW_HIGHBYTE));
        }
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "SCALE      %s", HID_SCALE_LABELS[hid_edit.scale == rumble::HID_BYTE ? 1 : 0]);
            ohud.blit_text_new(1, 14, buf, row_colour(ROW_SCALE));
        }
    }

    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "KEEPALIVE  %d MS", hid_edit.keepalive_ms);
        ohud.blit_text_new(1, 15, buf, row_colour(ROW_KEEPALIVE));
    }

    {
        bool sel = rows[hid_edit_row] == ROW_TEST;
        ohud.blit_text_new(1,  17, "TEST ON",    static_cast<uint16_t>(sel && hid_edit_field == 0 ? ohud.PINK : ohud.GREEN));
        ohud.blit_text_new(11, 17, "TEST OFF",   static_cast<uint16_t>(sel && hid_edit_field == 1 ? ohud.PINK : ohud.GREEN));
        ohud.blit_text_new(22, 17, "TEST PULSE", static_cast<uint16_t>(sel && hid_edit_field == 2 ? ohud.PINK : ohud.GREEN));
    }
    {
        bool sel = rows[hid_edit_row] == ROW_ACTIONS;
        ohud.blit_text_new(1,  19, "SAVE CUSTOM", static_cast<uint16_t>(sel && hid_edit_field == 0 ? ohud.PINK : ohud.GREEN));
        ohud.blit_text_new(14, 19, "RESET",       static_cast<uint16_t>(sel && hid_edit_field == 1 ? ohud.PINK : ohud.GREEN));
        ohud.blit_text_new(22, 19, "BACK",        static_cast<uint16_t>(sel && hid_edit_field == 2 ? ohud.PINK : ohud.GREEN));
    }

    ohud.blit_text_new(1, 22, "UP/DOWN ROW   LEFT/RIGHT FIELD", ohud.GREY);
    ohud.blit_text_new(1, 23, "ENTER +1   ACCEL +16   BRAKE -1", ohud.GREY);
    ohud.blit_text_new(1, 24, "MENU - EXIT WITHOUT SAVING", ohud.GREY);
}

// Display a contextual message in the top left of the screen
void Menu::display_message(std::string s)
{
    msg = " " + s;
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
    if (settings == 1) {
        // Enhanced Settings
        if (!config.sound.fix_samples)
        {
            if (roms.load_pcm_rom(true) == 0)
                config.sound.fix_samples = 1;
        }

        config.video.scale          = 1;
        config.video.widescreen     = 0;
        config.video.hires_next     = 1;
        config.video.shader_mode    = video_settings_t::SHADER_FULL;
        config.video.shadow_mask    = video_settings_t::SHADOW_MASK_SHADER;
        config.video.maskDim        = 75;
        config.video.maskBoost      = 125;
        config.video.scanlines      = 0;
        config.video.crt_shape      = 1;
        config.video.vignette       = 30;
        config.video.noise          = 6;
        config.video.warpX          = 1;
        config.video.warpY          = 3;
        config.video.desaturate     = 5;
        config.video.desaturate_edges = 4;
        config.video.brightboost    = 1;
        config.video.blargg         = video_settings_t::BLARGG_COMPOSITE;
        config.video.saturation     = 30;
        config.video.contrast       = 0;
        config.video.brightness     = 0;
        config.video.sharpness      = 0;
        config.video.resolution     = 0;
        config.video.gamma          = 0;
        config.video.hue            = -2;

        config.engine.level_objects = 1;
        config.engine.new_attract   = 1;
        config.engine.fix_bugs      = 1;

        config.sound.preview        = 1;

        // try to save the settings
        display_message(config.save() ? "SETTINGS SAVED" : "ERROR SAVING SETTINGS!");

        // flag to restart the video subsystem
        config.videoRestartRequired = true;
    }
    else if (settings == 2) {
        // Original Settings
        if (config.sound.fix_samples)
        {
            if (roms.load_pcm_rom(false) == 0)
                config.sound.fix_samples = 0;
        }

        config.video.scale          = 1;
        config.video.widescreen     = 0;
        config.video.hires_next     = 0;
        config.video.shader_mode    = video_settings_t::SHADER_OFF;
        config.video.shadow_mask    = video_settings_t::SHADOW_MASK_OFF;
        config.video.maskDim        = 100;
        config.video.maskBoost      = 100;
        config.video.scanlines      = 0;
        config.video.crt_shape      = 0;
        config.video.vignette       = 0;
        config.video.noise          = 0;
        config.video.warpX          = 0;
        config.video.warpY          = 0;
        config.video.desaturate     = 0;
        config.video.desaturate_edges = 0;
        config.video.brightboost    = 0;
        config.video.blargg         = video_settings_t::BLARGG_DISABLE;
        config.video.saturation     = 0;
        config.video.contrast       = 0;
        config.video.brightness     = 0;
        config.video.sharpness      = 0;
        config.video.resolution     = 0;
        config.video.gamma          = 0;
        config.video.hue            = 0;

        config.engine.level_objects = 0;
        config.engine.new_attract   = 0;
        config.engine.fix_bugs      = 0;

        config.sound.preview        = 0;

        // try to save the settings
        display_message(config.save() ? "SETTINGS SAVED" : "ERROR SAVING SETTINGS!");

        // flag to restart the video subsystem
        config.videoRestartRequired = true;
    }
    else {
        // Otherwise, use whatever is already setup...
        config.engine.fix_bugs = config.engine.fix_bugs_backup;
    }

    if (check_jap_roms())
    {
        outrun.cannonball_mode = mode;
        cannonball::state = cannonball::STATE_INIT_GAME;
        osoundint.queue_clear();
    }
}
