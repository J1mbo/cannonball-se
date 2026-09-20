/***************************************************************************
    Front End Menu System.

    This file is part of Cannonball.
    Copyright (c) Chris White.
    See license.txt for more details.

    Modifications for CannonBall-SE Copyright (c) 2025, 2026 James Pearce
***************************************************************************/

#pragma once

#include <vector>
#include <string>
#include "stdint.hpp"
#include "sdl2/rumble.hpp" // rumble::HidProfileEdit (Stage 4b editor scratch state)
#include "sdl2/input.hpp"  // Input::Captured (Stage 3 matrix editor state)

class CabDiag;
class TTrial;

class Menu
{
public:
    Menu();
    ~Menu(void);

    void populate();
    void init(bool init_main_menu = true);
    void tick();
    void restart_video();

private:
    CabDiag* cabdiag;

    // Menu state
    uint8_t state;

    enum
    {
        STATE_MENU,
        STATE_REDEFINE_KEYS,
        STATE_REDEFINE_JOY,
        STATE_TTRIAL,
        STATE_DIAGNOSTICS,
        STATE_RUMBLE_EDITOR,   // Stage 4b: ADVANCED HID SETUP (tools/INPUT_BINDING_DESIGN.md B.6.2)
    };

    TTrial* ttrial;

    // Music track for music test menu
    int music_track;

    // Redefine keys/joystick substate
    uint8_t redef_state;

    uint32_t frame;

    // Counter for showing messages
    int32_t message_counter;

    // Number of seconds to show message for
    const static int32_t MESSAGE_TIME = 5;

    // Message text
    std::string msg;

    // Cursor
    int16_t cursor;

    struct menu_pair
    {
        int16_t cursor;
        std::vector<std::string>* menu;
    };

    std::vector<menu_pair> menu_stack;

    // Stores whether this is a textual menu (i.e. no options that can be chosen)
    bool is_text_menu;

    // Used to control the horizon pan effect
    uint16_t horizon_pos;

    std::vector<std::string>* menu_selected;
    std::vector<std::string> menu_main;
    std::vector<std::string> menu_gamemodes;
    std::vector<std::string> menu_cont;
    std::vector<std::string> menu_timetrial;
    std::vector<std::string> menu_about;
    std::vector<std::string> menu_settings;
    std::vector<std::string> menu_video;
    // JJP - CRT Emulation related menus
    std::vector<std::string> menu_crt_shader1;
    std::vector<std::string> menu_crt_shader2;
    std::vector<std::string> menu_crt_shape_settings;
    std::vector<std::string> menu_crt_mask_settings;
    std::vector<std::string> menu_blargg_filter;
    // JJP - end of insert
    std::vector<std::string> menu_sound;
    std::vector<std::string> menu_controls;
    std::vector<std::string> menu_feedback;      // Stage 4a: RUMBLE AND FEEDBACK... submenu
    std::vector<std::string> menu_engine;
    std::vector<std::string> menu_enhancements;
    std::vector<std::string> menu_handling;
    std::vector<std::string> menu_musictest;
    std::vector<std::string> menu_s_exsettings;     // smartypi specific
    std::vector<std::string> menu_s_tests;          // smartypi specific
    std::vector<std::string> menu_s_dips;           // smartypi specific
    std::vector<std::string> menu_s_enhance;        // smartypi specific

    std::vector<std::string> text_redefine;

    // Stage 4b (tools/INPUT_BINDING_DESIGN.md B.6.2): ADVANCED HID SETUP editor state, kept as Menu members
    // (not function statics) matching the pattern used for the Stage 3 matrix editor rewrite
    // (A.4.2). `hid_edit` is a scratch copy - only SAVE AS CUSTOM persists it via
    // rumble::save_custom_hid_profile(); MENU/BACK discard it.
    rumble::HidProfileEdit hid_edit;
    int16_t  hid_edit_row   = 0;   // selected row (device/len/reportid/mode/bytes.../keepalive/test/actions)
    int16_t  hid_edit_field = 0;   // selected field within the row (byte index, or action index)

    // Stage 3 (tools/INPUT_BINDING_DESIGN.md A.4.2): binding-matrix editor state, kept as Menu members
    // (not function statics). bind_row 0..12 = data rows, 13 = SAVE, 14 = CANCEL; bind_col 0=KEYBOARD,
    // 1=GAMEPAD, 2=WHEEL. bind_phase runs BROWSE -> WAIT_RELEASE -> CAPTURE -> WAIT_RELEASE.
    int16_t          bind_row = 0, bind_col = 0;
    uint8_t          bind_phase = 0;
    Input::Captured  bind_wait;            // control captured, awaited for release after a bind
    int              bind_wait_key = -1;   // scancode awaited for release after a keyboard bind (-1 = none)
    int32_t          bind_msg_timer = 0;
    std::string      bind_msg;

    // Snapshot of the editable binding state, taken on entry to the matrix editor so CANCEL can
    // restore it (including undoing a first-open legacy migration).
    std::vector<device_binding_t> bind_snap_bindings;
    int                            bind_snap_keyconfig[12];
    bool                           bind_snap_active;

    void populate_for_pc();
    void populate_controls();
    void populate_for_cabinet();
    void tick_ui();
    void draw_menu_options();
    void draw_text(std::string);
    void tick_menu();
    bool select_pressed();
    void set_menu(std::vector<std::string>*);
    void menu_back();
    void refresh_menu();
    void set_menu_text(std::string s1, std::string s2);
    void redefine_keyboard();
    void redefine_joystick();      // Stage 3: binding-matrix editor (STATE_REDEFINE_JOY, A.4.2)
    void leave_editor();           // Stage 3: end capture, rebuild cache, return to STATE_MENU
    void migrate_legacy_to_bindings(); // Stage 3: first-open migration of legacy arrays (A.2.2)
    void rumble_editor();          // Stage 4b: STATE_RUMBLE_EDITOR (B.6.2)
    void display_message(std::string);
    bool check_jap_roms();
    void start_game(int mode, int settings = 0);
};
