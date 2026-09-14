/***************************************************************************
    Rumble / force-feedback subsystem - platform-agnostic game-side API.

    Replaces the hard-coded Thrustmaster hidraw bytes and mixed concerns
    formerly in Input::set_rumble() with a small mixer, backend selection
    (SDL_GameControllerRumble/SDL_JoystickRumble, SDL_Haptic, and an adapter
    over the existing forcefeedback:: namespace), and the safety rules in
    tools/INPUT_BINDING_DESIGN.md Deliverable B (B.7). This header is the *only*
    thing engine/main code should need - it never exposes InputDevice or
    any backend type (see rumble_backend.hpp for those).

    Stage 4a implements the CORE + SDL/Haptic/FFB backends only:
    - HID_REPORT has no backend behind it yet and behaves like OFF if
      selected (built-in profiles / custom editor are Stage 4b).
    - CRASH/GEAR/START_REV exist in the Effect enum and the mixer's
      priority table so Stage 4c can start driving them with no API
      changes, but nothing in Stage 4a calls set_level()/event() for them.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#pragma once

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
        HID_REPORT,   // Stage 4b; no backend implements this yet (behaves like OFF)
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
}
