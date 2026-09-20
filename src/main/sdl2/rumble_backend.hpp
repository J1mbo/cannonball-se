/***************************************************************************
    Rumble subsystem - internal backend interface (Stage 4a, extended 4b).

    Shared only by rumble.cpp (mixer, target-device/backend selection, B.7
    safety rules) and the per-API backend implementations
    (rumble_backend_sdl.cpp, rumble_backend_ffb.cpp, rumble_backend_hid.cpp).
    This is NOT part of the public game-side API - see sdl2/rumble.hpp for
    that; main.cpp, menu.cpp etc. never need to include this file.

    See tools/INPUT_BINDING_DESIGN.md Deliverable B, B.2.3, B.3.

    Copyright (c) 2025, 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include <string>

#include "stdint.hpp"
#include "sdl2/rumble.hpp" // HidProfileEdit/HidTestAction (Stage 4b menu-facing types, reused here
                            // so the internal HID catalogue can be queried/rendered without a
                            // second copy of those structs)

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

        // Stage 4b (B.3.1): non-zero for a backend whose device expects a periodic refresh even
        // when the mixed frame hasn't changed (some HID rumble reports time out and switch the
        // motors off if not repeated). rumble.cpp's edge-triggered write_frame() re-sends the last
        // non-zero frame every this-many ms when the active backend reports one; default 0 means
        // "no keepalive needed" and is unchanged behaviour for every Stage-4a backend.
        virtual uint16_t keepalive_ms() const { return 0; }
    };

    // Factories - each returns a freshly constructed, unopened backend; caller (rumble.cpp)
    // owns it and must call close() then delete it when done.
    RumbleBackend* create_sdl_controller_backend();  // SDL_GameControllerRumble / SDL_JoystickRumble
    RumbleBackend* create_sdl_haptic_backend();       // SDL_HapticOpenFromJoystick + Rumble Init/Play/Stop
    RumbleBackend* create_ffb_backend();              // adapter over forcefeedback:: (ffeedback.cpp)
    RumbleBackend* create_hid_backend();              // Stage 4b: profile -> hidraw | SDL_hid_write

    // --------------------------------------------------------------------------------------------
    // Stage 4b internal HID profile catalogue (rumble_backend_hid.cpp). Built-ins + the one CUSTOM
    // slot (config.controls.rumble_hid_custom), indexed 0..hid_catalogue_count()-1 with CUSTOM
    // always last. HidRumbleProfile itself never leaves rumble_backend_hid.cpp; everything here
    // and in sdl2/rumble.hpp's hid_profile_* wrappers deals only in indices/ids/HidProfileEdit.
    // --------------------------------------------------------------------------------------------
    int            hid_catalogue_count();
    const char*    hid_catalogue_id(int index);                     // "thrustmaster3", "custom", ...
    const char*    hid_catalogue_name(int index);                   // menu text
    bool           hid_catalogue_vendor_match(int index, uint16_t vid, uint16_t pid); // B.7.4: built-in = vendor (+ pid if profile pid != 0); custom = exact vid+pid vs config.controls.rumble_hid_custom
    int            hid_catalogue_index_for_id(const std::string& id); // -1 if unrecognised
    HidProfileEdit hid_catalogue_snapshot(int index);                // catalogue entry -> editor scratch form

    // Renders the wire payload (<= HidProfileEdit::MAX_LEN bytes, NOT including any report-id
    // prefix - see prefix rule below) for one editor TEST action against a scratch HidProfileEdit.
    // Returns the payload length, or 0 if the profile/action is invalid (e.g. INTENSITY
    // pos_low/pos_high out of range).
    int render_hid_edit_action(const HidProfileEdit& profile, HidTestAction action, uint8_t* out);

    // Result of one HID write attempt - distinguishes "no such device"/IO failure from a
    // permissions problem so the FEEDBACK menu can say so explicitly (B.7.7).
    enum class HidWriteResult { OK, FAILED, PERMISSION_DENIED };

    // One-shot, transient open/write/close - used by the editor's TEST buttons, which must be able
    // to write regardless of which backend rumble::AUTO/explicit selection currently has open
    // (B.3.2: a profile can be tested as long as its own vid/pid matches the target device, even
    // if a different backend is currently active). report_id: -1 = unnumbered (hidraw: no prefix;
    // SDL_hid: prefix 0), 0..255 = numbered report (hidraw and SDL_hid both prefix it).
    HidWriteResult hid_raw_write(uint16_t vid, uint16_t pid, int16_t report_id, const uint8_t* payload, int len);

    // True if the most recent hidraw/SDL_hid open or write attempt (from either the persistent
    // HidReportBackend or hid_raw_write()) failed with EACCES - surfaced by rumble.cpp's
    // active_backend_name() as "HID: PERMISSION DENIED" (B.7.7) rather than silently falling back.
    bool hid_permission_denied();
}
