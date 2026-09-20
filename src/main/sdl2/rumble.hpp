/***************************************************************************
    Rumble / force-feedback subsystem - platform-agnostic game-side API.

    Replaces the hard-coded Thrustmaster hidraw bytes and mixed concerns
    formerly in Input::set_rumble() with a small mixer, backend selection
    (SDL_GameControllerRumble/SDL_JoystickRumble, SDL_Haptic, a profile-driven
    raw-HID backend, and an adapter over the existing forcefeedback::
    namespace), and the safety rules in tools/INPUT_BINDING_DESIGN.md Deliverable B
    (B.7). This header is the *only* thing engine/main code should need - it
    never exposes InputDevice or any backend type (see rumble_backend.hpp
    for those).

    Stage 4a implemented the CORE + SDL/Haptic/FFB backends only; HID_REPORT
    had no backend behind it and behaved like OFF if selected.

    Stage 4b (tools/INPUT_BINDING_DESIGN.md B.3-B.7) adds the HID_REPORT backend
    itself (rumble_backend_hid.cpp): a built-in profile table (Thrustmaster
    3-byte reports, byte-identical to the removed Input::set_rumble()), one
    user-editable custom profile, and the menu-facing HidProfileEdit/
    HidTargetInfo surface below so the FEEDBACK submenu and the ADVANCED HID
    SETUP editor never need to see the internal HidRumbleProfile/
    RumbleBackend types (rumble_backend.hpp).

    - CRASH/GEAR/START_REV exist in the Effect enum and the mixer's
      priority table so Stage 4c can start driving them with no API
      changes, but nothing calls set_level()/event() for them yet.

    Copyright (c) 2025, 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include <string>

#include "stdint.hpp"

namespace rumble
{
    // Effects the mixer understands (tools/INPUT_BINDING_DESIGN.md B.2.1/B.2.2). Priority when several
    // are simultaneously active: CRASH > GEAR > START_REV > MOTOR > SKID (highest wins; no
    // summing). TEST outranks everything else - it is a deliberate, exclusive user action from
    // the FEEDBACK menu, never concurrent with real gameplay effects.
    enum Effect
    {
        MOTOR,
        SKID,
        CRASH,
        GEAR,
        START_REV,
        TEST,
        EFFECT_COUNT
    };

    // Selectable backend (menu ENTRY_RUMBLE_BACKEND, config.controls.rumble_backend).
    enum Backend
    {
        AUTO,
        SDL_CONTROLLER,
        SDL_HAPTIC,
        HID_REPORT,   // Stage 4b: profile-driven hidraw/SDL_hid_write backend (rumble_backend_hid.cpp)
        FFB,
        OFF
    };

    // Lifecycle -----------------------------------------------------------------------------
    void init();                 // main.cpp, right after input.init(); no devices exist yet, so
                                  // this just loads the master strength and lands on OFF until
                                  // on_devices_changed() resolves a real target + backend.
    void shutdown();              // main.cpp quit_func(): stop_all() + release the active backend.
    void on_devices_changed();    // call after every Input::add_joystick()/remove_joystick(); always
                                   // stop_all()s first, then re-picks the target device and backend.

    // Config surface --------------------------------------------------------------------------
    void set_master(float strength01);            // mirrors config.controls.rumble (0..1, already clamped)
    Backend     configured_backend();              // config.controls.rumble_backend, as an enum
    void        set_backend(Backend backend);      // persists to config, stop_all()s, re-selects
    const char* active_backend_name();             // e.g. "SDL", "SDL HAPTIC", "FORCE FEEDBACK", "NONE"

    // Per-tick game-side API ------------------------------------------------------------------
    void set_level(Effect effect, float intensity01);                    // continuous; caller refreshes every tick_frame
    void event(Effect effect, float intensity01, uint16_t duration_ms);  // one-shot, self-expiring
    void tick();                  // once per tick_frame: expire one-shots, mix, apply safety, write
    void stop_all();              // force every effect + the backend off now (mode/device/backend
                                   // changes, editor entry, shutdown - tools/INPUT_BINDING_DESIGN.md B.7.5)

    // Menu TEST RUMBLE entry: shares the same one-write-per-tick limiter and cannot be
    // re-triggered while a previous test is still running. Returns false if there is no usable
    // backend/device or a test is already in flight.
    bool test(Effect effect, uint16_t duration_ms);

    // ============================================================================================
    // Stage 4b - raw-HID rumble profiles (tools/INPUT_BINDING_DESIGN.md B.3, B.6). Only plain data crosses
    // this boundary: the internal HidRumbleProfile catalogue and RumbleBackend types never do (see
    // rumble_backend.hpp, included only by rumble.cpp and the backend .cpp files).
    // ============================================================================================

    enum HidMode  { HID_PRESETS, HID_INTENSITY };              // HidRumbleProfile::Mode, B.3.1
    enum HidScale { HID_NIBBLE_HI, HID_BYTE };                  // HidRumbleProfile::Scale, B.3.1
    enum HidTestAction { HID_TEST_ON, HID_TEST_OFF, HID_TEST_PULSE }; // B.6.2 TEST ON/OFF/PULSE

    // Editable/scratch form of one profile for the ADVANCED HID SETUP editor (B.6.2). report_len
    // is clamped to 1..MAX_LEN in the UI (the on-disk/model cap is 64 per B.3.1, but no built-in
    // profile or realistic HID rumble report needs more than a handful of bytes).
    struct HidProfileEdit
    {
        static constexpr int MAX_LEN = 16; // constexpr (not plain static const): ODR-used by
                                            // std::min/std::max call sites across several .cpp
                                            // files; C++17 makes a constexpr static data member
                                            // implicitly inline, so no out-of-line definition is
                                            // needed anywhere.

        std::string name = "CUSTOM";
        uint16_t    vid = 0, pid = 0;
        int         report_len = 3;
        int         report_id  = -1;      // -1 = unnumbered report; 0..255 = numbered report prefix
        HidMode     mode = HID_PRESETS;
        uint8_t     on[MAX_LEN]    = { 0 };
        uint8_t     off[MAX_LEN]   = { 0 };
        uint8_t     pulse[MAX_LEN] = { 0 };
        uint8_t     tmpl[MAX_LEN]  = { 0 };
        int         pos_low = -1, pos_high = -1;
        HidScale    scale = HID_NIBBLE_HI;
        int         keepalive_ms = 0;
    };

    // The device rumble::select_target_device() currently points at, for the FEEDBACK/editor
    // "DEVICE: <name> VVVV:PPPP" row (B.3.3). present == false when no device is available.
    struct HidTargetInfo
    {
        bool        present = false;
        std::string name;
        uint16_t    vid = 0, pid = 0;
    };
    HidTargetInfo hid_target_info();

    int         hid_profile_count();                    // built-in profiles + CUSTOM, menu cycle order
    const char* hid_profile_name(int index);             // menu text, e.g. "THRUSTMASTER 3-BYTE", "CUSTOM"
    bool        hid_profile_matches_target(int index);   // B.7.4 arming rule vs the current target device
    int         hid_selected_profile_index();            // config.controls.rumble_hid_profile, as an index; -1 if unknown
    void        set_hid_profile_index(int index);        // persists + stop_all() + re-selects (B.7.5)

    HidProfileEdit hid_profile_snapshot(int index);       // load a catalogue entry (or the stored custom) into scratch form
    void        save_custom_hid_profile(const HidProfileEdit& profile); // B.6.2 SAVE AS CUSTOM only

    // B.6.2 TEST ON/OFF/PULSE: writes the scratch profile's bytes directly (not through the
    // Effect/mixer path), sharing the same TEST limiter as the menu's TEST RUMBLE entry (B.7.6)
    // and refusing to write unless profile.vid/pid matches the current target device (B.3.2/B.7.4).
    // TEST_ON/TEST_PULSE auto-turn-off after the same 500 ms cap as the ordinary TEST effect.
    bool        hid_test(const HidProfileEdit& profile, HidTestAction action);
}
