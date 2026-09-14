/***************************************************************************
    Rumble subsystem - internal backend interface (Stage 4a).

    Shared only by rumble.cpp (mixer, target-device/backend selection, B.7
    safety rules) and the per-API backend implementations
    (rumble_backend_sdl.cpp, rumble_backend_ffb.cpp). This is NOT part of
    the public game-side API - see sdl2/rumble.hpp for that; main.cpp,
    menu.cpp etc. never need to include this file.

    See tools/INPUT_BINDING_DESIGN.md Deliverable B, B.2.3.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "stdint.hpp"

struct InputDevice; // sdl2/input.hpp; only used by reference/pointer here, so the full
                     // definition is not needed by anything that only sees this header.

namespace rumble
{
    // Canonical mixed output (B.2.2). low/high follow SDL_GameControllerRumble semantics
    // (low = strong/low-frequency motor, high = weak/high-frequency motor), 0..65535.
    // duration_ms is the window the backend should consider the frame valid for; continuous
    // effects (MOTOR/SKID) refresh well inside it every tick, so it doubles as a self-expiring
    // safety net if tick() ever stops being called (B.7.5).
    struct RumbleFrame
    {
        uint16_t low = 0;
        uint16_t high = 0;
        uint16_t duration_ms = 0;
    };

    // One platform/API-specific way of turning a RumbleFrame into physical motor movement.
    // rumble.cpp owns all B.7 safety behaviour (edge-triggered writes, one write per tick,
    // 3-strikes-and-disable, the stale-tick guard) - a backend only needs to report success or
    // failure honestly via its return values; it must never throw or block.
    class RumbleBackend
    {
    public:
        virtual ~RumbleBackend() {}

        // Attempt to attach to `dev` as the rumble target. Returns false if this backend cannot
        // drive it (device lacks the capability, API unsupported at compile/runtime, or - FFB -
        // the haptic config/cabinet-type gate says no). Never called with duplicate opens
        // without an intervening close().
        virtual bool open(const InputDevice& dev) = 0;

        // Render one mixed frame. Called at most once per rumble::tick(). Returns false on a
        // write/API failure (counted towards the 3-strikes session disable in rumble.cpp).
        virtual bool play(const RumbleFrame& frame) = 0;

        // Force the motors off immediately. Best-effort; must never throw or assert on failure -
        // rumble.cpp calls this on every safety edge (frame-to-zero, stale tick, stop_all()).
        virtual void stop() = 0;

        // Release whatever open() acquired. Must NOT close the underlying SDL_Joystick/
        // SDL_GameController (or, for FFB, the shared forcefeedback:: singleton) - Input and the
        // SmartyPi cabinet motor own those respectively.
        virtual void close() = 0;

        // Short, menu-facing name, e.g. "SDL", "SDL HAPTIC", "FORCE FEEDBACK". Never null.
        virtual const char* name() const = 0;
    };

    // Factories - each returns a freshly constructed, unopened backend; caller (rumble.cpp)
    // owns it and must call close() then delete it when done.
    RumbleBackend* create_sdl_controller_backend();  // SDL_GameControllerRumble / SDL_JoystickRumble
    RumbleBackend* create_sdl_haptic_backend();       // SDL_HapticOpenFromJoystick + Rumble Init/Play/Stop
    RumbleBackend* create_ffb_backend();              // adapter over forcefeedback:: (ffeedback.cpp)
}
