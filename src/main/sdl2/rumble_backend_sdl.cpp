/***************************************************************************
    Rumble subsystem - SDL-native backends (Stage 4a).

    SDL_CONTROLLER: SDL_GameControllerRumble (recognised GameControllers) or
    SDL_JoystickRumble (raw joysticks/wheels) - SDL >= 2.0.9. Probed by
    calling the rumble function with (0,0,0) once at open() time: it returns
    0 on devices that actually support rumble and a negative error
    otherwise, which is the only reliable check on SDL 2.0.9-2.0.17 (before
    SDL_GameControllerHasRumble existed in 2.0.18) - the same probe DX uses,
    described in tools/INPUT_BINDING_DESIGN.md B.2.3.

    SDL_HAPTIC: SDL_HapticOpenFromJoystick + SDL_HapticRumbleInit/Play/Stop.
    This is the haptic setup that used to live inline in
    Input::open_legacy_device() (pre-Stage-4a) - moved here verbatim in
    spirit, now scoped to whichever device the rumble subsystem picks as its
    target rather than only the legacy pad_id device.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#include <SDL.h>
#include <algorithm>

#include "sdl2/rumble_backend.hpp"
#include "sdl2/input.hpp" // InputDevice

namespace rumble
{
    // ------------------------------------------------------------------------------------------
    // SDL_CONTROLLER: SDL_GameControllerRumble (GC devices) / SDL_JoystickRumble (raw joysticks).
    // ------------------------------------------------------------------------------------------
    class SdlControllerBackend : public RumbleBackend
    {
    public:
        bool open(const InputDevice& dev) override
        {
#if SDL_VERSION_ATLEAST(2, 0, 9)
            controller_ = dev.controller;
            joystick_   = dev.joystick;

            int rc = controller_ ? SDL_GameControllerRumble(controller_, 0, 0, 0)
                                  : (joystick_ ? SDL_JoystickRumble(joystick_, 0, 0, 0) : -1);
            if (rc != 0)
            {
                controller_ = nullptr;
                joystick_   = nullptr;
                return false;
            }
            return true;
#else
            (void)dev;
            return false; // SDL headers older than 2.0.9 at compile time: no rumble API at all
#endif
        }

        bool play(const RumbleFrame& frame) override
        {
#if SDL_VERSION_ATLEAST(2, 0, 9)
            if (controller_)
                return SDL_GameControllerRumble(controller_, frame.low, frame.high, frame.duration_ms) == 0;
            if (joystick_)
                return SDL_JoystickRumble(joystick_, frame.low, frame.high, frame.duration_ms) == 0;
#else
            (void)frame;
#endif
            return false;
        }

        void stop() override
        {
#if SDL_VERSION_ATLEAST(2, 0, 9)
            if (controller_) SDL_GameControllerRumble(controller_, 0, 0, 0);
            else if (joystick_) SDL_JoystickRumble(joystick_, 0, 0, 0);
#endif
        }

        void close() override
        {
            // Does not own controller_/joystick_ - Input closes the underlying SDL handles.
            controller_ = nullptr;
            joystick_   = nullptr;
        }

        const char* name() const override { return "SDL"; }

    private:
        SDL_GameController* controller_ = nullptr;
        SDL_Joystick*       joystick_   = nullptr;
    };

    // ------------------------------------------------------------------------------------------
    // SDL_HAPTIC: SDL_HapticOpenFromJoystick + SDL_HapticRumbleInit/Play/Stop.
    // ------------------------------------------------------------------------------------------
    class SdlHapticBackend : public RumbleBackend
    {
    public:
        bool open(const InputDevice& dev) override
        {
            SDL_Joystick* joy = dev.controller ? SDL_GameControllerGetJoystick(dev.controller) : dev.joystick;
            if (!joy)
                return false;

            haptic_ = SDL_HapticOpenFromJoystick(joy);
            if (!haptic_)
                return false;

            if (!SDL_HapticRumbleSupported(haptic_) || SDL_HapticRumbleInit(haptic_) != 0)
            {
                SDL_HapticClose(haptic_);
                haptic_ = nullptr;
                return false;
            }
            return true;
        }

        bool play(const RumbleFrame& frame) override
        {
            if (!haptic_)
                return false;

            // B.2.3: strength = max(low, high) / 65535.
            float strength = std::max(frame.low, frame.high) / 65535.0f;
            if (strength <= 0.0f)
            {
                SDL_HapticRumbleStop(haptic_);
                return true;
            }
            return SDL_HapticRumblePlay(haptic_, strength, frame.duration_ms) == 0;
        }

        void stop() override
        {
            if (haptic_) SDL_HapticRumbleStop(haptic_);
        }

        void close() override
        {
            if (haptic_)
            {
                SDL_HapticClose(haptic_);
                haptic_ = nullptr;
            }
        }

        const char* name() const override { return "SDL HAPTIC"; }

    private:
        SDL_Haptic* haptic_ = nullptr;
    };

    RumbleBackend* create_sdl_controller_backend() { return new SdlControllerBackend(); }
    RumbleBackend* create_sdl_haptic_backend()     { return new SdlHapticBackend(); }
}
