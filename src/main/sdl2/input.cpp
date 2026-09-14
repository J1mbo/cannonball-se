/***************************************************************************
    SDL Based Input Handling.

    Populates keys array with user input.
    If porting to a non-SDL platform, you would need to replace this class.

    Copyright Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
    - Input binding matrix: multi-device runtime (device scan/hot-plug, per-device
      resolved binding cache, group gating, WHEEL-only zone/dead steering curve,
      derived analog, default GameController profile materialisation). The legacy
      single-device path (handle_axis/handle_joy/store_last_axis/get_axis_config/
      scale_trigger) is kept verbatim for the device at controls.pad_id when
      controls.bindings_active == false; see tools/INPUT_BINDING_DESIGN.md Stage 2 (A.3).
    - Bug fix: wheel-zone maths corrected from divide to multiply-up (both in the
      new apply_axis() and in the legacy handle_axis() below) - see
      tools/INPUT_BINDING_DESIGN.md S0.4.
    - Rumble subsystem (Stage 4a): set_rumble(), the udev/hidraw discovery and the
      SDL_Haptic setup formerly here have moved to sdl2/rumble.cpp and
      sdl2/rumble_backend_sdl.cpp; Input no longer contains any rumble/haptic
      code. See tools/INPUT_BINDING_DESIGN.md Deliverable B.
***************************************************************************/

#include <iostream>
#include <cstring>
#include <cstdlib> // abs
#include "sdl2/input.hpp"
#include "main.hpp" // cannonball::state / STATE_GAME (group gating, A.3.2)

#if defined(__has_include)
#  if __has_include("directx/ffeedback.hpp")
#    include "directx/ffeedback.hpp"
#  else
     namespace forcefeedback { bool init(int,int,int); int set(int,int); void close(); bool is_supported(); }
#  endif
#else
  namespace forcefeedback { bool init(int,int,int); int set(int,int); void close(); bool is_supported(); }
#endif

#include "frontend/config.hpp"

Input input;

namespace
{
    // A.1.2: device signature = GUID + "|A<axes>|B<buttons>|H<hats>".
    // The "|A...|B...|H..." suffix is used on its own for the VID/PID fallback match, so that a
    // GUID that drifts across an SDL upgrade (CRC bytes on Linux) doesn't orphan a binding as long
    // as the vendor/product and axis/button/hat counts still agree.
    std::string abh_suffix(const std::string& signature)
    {
        auto pos = signature.find('|');
        return pos == std::string::npos ? signature : signature.substr(pos);
    }

    bool device_matches_binding(const InputDevice& dev, const device_binding_t& b)
    {
        if (!b.device.empty() && b.device == dev.signature)
            return true;

        if (b.vid != 0 && b.vid == dev.vid && b.pid == dev.pid &&
            abh_suffix(b.device) == abh_suffix(dev.signature))
            return true;

        return false;
    }
}

Input::Input(void)
{
    gamepad = false;

    // Rumble/haptic detection has moved to the rumble subsystem (Stage 4a); this field is no
    // longer set by Input and always reads false. Kept only because menu.cpp's legacy
    // menu_controls_gp still references it (that submenu is retired in Stage 3, not this one).
    rumble_supported = false;
}

Input::~Input(void)
{
}

void Input::init(int pad_id, int* key_config, int* pad_config, int analog, int* axis, bool* invert, int* analog_settings)
{
    this->pad_id      = pad_id;
    this->key_config  = key_config;
    this->pad_config  = pad_config;
    this->analog      = analog;
    this->axis        = axis;
    this->invert      = invert;
    this->wheel_zone  = analog_settings[0];
    this->wheel_dead  = analog_settings[1];
    motor_limits[0] = motor_limits[1] = motor_limits[2] = 0;

    // Input binding matrix migration (tools/INPUT_BINDING_DESIGN.md A.2.2): an absent
    // <input_mode> (config.cpp loads it as -1) is migrated once, here, at startup.
    // Legacy configs (no <bindings>) default to GAMEPAD; configs that already carry
    // <bindings> but no <input_mode> infer WHEEL if any wheel-group cell exists.
    if (config.controls.input_mode < 0)
    {
        bool any_wheel = false;
        for (const auto& b : config.controls.device_bindings)
        {
            if (b.group == device_binding_t::GROUP_WHEEL)
            {
                any_wheel = true;
                break;
            }
        }
        config.controls.input_mode = any_wheel ? device_binding_t::GROUP_WHEEL : device_binding_t::GROUP_GAMEPAD;
    }
}

// ------------------------------------------------------------------------------------------------
// Device scan / hot-plug (Stage 2, tools/INPUT_BINDING_DESIGN.md A.3.1)
// ------------------------------------------------------------------------------------------------

void Input::open_joy()
{
    // Full rescan: opens every joystick currently attached (add_joystick de-dups on instance id).
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++)
        add_joystick(i);
}

const InputDevice* Input::find_device(SDL_JoystickID id) const
{
    for (const auto& d : devices)
    {
        if (d.instance_id == id)
            return &d;
    }
    return nullptr;
}

const InputDevice* Input::find_legacy_device() const
{
    for (const auto& d : devices)
    {
        if (d.legacy_device)
            return &d;
    }
    return nullptr;
}

void Input::add_joystick(int device_index)
{
    SDL_JoystickID prospective_id = SDL_JoystickGetDeviceInstanceID(device_index);
    if (prospective_id < 0)
        return;

    // SDL re-delivers SDL_JOYDEVICEADDED for already-attached devices on the first poll.
    if (find_device(prospective_id) != nullptr)
        return;

    SDL_Joystick* joystick = SDL_JoystickOpen(device_index);
    if (!joystick)
    {
        std::cerr << "Failed to open joystick " << device_index << ": " << SDL_GetError() << std::endl;
        return;
    }

    InputDevice dev;
    dev.joystick    = joystick;
    dev.instance_id = SDL_JoystickInstanceID(joystick);

    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    char guid_buf[33] = {};
    SDL_JoystickGetGUIDString(guid, guid_buf, sizeof(guid_buf));
    dev.guid_string = guid_buf;

    dev.axes    = SDL_JoystickNumAxes(joystick);
    dev.buttons = SDL_JoystickNumButtons(joystick);
    dev.hats    = SDL_JoystickNumHats(joystick);
    dev.signature = dev.guid_string + "|A" + std::to_string(dev.axes)
                                     + "|B" + std::to_string(dev.buttons)
                                     + "|H" + std::to_string(dev.hats);

    const char* name = SDL_JoystickName(joystick);
    dev.name = name ? name : "Unknown Device";

    dev.vid = SDL_JoystickGetVendor(joystick);
    dev.pid = SDL_JoystickGetProduct(joystick);

    dev.legacy_device = (device_index == pad_id);

    if (SDL_IsGameController(device_index))
        dev.controller = SDL_GameControllerOpen(device_index);

    devices.push_back(dev);

    // Legacy-path parity (A.3.3): only the device at controls.pad_id gets the original
    // open_joy() default-binding/haptic/hidraw setup, exactly as it did before Stage 2.
    if (devices.back().legacy_device)
        open_legacy_device(devices.back());

    rebuild_binding_cache();
}

void Input::remove_joystick(SDL_JoystickID id)
{
    for (auto it = devices.begin(); it != devices.end(); ++it)
    {
        if (it->instance_id != id)
            continue;

        bool was_legacy = it->legacy_device;

        if (it->controller != nullptr)
        {
            SDL_GameControllerClose(it->controller);
            it->controller = nullptr;
        }
        if (it->joystick != nullptr)
        {
            SDL_JoystickClose(it->joystick);
            it->joystick = nullptr;
        }

        devices.erase(it);

        if (was_legacy)
        {
            // The legacy device is gone: tear down the wheel-haptic force-feedback state it
            // owned, exactly as the original close_joy() did for the single device it used to
            // track. (Rumble's own SDL_Haptic handle, if any, lives in the rumble subsystem now -
            // Stage 4a - and is torn down via rumble::on_devices_changed(), called from main.cpp.)
            if (forcefeedback::is_supported())
                forcefeedback::close();
        }
        break;
    }

    rebuild_binding_cache();
}

void Input::close_joy()
{
    for (auto& dev : devices)
    {
        if (dev.controller != nullptr)
        {
            SDL_GameControllerClose(dev.controller);
            dev.controller = nullptr;
        }
        if (dev.joystick != nullptr)
        {
            SDL_JoystickClose(dev.joystick);
            dev.joystick = nullptr;
        }
    }
    devices.clear();

    // Close kernel/DI force feedback backend if active
    if (forcefeedback::is_supported())
        forcefeedback::close();

    gamepad = false;
}

// ------------------------------------------------------------------------------------------------
// Legacy single-device setup (A.3.3) - verbatim body of the pre-Stage-2 Input::open_joy(),
// scoped to whichever device is flagged legacy_device (index == pad_id).
// ------------------------------------------------------------------------------------------------
void Input::open_legacy_device(InputDevice& dev)
{
    // If this is a recognized Game Controller, set up the default digital/analog bindings for
    // the legacy single-device wizard (A.3.3). Rumble/haptic support detection (SDL rumble probe,
    // SDL_Haptic, hidraw) has moved to the rumble subsystem - see sdl2/rumble.cpp and
    // sdl2/rumble_backend_sdl.cpp (Stage 4a, tools/INPUT_BINDING_DESIGN.md Deliverable B) - Input no longer
    // probes for or opens any rumble/haptic handle itself.
    if (dev.controller != nullptr)
    {
        std::cout << "Game controller detected" << std::endl;

        bind_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);                // Analog: Default Steering Axis
        bind_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1);         // Analog: Default Accelerate Axis
        bind_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 2);          // Analog: Default Brake Axis
        bind_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);    // Digital Controls. Map 'Right Shoulder' to Accelerate
        bind_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);     // Digital Controls. Map 'Left Shoulder' to Brake
        bind_button(SDL_CONTROLLER_BUTTON_A, 2);                // Digital Controls. Map 'A' to Gear 1
        bind_button(SDL_CONTROLLER_BUTTON_B, 3);                // Digital Controls. Map 'B' to Gear 2 (has to be enabled)
        bind_button(SDL_CONTROLLER_BUTTON_START, 4);            // Digital Controls. Map 'START'
        bind_button(SDL_CONTROLLER_BUTTON_Y, 5);                // Digital Controls. Map 'Y' to Coin
        bind_button(SDL_CONTROLLER_BUTTON_BACK, 6);             // Digital Controls. Map 'Back' to Menu Button
        bind_button(SDL_CONTROLLER_BUTTON_X, 7);                // Digital Controls. Map 'X' to Change View
        bind_button(SDL_CONTROLLER_BUTTON_DPAD_UP, 8);          // Digital Controls. Map D-Pad
        bind_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 9);
        bind_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 10);
        bind_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 11);
    }

    reset_axis_config();
    wheel = a_wheel = CENTRE;
}

void Input::bind_axis(SDL_GameControllerAxis ax, int offset)
{
    if (axis[offset] == -1) axis[offset] = ax;
}

void Input::bind_button(SDL_GameControllerButton button, int offset)
{
    if (pad_config[offset] == -1) pad_config[offset] = button;
}

// ------------------------------------------------------------------------------------------------
// Binding cache (Stage 2, tools/INPUT_BINDING_DESIGN.md A.3.1/A.3.4). Rebuilt after any hot-plug, config
// edit or input-mode change; per-event handling below only ever scans the resulting resolved[].
// ------------------------------------------------------------------------------------------------

bool Input::has_binding_for_device(const std::string& signature, int group) const
{
    for (const auto& b : config.controls.device_bindings)
    {
        if (b.group == group && b.device == signature)
            return true;
    }
    return false;
}

void Input::materialise_default_gamepad_profile(const InputDevice& dev)
{
    // Same defaults as the legacy single-device table above (open_legacy_device), so a
    // freshly-attached, never-configured GameController works out of the box (A.2.2).
    struct Cell { int target, type, index; };
    static const Cell defaults[] = {
        { device_binding_t::TARGET_STEER, device_binding_t::TYPE_AXIS,   SDL_CONTROLLER_AXIS_LEFTX },
        { device_binding_t::TARGET_ACCEL, device_binding_t::TYPE_AXIS,   SDL_CONTROLLER_AXIS_TRIGGERRIGHT },
        { device_binding_t::TARGET_BRAKE, device_binding_t::TYPE_AXIS,   SDL_CONTROLLER_AXIS_TRIGGERLEFT },
        { device_binding_t::TARGET_GEAR1, device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_A },
        { device_binding_t::TARGET_GEAR2, device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_B },
        { device_binding_t::TARGET_START, device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_START },
        { device_binding_t::TARGET_COIN,  device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_Y },
        { device_binding_t::TARGET_MENU,  device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_BACK },
        { device_binding_t::TARGET_VIEW,  device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_X },
        { device_binding_t::TARGET_UP,    device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_UP },
        { device_binding_t::TARGET_DOWN,  device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
        { device_binding_t::TARGET_LEFT,  device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
        { device_binding_t::TARGET_RIGHT, device_binding_t::TYPE_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    };

    for (const auto& cell : defaults)
    {
        device_binding_t b;
        b.group  = device_binding_t::GROUP_GAMEPAD;
        b.target = cell.target;
        b.type   = cell.type;
        b.index  = cell.index;
        b.device = dev.signature;
        b.vid    = dev.vid;
        b.pid    = dev.pid;
        b.name   = dev.name;
        config.set_binding(b);
    }
}

void Input::rebuild_binding_cache()
{
    if (config.controls.bindings_active)
    {
        // A.2.2: a recognised GameController with zero GAMEPAD cells for its signature gets the
        // SE default profile synthesised in memory so it works immediately; config.save()
        // persists it the next time settings are saved.
        for (auto& dev : devices)
        {
            if (dev.controller != nullptr &&
                !has_binding_for_device(dev.signature, device_binding_t::GROUP_GAMEPAD))
            {
                materialise_default_gamepad_profile(dev);
            }
        }
    }

    for (auto& dev : devices)
    {
        dev.resolved[device_binding_t::GROUP_GAMEPAD].clear();
        dev.resolved[device_binding_t::GROUP_WHEEL].clear();
        dev.nav_bound[device_binding_t::GROUP_GAMEPAD] = false;
        dev.nav_bound[device_binding_t::GROUP_WHEEL]   = false;

        if (!config.controls.bindings_active)
            continue; // legacy path: resolved[] stays empty and unused

        for (const auto& b : config.controls.device_bindings)
        {
            if (!device_matches_binding(dev, b))
                continue;

            InputDevice::Resolved r;
            r.type     = static_cast<uint8_t>(b.type);
            r.index    = static_cast<uint8_t>(b.index);
            r.hat_mask = static_cast<uint8_t>(b.value);
            r.target   = static_cast<uint8_t>(b.target);
            dev.resolved[b.group].push_back(r);

            if (b.target == device_binding_t::TARGET_UP    ||
                b.target == device_binding_t::TARGET_DOWN  ||
                b.target == device_binding_t::TARGET_LEFT  ||
                b.target == device_binding_t::TARGET_RIGHT)
            {
                dev.nav_bound[b.group] = true;
            }
        }
    }

    if (config.controls.bindings_active)
    {
        gamepad = !devices.empty();

        // Derived analog (A.3.4) for the active input_mode group only.
        int active_group = config.controls.input_mode;
        bool steer_bound = false, accel_bound = false, brake_bound = false;
        for (const auto& b : config.controls.device_bindings)
        {
            if (b.group != active_group) continue;
            if (b.target == device_binding_t::TARGET_STEER && b.type == device_binding_t::TYPE_AXIS)
                steer_bound = true;
            else if (b.target == device_binding_t::TARGET_ACCEL && b.type == device_binding_t::TYPE_AXIS)
                accel_bound = true;
            else if (b.target == device_binding_t::TARGET_BRAKE && b.type == device_binding_t::TYPE_AXIS)
                brake_bound = true;
        }
        analog = steer_bound ? ((accel_bound && brake_bound) ? 1 : 2) : 0;
    }
    else
    {
        // Legacy path: preserve exactly what open_joy()/the ANALOG menu entry computed before
        // Stage 2 - gamepad reflects presence of the single pad_id device, analog is whatever
        // config.controls.analog (the manual ANALOG menu toggle) holds.
        gamepad = find_legacy_device() != nullptr;
        analog  = config.controls.analog;
    }
}

void Input::reset_keys()
{
    // Clears every keys[] slot that a joystick/wheel binding can drive (not the function-key-only
    // slots PAUSE/STEP/TIMER, which are keyboard-only and unaffected by device groups or
    // input_mode). Called on game entry and on input-mode change (A.3.2) so a held control from
    // the group that just became inactive cannot leave a stuck key behind.
    keys[LEFT] = keys[RIGHT] = keys[UP] = keys[DOWN] = false;
    keys[ACCEL] = keys[BRAKE] = keys[GEAR1] = keys[GEAR2] = false;
    keys[START] = keys[COIN] = keys[MENU] = keys[VIEWPOINT] = false;
}

// ------------------------------------------------------------------------------------------------
// Group gating (A.3.2): in cannonball::STATE_GAME only the active input_mode group drives the
// car; everywhere else (menus, transitional states) both groups navigate. The keyboard is always
// live and is not gated here.
// ------------------------------------------------------------------------------------------------
bool Input::group_active(int group) const
{
    if (cannonball::state == cannonball::STATE_GAME)
        return group == config.controls.input_mode;
    return true;
}

void Input::apply_target(int target, bool pressed)
{
    switch (target)
    {
        case device_binding_t::TARGET_ACCEL: keys[ACCEL]     = pressed; break;
        case device_binding_t::TARGET_BRAKE: keys[BRAKE]     = pressed; break;
        case device_binding_t::TARGET_GEAR1: keys[GEAR1]     = pressed; break;
        case device_binding_t::TARGET_GEAR2: keys[GEAR2]     = pressed; break;
        case device_binding_t::TARGET_START: keys[START]     = pressed; break;
        case device_binding_t::TARGET_COIN:  keys[COIN]      = pressed; break;
        case device_binding_t::TARGET_MENU:  keys[MENU]      = pressed; break;
        case device_binding_t::TARGET_VIEW:  keys[VIEWPOINT] = pressed; break;
        case device_binding_t::TARGET_UP:    keys[UP]        = pressed; break;
        case device_binding_t::TARGET_DOWN:  keys[DOWN]      = pressed; break;
        case device_binding_t::TARGET_LEFT:  keys[LEFT]      = pressed; break;
        case device_binding_t::TARGET_RIGHT: keys[RIGHT]     = pressed; break;
        default: break; // TARGET_STEER is axis-only; handled by apply_axis
    }
}

// A.3.4: symmetric steering curve (reaches both 0x40 and 0xC0, 0 -> exactly 0x80); WHEEL-only
// zone/dead. ACCEL/BRAKE axis mapping + invert. `index` is the raw axis (WHEEL) or
// SDL_GameControllerAxis enum (GAMEPAD) the value came from, needed to tell a GAMEPAD trigger
// (0..32767) from every other axis (-32768..32767).
void Input::apply_axis(const InputDevice& /*dev*/, int group, int target, int index, int16_t value)
{
    if (target == device_binding_t::TARGET_STEER)
    {
        int raw = value;

        // Saturation zone is a WHEEL setting only - applying it to a GAMEPAD thumbstick would
        // make it far too sensitive (DX input.cpp:884-892). Bug fix: multiply up, not divide -
        // see tools/INPUT_BINDING_DESIGN.md S0.4 (the legacy handle_axis() below gets the same fix).
        if (group == device_binding_t::GROUP_WHEEL && wheel_zone > 0 && wheel_zone < 100)
            raw = raw * 100 / (100 - wheel_zone);

        if (raw < -32768) raw = -32768;
        else if (raw > 32767) raw = 32767;

        // Map the signed SDL range exactly onto OutRun's 0x40..0xC0 range, symmetrically.
        int adjusted = CENTRE + (raw >= 0 ? (raw * 0x40) / 32767 : (raw * 0x40) / 32768);
        if (adjusted < 0x40) adjusted = 0x40;
        else if (adjusted > 0xC0) adjusted = 0xC0;

        if (group == device_binding_t::GROUP_WHEEL && wheel_dead &&
            std::abs(CENTRE - adjusted) <= wheel_dead)
        {
            adjusted = CENTRE;
        }

        a_wheel = adjusted;
    }
    else if (target == device_binding_t::TARGET_ACCEL || target == device_binding_t::TARGET_BRAKE)
    {
        bool is_accel = target == device_binding_t::TARGET_ACCEL;
        int invert_slot = is_accel ? 1 : 2;
        int working = invert[invert_slot] ? -value : value;

        bool is_trigger_axis = group == device_binding_t::GROUP_GAMEPAD &&
            (index == SDL_CONTROLLER_AXIS_TRIGGERLEFT || index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

        int scaled = is_trigger_axis ? (working / 0x80) : ((working + 0x8000) / 0x100);
        if (scaled < 0) scaled = 0;
        else if (scaled > 0xFF) scaled = 0xFF;

        if (is_accel) a_accel = scaled;
        else          a_brake = scaled;
    }
}

// Detect whether a key press change has occurred
bool Input::has_pressed(presses p)
{
    return keys[p] && !keys_old[p];
}

// Detect whether key is still pressed
bool Input::is_pressed(presses p)
{
    return keys[p];
}

// Detect whether pressed and clear the press
bool Input::is_pressed_clear(presses p)
{
    bool pressed = keys[p];
    keys[p] = false;
    return pressed;
}

// Denote that a frame has been done by copying key presses into previous array
void Input::frame_done()
{
    memcpy(&keys_old, &keys, sizeof(keys));
}

void Input::handle_key_down(SDL_Keysym* keysym)
{
    key_press = keysym->sym;
    handle_key(key_press, true);
}

void Input::handle_key_up(SDL_Keysym* keysym)
{
    handle_key(keysym->sym, false);
}
void Input::handle_key(const int key, const bool is_pressed)
{
    // Redefinable Key Input
    if (key == key_config[0])  keys[UP] = is_pressed;
    if (key == key_config[1])  keys[DOWN] = is_pressed;
    if (key == key_config[2])  keys[LEFT] = is_pressed;
    if (key == key_config[3])  keys[RIGHT] = is_pressed;
    if (key == key_config[4])  keys[ACCEL] = is_pressed;
    if (key == key_config[5])  keys[BRAKE] = is_pressed;
    if (key == key_config[6])  keys[GEAR1] = is_pressed;
    if (key == key_config[7])  keys[GEAR2] = is_pressed;
    if (key == key_config[8])  keys[START] = is_pressed;
    if (key == key_config[9])  keys[COIN] = is_pressed;
    if (key == key_config[10]) keys[MENU] = is_pressed;
    if (key == key_config[11]) keys[VIEWPOINT] = is_pressed;

    // Function keys are not redefinable
    switch (key)
    {
        case SDLK_F1:
            keys[PAUSE] = is_pressed;
            break;

        case SDLK_F2:
            keys[STEP] = is_pressed;
            break;

        case SDLK_F3:
            keys[TIMER] = is_pressed;
            break;

        case SDLK_F5:
            keys[MENU] = is_pressed;
            break;

        case SDLK_F7:
            // JJP - switches between sprite rendering (original/hi-res)
            //
            // Hi-res sprites are only valid while the hi-res game engine is
            // active. With hiresprites set, OSprites::do_sprite() takes its
            // zoom from ZOOM_LOOKUP_HIRES, whose values are pre-doubled on the
            // assumption that hwsprites::render() halves them again - which it
            // only does when config.video.hires is set. Toggling this on in
            // lo-res therefore feeds the renderer unhalved zoom values of up
            // to 0x7E0, roughly double anything the lo-res path is designed
            // for, and also applies the hi-res sub-pixel x offset that was
            // computed for a doubled canvas.
            //
            // The menu already refuses this combination (menu.cpp, "Set game
            // engine to hires first") and clears the flag when hi-res is
            // switched off; the hotkey must agree with it.
            if (!is_pressed) break;
            if (!config.video.hires) break;
            config.video.hiresprites ^= 1;
            break;

        case SDLK_F8:
            // JJP - switches between all video processing off and Pi3 defaults (full shader etc)
            // Note - doesn't alter widescreen of hires setting
            if (!is_pressed) break;
            if (config.videoRestartRequired) break;
            if (config.video.blargg!=0) {
                // filter is currently enabled - so reset everything
                config.video.hires_next     = config.video.hires;
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
            } else {
                // filter is not currently enable - so enable everything
                config.video.hires_next     = config.video.hires;
                config.video.shader_mode    = video_settings_t::SHADER_FULL;
                config.video.shadow_mask    = video_settings_t::SHADOW_MASK_SHADER;
                config.video.maskDim        = 75;
                config.video.maskBoost      = 125;
                config.video.scanlines      = 0;
                config.video.crt_shape      = 1;
                config.video.vignette       = 30;
                config.video.noise          = 6;
                config.video.warpX          = 2;
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
            }
            // flag to restart the video subsystem
            config.videoRestartRequired = true;
            break;
        case SDLK_F9:
            // JJP - enables/disables the mask if it's enabled
            if (!is_pressed) break;
            if (config.video.shadow_mask==2) {
                static int manual_off = false;
                if (manual_off) {
                    // set to default values
                    config.video.maskDim        = 70;
                    config.video.maskBoost      = 130;
                } else {
                    // set both values to 100 which effectively disables it
                    config.video.maskDim        = 100;
                    config.video.maskBoost      = 100;
                }
                manual_off = !manual_off; // toggle state
            }
            break;
    }
}

// ------------------------------------------------------------------------------------------------
// Event routing (A.3.2). Each handler first resolves the SDL device id to its InputDevice; if
// bindings_active is false only the device flagged legacy_device drives anything, via the
// verbatim legacy functions below (A.3.3). If bindings_active is true, the event is matched
// against that device's resolved[] cache for the relevant group, gated by group_active().
// ------------------------------------------------------------------------------------------------

void Input::handle_joy_axis(SDL_JoyAxisEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device || dev->controller != nullptr) return;
        handle_axis(evt->axis, evt->value);
        return;
    }

    if (!group_active(device_binding_t::GROUP_WHEEL)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_WHEEL])
    {
        if (r.type != device_binding_t::TYPE_AXIS || r.index != evt->axis) continue;
        if (r.target == device_binding_t::TARGET_STEER ||
            r.target == device_binding_t::TARGET_ACCEL ||
            r.target == device_binding_t::TARGET_BRAKE)
        {
            apply_axis(*dev, device_binding_t::GROUP_WHEEL, r.target, r.index, evt->value);
        }
    }
}

void Input::handle_controller_axis(SDL_ControllerAxisEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device) return;
        handle_axis(evt->axis, evt->value);
        return;
    }

    if (!group_active(device_binding_t::GROUP_GAMEPAD)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_GAMEPAD])
    {
        if (r.type != device_binding_t::TYPE_AXIS || r.index != evt->axis) continue;
        if (r.target == device_binding_t::TARGET_STEER ||
            r.target == device_binding_t::TARGET_ACCEL ||
            r.target == device_binding_t::TARGET_BRAKE)
        {
            apply_axis(*dev, device_binding_t::GROUP_GAMEPAD, r.target, r.index, evt->value);
        }
    }
}

void Input::handle_axis(const uint8_t ax, const int16_t value)
{
    // Analog Controls
    if (analog)
    {
        int workingv = value;
        store_last_axis(ax, value);

        // Steering
        // OutRun requires values between 0x48 and 0xb8. // JJP - 0x40 to 0xC0?
        if (ax == axis[0])
        {
            // JJP - convert Thrustmaster wheel input, -32768..+32767, to 0..128
            // first multiple the reading by the adjustment percentage
            int adjusted = value;
            if (wheel_zone)
              adjusted = adjusted * 100 / (100 - wheel_zone); // multiply up the input value, note input still signed
            adjusted = ((adjusted + 0x8000) / 0x200); // reduce to 0..128

            adjusted += 0x40; // Centre; 0x40 is hard left

            // clip at game limits
            if (adjusted < 0x40) adjusted = 0x40;
            else if (adjusted > 0xC0) adjusted = 0xC0;

            // Remove Dead Zone
            if (wheel_dead)
            {
                if (std::abs(CENTRE - adjusted) <= wheel_dead)
                    adjusted = CENTRE;
            }

            a_wheel = adjusted;
        }
        // Accelerator [Single Axis]
        else if (ax == axis[1])
        {
            a_accel = scale_trigger(invert[1] ? -workingv : workingv);
            if (a_accel > 0xff) a_accel = 0xff;
        }

        // Brake [Single Axis]
        else if (ax == axis[2])
        {
            a_brake = scale_trigger(invert[2] ? -workingv : workingv);
            if (a_brake > 0xff) a_brake = 0xff;
        }

        // Motor Banking (Moving Cabient Only with SmartyPi)
        else if (ax == axis[3])
        {
            //a_motor = (workingv + 0x8000) / 0x100;
            a_motor = scale_trigger(workingv);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Scale the trigger value to be between 0 and 0xFF
//
// This is based on whether this is an SDL Controller or Joystick.
// Controllers: Trigger axis values range from 0 to SDL_JOYSTICK_AXIS_MAX (32767)
// Joysticks:   Undefined, but usually between -32768 to 32767
//
// Legacy-path only (A.3.3): scoped by handle_axis()'s caller to the single legacy device, so
// "is this a controller" is still a valid proxy for "is this axis a 0..32767 trigger" exactly as
// before Stage 2.
// ------------------------------------------------------------------------------------------------

int Input::scale_trigger(const int value)
{
    const InputDevice* legacy = find_legacy_device();
    if (legacy != nullptr && legacy->controller != nullptr)
        return value / 0x80;
    else
        return (value + 0x8000) / 0x100;
}

// ------------------------------------------------------------------------------------------------
// Store the last analog axis to be pressed and depressed beyond the cap value for config purposes
// ------------------------------------------------------------------------------------------------
void Input::store_last_axis(const uint8_t ax, const int16_t value)
{
    const static int CAP = SDL_JOYSTICK_AXIS_MAX / 4;

    if (std::abs(value) > CAP)
        axis_last = ax;
    else if (ax == axis_last)
    {
        axis_last = -1;
        axis_counter = 0;
    }

    if (ax == axis_last)
    {
        if (value > CAP*2 && axis_counter == 0) axis_counter = 1;
        if (value < CAP*2 && axis_counter == 1) axis_counter = 2;
        if (axis_counter == 2)                  axis_config = ax; // Store the axis
    }
}

int Input::get_axis_config()
{
    if (axis_counter == 2)
    {
        int value = axis_config;
        reset_axis_config();
        return value;
    }
    return -1;
}

void Input::reset_axis_config()
{
    axis_config = -1;
    axis_last = -1;
    axis_counter = 0;
}

void Input::handle_joy_down(SDL_JoyButtonEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    // Latch for the legacy REDEFINE GAMEPAD wizard (Stage 3 removes it). Suppressed for
    // GameController-recognised devices, which report the same physical press again via
    // SDL_CONTROLLERBUTTONDOWN - exactly as before Stage 2.
    if (dev->controller == nullptr)
        joy_button = evt->button;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device || dev->controller != nullptr) return;
        handle_joy(evt->button, true);
        return;
    }

    if (!group_active(device_binding_t::GROUP_WHEEL)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_WHEEL])
    {
        if (r.type == device_binding_t::TYPE_BUTTON && r.index == evt->button)
            apply_target(r.target, true);
    }
}

void Input::handle_joy_up(SDL_JoyButtonEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device || dev->controller != nullptr) return;
        handle_joy(evt->button, false);
        return;
    }

    if (!group_active(device_binding_t::GROUP_WHEEL)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_WHEEL])
    {
        if (r.type == device_binding_t::TYPE_BUTTON && r.index == evt->button)
            apply_target(r.target, false);
    }
}

void Input::handle_controller_down(SDL_ControllerButtonEvent* evt)
{
    joy_button = evt->button;

    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device) return;
        handle_joy(evt->button, true);
        return;
    }

    if (!group_active(device_binding_t::GROUP_GAMEPAD)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_GAMEPAD])
    {
        if (r.type == device_binding_t::TYPE_BUTTON && r.index == evt->button)
            apply_target(r.target, true);
    }

    // Navigation default (A.3.2): a device with no UP/DOWN/LEFT/RIGHT cell in this group still
    // needs to reach the menu, so the D-pad falls back to SE's original mapping.
    if (!dev->nav_bound[device_binding_t::GROUP_GAMEPAD])
    {
        switch (evt->button)
        {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    keys[UP]    = true; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  keys[DOWN]  = true; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  keys[LEFT]  = true; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: keys[RIGHT] = true; break;
            default: break;
        }
    }
}

void Input::handle_controller_up(SDL_ControllerButtonEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device) return;
        handle_joy(evt->button, false);
        return;
    }

    if (!group_active(device_binding_t::GROUP_GAMEPAD)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_GAMEPAD])
    {
        if (r.type == device_binding_t::TYPE_BUTTON && r.index == evt->button)
            apply_target(r.target, false);
    }

    if (!dev->nav_bound[device_binding_t::GROUP_GAMEPAD])
    {
        switch (evt->button)
        {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    keys[UP]    = false; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  keys[DOWN]  = false; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  keys[LEFT]  = false; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: keys[RIGHT] = false; break;
            default: break;
        }
    }
}

void Input::handle_joy(const uint8_t button, const bool is_pressed)
{
    if (button == pad_config[0])   keys[ACCEL]     = is_pressed;
    if (button == pad_config[1])   keys[BRAKE]     = is_pressed;
    if (button == pad_config[2])   keys[GEAR1]     = is_pressed;
    if (button == pad_config[3])   keys[GEAR2]     = is_pressed;
    if (button == pad_config[4])   keys[START]     = is_pressed;
    if (button == pad_config[5])   keys[COIN]      = is_pressed;
    if (button == pad_config[6])   keys[MENU]      = is_pressed;
    if (button == pad_config[7])   keys[VIEWPOINT] = is_pressed;
    if (button == pad_config[8])   keys[UP]        = is_pressed;
    if (button == pad_config[9])   keys[DOWN]      = is_pressed;
    if (button == pad_config[10])  keys[LEFT]      = is_pressed;
    if (button == pad_config[11])  keys[RIGHT]     = is_pressed;

    // Limit Input Switches
    if (button == pad_config[12])  motor_limits[SW_LEFT]   = is_pressed;
    if (button == pad_config[13])  motor_limits[SW_CENTRE] = is_pressed;
    if (button == pad_config[14])  motor_limits[SW_RIGHT]  = is_pressed;
}

void Input::handle_joy_hat(SDL_JoyHatEvent* evt)
{
    const InputDevice* dev = find_device(evt->which);
    if (!dev) return;

    if (!config.controls.bindings_active)
    {
        if (!dev->legacy_device || dev->controller != nullptr) return;

        keys[UP] = evt->value == SDL_HAT_UP;
        keys[DOWN] = evt->value == SDL_HAT_DOWN;
        keys[LEFT] = evt->value == SDL_HAT_LEFT;
        keys[RIGHT] = evt->value == SDL_HAT_RIGHT;
        return;
    }

    if (!group_active(device_binding_t::GROUP_WHEEL)) return;

    for (const auto& r : dev->resolved[device_binding_t::GROUP_WHEEL])
    {
        if (r.type != device_binding_t::TYPE_HAT || r.index != evt->hat) continue;
        bool pressed = r.hat_mask != SDL_HAT_CENTERED && (evt->value & r.hat_mask) != 0;
        apply_target(r.target, pressed);
    }

    // Navigation default (A.3.2): a device with no UP/DOWN/LEFT/RIGHT cell in this group still
    // needs to reach the menu, so hat 0 falls back to SE's original mapping.
    if (evt->hat == 0 && !dev->nav_bound[device_binding_t::GROUP_WHEEL])
    {
        keys[UP] = evt->value == SDL_HAT_UP;
        keys[DOWN] = evt->value == SDL_HAT_DOWN;
        keys[LEFT] = evt->value == SDL_HAT_LEFT;
        keys[RIGHT] = evt->value == SDL_HAT_RIGHT;
    }
}

// set_rumble() removed (Stage 4a, tools/INPUT_BINDING_DESIGN.md Deliverable B): its hard-coded hidraw
// report bytes, the SDL_Haptic fallback and the forcefeedback:: adapter now live behind the
// RumbleBackend interface in sdl2/rumble_backend_sdl.cpp / sdl2/rumble_backend_ffb.cpp, selected
// and mixed by sdl2/rumble.cpp. Callers use the rumble:: API (rumble::set_level/event/tick) - see
// main.cpp's tick() rumble call site.
