/***************************************************************************
    SDL Based Input Handling.

    Populates keys array with user input.
    If porting to a non-SDL platform, you would need to replace this class.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <iostream>
#include <cstring>
#include <cstdlib> // abs
#include "sdl2/input.hpp"

#ifndef WIN32
// JJP - Includes for udev, to find gamepad haptics where not supported by SDL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libudev.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

Input input;

Input::Input(void)
{
    stick      = NULL;
    controller = NULL;
    haptic     = NULL;

    gamepad = false;
    rumble_supported = false;
}

Input::~Input(void)
{
}

void Input::init(int pad_id, int* key_config, int* pad_config, int analog, int* axis, bool* invert, int* analog_settings)
{
//    std::cout << "Setting up key mappings." << std::endl; 
    this->pad_id      = pad_id;
    this->key_config  = key_config;
    this->pad_config  = pad_config;
    this->analog      = analog;
    this->axis        = axis;
    this->invert      = invert;
    this->wheel_zone  = analog_settings[0];
    this->wheel_dead  = analog_settings[1];
    motor_limits[0] = motor_limits[1] = motor_limits[2] = 0;
}

void Input::open_joy()
{
    gamepad = SDL_NumJoysticks() > pad_id;
    if (gamepad)
    {
        stick = SDL_JoystickOpen(pad_id);

        // If this is a recognized Game Controller, set up buttons and attempt to configure rumble support
        if (SDL_IsGameController(pad_id))
        {
            std::cout << "Game controller detected";
            controller = SDL_GameControllerOpen(pad_id);

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

#ifdef WIN32
            std::cout << " without SDL rumble support." << std::endl;
#else
            // Linux only - Check for rumble support
            if (SDL_GameControllerHasRumble(controller)) {
                std::cout << " with SDL rumble support." << std::endl;
            }
            else {
                std::cout << " without SDL rumble support." << std::endl;

                // JJP - try and find if rumble is supported via /dev/hidraw
                // First, get the vendor ID
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller);
                Uint16 vendor = 0;
                Uint16 product = 0;
                if (!joy) {
                    fprintf(stderr, "SDL_GameControllerGetJoystick Error: %s\n", SDL_GetError());
                }
                else {
                    // Get the vendor ID and product ID.
                    vendor = SDL_JoystickGetVendor(joy);
                    product = SDL_JoystickGetProduct(joy);
                    // printf("Vendor ID: 0x%04x\n", vendor);
                    // printf("Product ID: 0x%04x\n", product);
                }

                // Next, try and find the device via udev
                // Create a new udev context.
                struct udev* udev = udev_new();
                if (!udev) {
                    fprintf(stderr, "Cannot create udev context\n");
                }
                else {
                    // Create an enumeration object and scan for hidraw devices.
                    struct udev_enumerate* enumerate = udev_enumerate_new(udev);
                    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
                    udev_enumerate_scan_devices(enumerate);

                    // Get a list of all matching devices.
                    struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
                    struct udev_list_entry* entry;

                    // Iterate over each hidraw device.
                    udev_list_entry_foreach(entry, devices) {
                        // Note - exits on first succesful open
                        const char* sysPath = udev_list_entry_get_name(entry);
                        struct udev_device* dev = udev_device_new_from_syspath(udev, sysPath);

                        // Get the device node (for example: /dev/hidraw0)
                        const char* devNode = udev_device_get_devnode(dev);

                        // Walk up the device tree to find the USB device.
                        struct udev_device* usb_dev = udev_device_get_parent_with_subsystem_devtype(
                            dev, "usb", "usb_device");

                        if (usb_dev) {
                            const char* vendor_str = udev_device_get_sysattr_value(usb_dev, "idVendor");
                            const char* product_str = udev_device_get_sysattr_value(usb_dev, "idProduct");

                            Uint16 vendor_udev = (Uint16)strtol(vendor_str, NULL, 16);
                            Uint16 product_udev = (Uint16)strtol(product_str, NULL, 16);

                            if (vendor_str && product_str) {
                                //printf("Found hidraw device %s: vendor=%s, product=%s\n",
                                //    devNode, vendor_str, product_str);
                                // Check if these match joystick/wheel IDs.
                                if ((vendor == vendor_udev) && (product == product_udev)) {
                                    // Device matches; devNode (e.g. /dev/hidrawX) can be used to control rumble
                                    hidraw_device = open(devNode, O_RDWR); // O_WRONLY);
                                    if (hidraw_device < 0)
                                        fprintf(stderr, "Rumbler detected but could not open device\n");
                                    else {
                                        printf("Successfully opened rumble device at %s\n", devNode);
                                        rumble_supported = true;
                                        udev_device_unref(dev);
                                        break;
                                    }
                                }
                            }
                        } // if (usb_dev)
                        udev_device_unref(dev);
                    } // udev_list_entry_foreach
                    // Clean up.
                    udev_enumerate_unref(enumerate);
                    udev_unref(udev);
                } // if (!udev)/else
            } // if (SDL_GameControllerHasRumble(controller))/else
#endif
        } // if (SDL_IsGameController(pad_id))
        haptic = SDL_HapticOpen(pad_id);
        if (haptic)
        {
            rumble_supported = false;

            if (SDL_HapticRumbleSupported(haptic))
                rumble_supported = SDL_HapticRumbleInit(haptic) != -1;
        }
    }

//    if (SDL_JoystickNumButtons(controller)) {
//      std::cout << "Joystick buttons detected: " << SDL_JoystickNumButtons() << std::endl;
//    }
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

void Input::close_joy()
{
    if (controller != NULL)
    {
        SDL_GameControllerClose(controller);
        controller = NULL;
    }

    if (stick != NULL)
    {
        SDL_JoystickClose(stick);
        stick = NULL;
    }

    if (haptic != NULL)
    {
        SDL_HapticClose(haptic);
        haptic = NULL;
        rumble_supported = false;
    }

    gamepad = false;
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
    }
}

void Input::handle_joy_axis(SDL_JoyAxisEvent* evt)
{
    if (controller != NULL) return;
    handle_axis(evt->axis, evt->value);
}

void Input::handle_controller_axis(SDL_ControllerAxisEvent* evt)
{
    handle_axis(evt->axis, evt->value);
}

void Input::handle_axis(const uint8_t ax, const int16_t value)
{
    // Analog Controls
    if (analog)
    {
        int workingv = value;
//        if (ax!=0) std::cout << "ax: " << (int)ax << " value " << value << std::endl; // JJP
        store_last_axis(ax, value);

        // Steering
        // OutRun requires values between 0x48 and 0xb8. // JJP - 0x40 to 0xC0?
        if (ax == axis[0])
        {
            // JJP - convert Thrustmaster wheel input, -32768..+32767, to 0..128
            // first multiple the reading by the adjustment percentage
            int adjusted = value;
            if (wheel_zone)
              adjusted = adjusted / (100 - wheel_zone); // multiply up the input value, note input still signed
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

//            std::cout << "wheel zone : " << wheel_zone << " : " << std::hex << " : " << (int) adjusted << std::endl;
            a_wheel = adjusted;
        }
        // Accelerator [Single Axis]
        else if (ax == axis[1])
        {
            a_accel = scale_trigger(invert[1] ? -workingv : workingv);
            if (a_accel > 0xff) a_accel = 0xff;
//            std::cout << "throttle zone : " << value << ", processed: " << std::hex << " : " << (int) a_accel << std::endl;
        }

        // Brake [Single Axis]
        else if (ax == axis[2])
        {
            a_brake = scale_trigger(invert[2] ? -workingv : workingv);
            if (a_brake > 0xff) a_brake = 0xff;
//            std::cout << "brake zone : " << value << ", processed: " << std::hex << " : " << (int) a_brake << std::endl;
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
// ------------------------------------------------------------------------------------------------

int Input::scale_trigger(const int value)
{
    if (controller != NULL)
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
    if (controller != NULL) return;
    // Latch joystick button presses for redefines
    joy_button = evt->button;
//    std::cout << "Joystick button pressed event: Button " << joy_button << std::endl;
    handle_joy(evt->button, true);
}

void Input::handle_joy_up(SDL_JoyButtonEvent* evt)
{
    if (controller != NULL) return;
    handle_joy(evt->button, false);
}

void Input::handle_controller_down(SDL_ControllerButtonEvent* evt)
{
    joy_button = evt->button;
//    std::cout << "SDL Controller button pressed event: Button " << joy_button << std::endl;
    handle_joy(evt->button, true);
}

void Input::handle_controller_up(SDL_ControllerButtonEvent* evt)
{
    handle_joy(evt->button, false);
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
    if (controller != NULL) return;

    keys[UP] = evt->value == SDL_HAT_UP;
    keys[DOWN] = evt->value == SDL_HAT_DOWN;
    keys[LEFT] = evt->value == SDL_HAT_LEFT;
    keys[RIGHT] = evt->value == SDL_HAT_RIGHT;
}

void Input::set_rumble(bool enable, float strength, int mode)
{
#ifndef WIN32
    if (hidraw_device >= 0) {
        // takes precidence over SDL native support

        // Prepare a 3-byte report:
        //   Byte 0: Command type (0x00 = both motors, 0x01 for high-frequency only)
        //   Byte 1: Intensity motor 1 - 0x0* (off) to 0xF* (max)
        //   Byte 2: Intensity motor 2 - 0x0* (off) to 0xF* (max)
        // When command = 0x00, byte 1 = low frequency motor and byte 2 is high frequency motor.
        // When command = 0x01, byte 1 = high frequency motor and byte 2 is ignored.

        uint8_t report[3] = { 0 };

        if (mode == 0) {
            // original code controlled rumble effect e.g. crash sequence
            if (enable) {
                report[0] = 0x01;
                report[1] = 0xF0; // on
                report[2] = 0x00;
            }
            else {
                report[0] = 0x00;
                report[1] = 0x00; // off
                report[2] = 0x00;
            }
        }
        else {
            // enhancement - pulsing effect when skidding on road
            if (enable) {
                report[0] = 0x00;
                report[1] = 0xA0;
                report[2] = 0xA0;
            }
            else {
                report[0] = 0x00;
                report[1] = 0x00; // off
                report[2] = 0x00;
            }
        }

        // Write the report to the hidraw device. Ignore any errors; will be updated next frame anyway
        size_t bytesWritten = write(hidraw_device, report, sizeof(report));
    }
    else 
#endif
    {
        if (haptic == NULL || !rumble_supported || strength == 0) return;

        if (enable)
            SDL_HapticRumblePlay(haptic, strength, 1000 / 30);
        else
            SDL_HapticRumbleStop(haptic);
    }
}
