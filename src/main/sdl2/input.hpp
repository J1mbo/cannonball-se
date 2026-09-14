/***************************************************************************
    SDL Based Input Handling.

    Populates keys array with user input.
    If porting to a non-SDL platform, you would need to replace this class.

    Copyright Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
    - Input binding matrix: multi-device runtime (InputDevice, hot-plug,
      per-device resolved binding cache). See tools/INPUT_BINDING_DESIGN.md Stage 2.
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

    // Latch last joystick button press for redefines
    int16_t joy_button;

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
    void reset_axis_config();
    int get_axis_config();
    void set_rumble(bool, float strength = 1.0f, int mode = 0);

private:
    static const int CENTRE = 0x80;

    // Every opened joystick/game controller. Populated by add_joystick(); the one at
    // (index == pad_id) is flagged legacy_device and drives the Stage 2 legacy fallback
    // path (A.3.3) when bindings_active == false.
    std::vector<InputDevice> devices;

    // Force-feedback / rumble state that is not yet per-device (Stage 4 moves this into the
    // rumble subsystem and removes it from Input). Kept exactly as before for now.
    SDL_Haptic* haptic;
    int hidraw_device = -1; // used for gamepad haptics where not supported by SDL

    // Configurations for keyboard and joypad
    int pad_id;
    int* pad_config;
    int* key_config;
    int* axis;
    bool* invert;

    int wheel_zone;
    int wheel_dead;

    // Last axis used
    int axis_last , axis_counter, axis_config;

    void bind_axis(SDL_GameControllerAxis ax, int offset);
    void bind_button(SDL_GameControllerButton button, int offset);
    void handle_key(const int, const bool);
    void handle_joy(const uint8_t, const bool);
    void handle_axis(const uint8_t axis, const int16_t value);
    void store_last_axis(const uint8_t axis, const int16_t value);
    int scale_trigger(const int);

    // Stage 2 multi-device helpers (tools/INPUT_BINDING_DESIGN.md A.3).
    const InputDevice* find_legacy_device() const;
    bool group_active(int group) const;
    void apply_axis(const InputDevice& dev, int group, int target, int index, int16_t value);
    void apply_target(int target, bool pressed);
    bool has_binding_for_device(const std::string& signature, int group) const;
    void materialise_default_gamepad_profile(const InputDevice& dev);
    void open_legacy_device(InputDevice& dev); // verbatim body of the old single-device open_joy()
};

extern Input input;
