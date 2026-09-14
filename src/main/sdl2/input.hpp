/***************************************************************************
    SDL Based Input Handling.

    Populates keys array with user input.
    If porting to a non-SDL platform, you would need to replace this class.

    Copyright Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
    - Input binding matrix: multi-device runtime (InputDevice, hot-plug,
      per-device resolved binding cache). See tools/INPUT_BINDING_DESIGN.md Stage 2.
    - Rumble subsystem (Stage 4a): removed set_rumble()/haptic/hidraw_device -
      moved to sdl2/rumble.* - and added get_devices() so the new rumble
      module can pick a target device. See tools/INPUT_BINDING_DESIGN.md Deliverable B.
***************************************************************************/

#pragma once

#include <SDL.h>
#include <string>
#include <vector>

#include "frontend/config.hpp" // device_binding_t

// ------------------------------------------------------------------------------------------------
// One opened SDL joystick / game controller (Stage 2, tools/INPUT_BINDING_DESIGN.md A.3.1).
//
// `resolved[group]` is rebuilt by Input::rebuild_binding_cache() whenever the binding matrix or
// the set of attached devices changes, so that per-event handling (handle_joy_*/handle_controller_*)
// never allocates or builds strings - it is a flat scan over a handful of entries.
// ------------------------------------------------------------------------------------------------
struct InputDevice
{
    SDL_Joystick*       joystick   = nullptr;
    SDL_GameController* controller = nullptr;   // non-null iff SDL_IsGameController(index) at open
    SDL_JoystickID      instance_id = -1;
    std::string         guid_string, signature, name;
    uint16_t            vid = 0, pid = 0;
    int                 axes = 0, buttons = 0, hats = 0;
    bool                legacy_device = false;  // opened at index == controls.pad_id (legacy fallback path)

    // One resolved cell: a (type, index[, hat_mask]) -> target mapping, pre-matched against this
    // device's signature/vid/pid so event handlers only need an integer compare.
    struct Resolved
    {
        uint8_t type;      // device_binding_t::Type
        uint8_t index;     // GC axis/button enum (GAMEPAD) or raw index (WHEEL)
        uint8_t hat_mask;  // SDL_HAT_* mask; unused unless type == TYPE_HAT
        uint8_t target;    // device_binding_t::Target
        uint8_t invert;    // device_binding_t::invert; axis only
    };
    std::vector<Resolved> resolved[device_binding_t::GROUP_COUNT];

    // True if this device has an explicit UP/DOWN/LEFT/RIGHT cell in that group; when false, the
    // navigation defaults (hat 0 / D-pad) apply as a fallback so the menu stays reachable.
    bool nav_bound[device_binding_t::GROUP_COUNT] = { false, false };
};

class Input
{
public:
    enum presses
    {
        LEFT  = 0,
        RIGHT = 1,
        UP    = 2,
        DOWN  = 3,
        ACCEL = 4,
        BRAKE = 5,
        GEAR1 = 6,
        GEAR2 = 7,

        START = 8,
        COIN  = 9,
        VIEWPOINT = 10,

        PAUSE = 11,
        STEP  = 12,
        TIMER = 13,
        MENU = 14,
    };

    bool keys[15];
    bool keys_old[15];

    enum limits
    {
        SW_LEFT   = 0,
        SW_CENTRE = 1,
        SW_RIGHT  = 2,
    };
    bool motor_limits[3];

    // Has gamepad been found?
    bool gamepad;

    // Gamepad supports rumble?
    int rumble_supported;

    // Use analog controls
    int analog;

    // Latch last key press for redefines
    int key_press;

    // Analog Controls
    int wheel, a_wheel;
    int a_accel;
    int a_brake;
    int a_motor;

    Input(void);
    ~Input(void);

    void init(int, int*, int*, const int, int*, bool*, int*);
    void open_joy();                              // full rescan (opens every currently attached joystick)
    void add_joystick(int device_index);          // SDL_JOYDEVICEADDED (device index); de-dups on instance id
    void remove_joystick(SDL_JoystickID id);       // SDL_JOYDEVICEREMOVED (instance id)
    void close_joy();                              // closes all devices
    const InputDevice* find_device(SDL_JoystickID id) const;
    void rebuild_binding_cache();                  // rebuild resolved[]; derive gamepad/analog
    void reset_keys();                             // clear keys[] contributions from joystick groups

    // Stage 4a (tools/INPUT_BINDING_DESIGN.md B.2.3): read-only enumeration for the rumble subsystem's
    // target-device selection. Input owns the device list; rumble.cpp never mutates it.
    const std::vector<InputDevice>& get_devices() const { return devices; }

    // ------------------------------------------------------------------------------------------
    // Capture API (Stage 3, tools/INPUT_BINDING_DESIGN.md A.3.5) - used only by the matrix editor
    // (Menu::redefine_joystick). While a capture is active (capture_group != -1) neither joystick
    // group updates keys[]/axes; the first qualifying event on the capturing stream is latched into
    // capture_pending instead. GAMEPAD capture listens to SDL_CONTROLLER* events only (index = GC
    // enum), WHEEL capture to SDL_JOY* events only (raw index); the other stream is ignored.
    // ------------------------------------------------------------------------------------------
    struct Captured
    {
        enum Kind { NONE, AXIS, BUTTON, HAT } kind = NONE;
        SDL_JoystickID device = -1;
        int            index = -1;
        int            hat_value = 0;
        int            delta = 0;  // AXIS only: value - rest baseline at the moment it qualified (sign gives direction)
    };
    void     begin_capture(int group);   // snapshot axis baselines, flush queued axis motion, clear latch
    void     end_capture();              // capture_group = -1
    Captured poll_capture();             // first qualifying event since begin_capture; cleared on read
    bool     control_released(const Captured& c) const; // for the wait-for-release phase

    void handle_key_up(SDL_Keysym*);
    void handle_key_down(SDL_Keysym*);
    void handle_joy_axis(SDL_JoyAxisEvent*);
    void handle_joy_down(SDL_JoyButtonEvent*);
    void handle_joy_up(SDL_JoyButtonEvent*);
    void handle_joy_hat(SDL_JoyHatEvent*);
    void handle_controller_axis(SDL_ControllerAxisEvent*);
    void handle_controller_down(SDL_ControllerButtonEvent*);
    void handle_controller_up(SDL_ControllerButtonEvent*);
    void frame_done();
    bool is_pressed(presses p);
    bool is_pressed_clear(presses p);
    bool has_pressed(presses p);

private:
    static const int CENTRE = 0x80;

    // Every opened joystick/game controller. Populated by add_joystick(); the one at
    // (index == pad_id) is flagged legacy_device and drives the Stage 2 legacy fallback
    // path (A.3.3) when bindings_active == false.
    std::vector<InputDevice> devices;

    // Configurations for keyboard and joypad
    int pad_id;
    int* pad_config;
    int* key_config;
    int* axis;
    bool* invert;

    int wheel_zone;
    int wheel_dead;

    // Capture API state (Stage 3, A.3.5). capture_group is the group the editor is listening to
    // (-1 == not capturing). capture_last_group survives end_capture() so control_released() can
    // still pick the controller-vs-joystick handle during the post-capture wait-for-release phase.
    int      capture_group = -1;
    int      capture_last_group = -1;
    Captured capture_pending;
    struct AxisBaseline { SDL_JoystickID device; int index; int16_t value; };
    std::vector<AxisBaseline> capture_baselines;
    int16_t  capture_baseline_of(SDL_JoystickID device, int index) const;

    void bind_axis(SDL_GameControllerAxis ax, int offset);
    void bind_button(SDL_GameControllerButton button, int offset);
    void handle_key(const int, const bool);
    void handle_joy(const uint8_t, const bool);
    void handle_axis(const uint8_t axis, const int16_t value);
    int scale_trigger(const int);

    // Stage 2 multi-device helpers (tools/INPUT_BINDING_DESIGN.md A.3).
    const InputDevice* find_legacy_device() const;
    bool group_active(int group) const;
    void apply_axis(const InputDevice& dev, int group, int target, int index, int16_t value, bool invert);
    void apply_target(int target, bool pressed);
    bool has_binding_for_device(const std::string& signature, int group) const;
    void materialise_default_gamepad_profile(const InputDevice& dev);
    void open_legacy_device(InputDevice& dev); // verbatim body of the old single-device open_joy()
};

extern Input input;
