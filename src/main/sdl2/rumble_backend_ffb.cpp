/***************************************************************************
    Rumble subsystem - force-feedback backend (Stage 4a).

    A thin adapter over the existing, untouched forcefeedback:: namespace
    (src/main/directx/ffeedback.cpp: evdev on Linux, DirectInput on
    Windows). This file must never modify ffeedback.cpp/.hpp; it only calls
    the four functions that namespace already exports.

    Gating (tools/INPUT_BINDING_DESIGN.md B.2.3): AUTO only ever tries this backend
    when controls.analog.haptic/@enabled is on and the machine is not a
    SmartyPi *moving* cabinet, whose steering motor owns forcefeedback::
    exclusively (ooutputs.cpp). An explicit user selection of FORCE FEEDBACK
    in the menu is honoured as long as haptic is enabled; a moving cabinet
    still refuses it, to avoid fighting the cabinet motor for the same
    device.

    forcefeedback::init() is normally already called once by main.cpp
    (gated on the startup value of controls.haptic); open() below also
    calls it lazily so that switching the rumble backend to FORCE FEEDBACK
    at runtime works even if haptic was off at boot.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#include <algorithm>

#include "sdl2/rumble_backend.hpp"
#include "sdl2/input.hpp"       // InputDevice (unused by this backend, kept for interface parity)
#include "frontend/config.hpp"  // Config / controls.haptic / smartypi
#include "directx/ffeedback.hpp"

namespace rumble
{
    class FfbBackend : public RumbleBackend
    {
    public:
        bool open(const InputDevice& /*dev*/) override
        {
            if (!config.controls.haptic)
                return false; // B.2.3: FFB only when haptic is enabled in config

            if (config.smartypi.enabled && config.smartypi.cabinet == Config::CABINET_MOVING)
                return false; // the cabinet motor owns forcefeedback:: on a moving cabinet

            if (!forcefeedback::is_supported())
            {
                if (!forcefeedback::init(config.controls.max_force, config.controls.min_force, config.controls.force_duration))
                    return false;
            }
            return true;
        }

        bool play(const RumbleFrame& frame) override
        {
            if (!forcefeedback::is_supported())
                return false;

            uint16_t peak = std::max(frame.low, frame.high);
            if (peak == 0)
            {
                stop();
                return true;
            }

            // B.2.3: level = 1 + int((1 - max(low,high)/65535) * 4) -> 1 strong .. 5 soft.
            int level = 1 + static_cast<int>((1.0f - std::min(1.0f, peak / 65535.0f)) * 4.0f);

            // RumbleFrame carries no effect tag (B.2.2); approximate "effect == SKID" from the
            // frame's own shape (SKID is the only Stage-4a effect with low == 0, high > 0) so the
            // existing forcefeedback::set(mode, level) ABI - unchanged, mode otherwise unused -
            // keeps distinguishing the skid pulse from the plain motor buzz, as it did before.
            int mode = (frame.low == 0 && frame.high > 0) ? 1 : 0;

            return forcefeedback::set(mode, level) == 0;
        }

        void stop() override
        {
            // forcefeedback:: exposes no per-effect stop (only init/set/close/is_supported); the
            // uploaded effect's own short, fixed duration (controls.force_duration, a few tens of
            // ms) lapses on its own once play() stops being called, well inside one 33 ms tick.
            // close() would tear down every uploaded effect - including the SmartyPi cabinet
            // motor's use of the same singleton - so it is deliberately not called here; see
            // tools/INPUT_BINDING_DESIGN.md B.2.3 (FFB row) and B.7.5.
        }

        void close() override
        {
            // Does not own the forcefeedback:: singleton - main.cpp's quit_func() closes it.
        }

        const char* name() const override { return "FORCE FEEDBACK"; }
    };

    RumbleBackend* create_ffb_backend() { return new FfbBackend(); }
}
