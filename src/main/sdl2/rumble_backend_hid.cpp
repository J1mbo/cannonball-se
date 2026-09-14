/***************************************************************************
    Rumble subsystem - raw-HID backend (Stage 4b).

    Implements HID_REPORT (tools/INPUT_BINDING_DESIGN.md B.2.3, B.3, B.5, B.7.4): a
    small built-in profile table plus one user-editable custom profile,
    rendered to bytes and written to the target device over Linux hidraw
    (the udev/sysfs VID/PID discovery moved here, generalised, from the
    pre-Stage-4a Input::open_legacy_device() - see git commit 4f5b454) or
    SDL_hid_open/SDL_hid_write when SDL_VERSION_ATLEAST(2,0,18) (Windows
    always; newer Linux too).

    The built-in "thrustmaster3" profile is byte-for-byte what the removed
    Input::set_rumble() wrote: mode 0 (motor/one-shot) -> {0x01,0xF0,0x00}
    on / {0x00,0x00,0x00} off; mode 1 (skid) -> {0x00,0xA0,0xA0} pulse /
    {0x00,0x00,0x00} off. RumbleFrame carries no effect tag (same
    constraint the FFB backend has, rumble_backend_ffb.cpp), so PRESETS
    mode approximates the effect from the mixed frame's own shape: SKID is
    the only Stage-4a/4b effect with low==0 && high>0 (kShapes[SKID],
    rumble.cpp), so that shape means "pulse"; any other non-zero frame
    means "on".

    B.7.4 (never write raw bytes to an unknown device) is enforced here at
    open() time and again for the editor's transient hid_raw_write(): a
    built-in profile must match the device's vendor (and product, if the
    profile pins one); the custom profile must match both fields exactly.

    Edge-triggered writes, the one-write-per-tick cap and the 3-strikes
    session disable all live in rumble.cpp (write_frame()) - this file only
    renders bytes and reports success/failure honestly.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL.h>

#include "sdl2/rumble_backend.hpp"
#include "sdl2/input.hpp"       // InputDevice
#include "frontend/config.hpp"  // Config / controls.rumble_hid_profile / rumble_hid_custom

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <libudev.h>
#endif

namespace rumble
{
    namespace
    {
        // ------------------------------------------------------------------------------------
        // B.3.1 profile model - internal only; never leaves this file. The menu-facing
        // HidProfileEdit (sdl2/rumble.hpp) is a separate, smaller, edit-friendly type - see
        // hid_catalogue_snapshot()/render_hid_edit_action() for the translation between the two.
        // ------------------------------------------------------------------------------------
        struct HidRumbleProfile
        {
            const char* id   = "";
            const char* name = "";
            uint16_t    vid = 0, pid = 0;   // pid 0 = any product of this vendor (built-ins only)
            uint8_t     report_len = 0;     // 1..64, excluding any report-id prefix
            int16_t     report_id  = -1;    // -1 = unnumbered report; 0..255 = numbered report
            enum Mode  { PRESETS, INTENSITY } mode = PRESETS;
            uint8_t     on[64]   = { 0 };
            uint8_t     off[64]  = { 0 };
            uint8_t     pulse[64]= { 0 };
            uint8_t     tmpl[64] = { 0 };
            int8_t      pos_low = -1, pos_high = -1;
            enum Scale { NIBBLE_HI, BYTE } scale = NIBBLE_HI;
            uint16_t    keepalive_ms = 0;
        };

        // Built-in table (B.3.1). thrustmaster3 is byte-identical to the bytes the removed
        // Input::set_rumble() wrote (see file header); thrustmaster3s is the same device/report
        // shape but demonstrates the INTENSITY (scaled) mode for a future/alternate wheel.
        HidRumbleProfile make_thrustmaster3()
        {
            HidRumbleProfile p;
            p.id = "thrustmaster3";
            p.name = "THRUSTMASTER 3-BYTE";
            p.vid = 0x044F; p.pid = 0; // any Thrustmaster product
            p.report_len = 3;
            p.report_id  = -1;
            p.mode = HidRumbleProfile::PRESETS;
            p.on[0]    = 0x01; p.on[1]    = 0xF0; p.on[2]    = 0x00;
            p.off[0]   = 0x00; p.off[1]   = 0x00; p.off[2]   = 0x00;
            p.pulse[0] = 0x00; p.pulse[1] = 0xA0; p.pulse[2] = 0xA0;
            return p;
        }

        HidRumbleProfile make_thrustmaster3s()
        {
            HidRumbleProfile p;
            p.id = "thrustmaster3s";
            p.name = "THRUSTMASTER 3-BYTE SCALED";
            p.vid = 0x044F; p.pid = 0;
            p.report_len = 3;
            p.report_id  = -1;
            p.mode = HidRumbleProfile::INTENSITY;
            p.tmpl[0] = 0x00; p.tmpl[1] = 0x00; p.tmpl[2] = 0x00;
            p.pos_low  = 1;
            p.pos_high = 2;
            p.scale = HidRumbleProfile::NIBBLE_HI;
            return p;
        }

        const HidRumbleProfile kBuiltins[] = { make_thrustmaster3(), make_thrustmaster3s() };
        const int kBuiltinCount = static_cast<int>(sizeof(kBuiltins) / sizeof(kBuiltins[0]));

        // B.7.7: set on the most recent EACCES from either the persistent backend or the editor's
        // transient hid_raw_write(); surfaced to the menu via rumble.cpp's active_backend_name().
        bool g_permission_denied = false;

        uint8_t scale_value(uint16_t v, HidRumbleProfile::Scale s)
        {
            return s == HidRumbleProfile::NIBBLE_HI
                ? static_cast<uint8_t>((v >> 12) << 4)   // 0x00, 0x10, ..., 0xF0
                : static_cast<uint8_t>(v >> 8);           // 0x00..0xFF
        }

        // B.3.1 "Effect -> profile action" for the persistent backend's per-tick play(RumbleFrame).
        int render_frame(const HidRumbleProfile& p, const RumbleFrame& frame, uint8_t* out)
        {
            if (p.report_len < 1 || p.report_len > 64)
                return 0;

            if (p.mode == HidRumbleProfile::PRESETS)
            {
                const uint8_t* src =
                    (frame.low == 0 && frame.high == 0) ? p.off  :
                    (frame.low == 0 && frame.high  > 0) ? p.pulse : p.on;
                std::memcpy(out, src, p.report_len);
            }
            else // INTENSITY
            {
                if ((p.pos_low  >= 0 && p.pos_low  >= p.report_len) ||
                    (p.pos_high >= 0 && p.pos_high >= p.report_len))
                    return 0;
                std::memcpy(out, p.tmpl, p.report_len);
                if (p.pos_low  >= 0) out[p.pos_low]  = scale_value(frame.low,  p.scale);
                if (p.pos_high >= 0) out[p.pos_high] = scale_value(frame.high, p.scale);
            }
            return p.report_len;
        }

        // Prefix rule (B.3.1/B.5): hidraw needs the report-id byte only for a numbered report;
        // SDL_hid_write always needs a leading id byte (0 if the device uses unnumbered reports).
        int prefix_report(int16_t report_id, bool always_prefix, const uint8_t* payload, int len, uint8_t* out)
        {
            if (report_id >= 0)
            {
                out[0] = static_cast<uint8_t>(report_id);
                std::memcpy(out + 1, payload, len);
                return len + 1;
            }
            if (always_prefix)
            {
                out[0] = 0;
                std::memcpy(out + 1, payload, len);
                return len + 1;
            }
            std::memcpy(out, payload, len);
            return len;
        }

#ifndef _WIN32
        // Linux hidraw discovery - moved here, generalised to any vid/pid (rather than tied to
        // whichever joystick opened it), from the pre-Stage-4a Input::open_legacy_device() (git
        // commit 4f5b454, input.cpp ~330-387). Opened O_NONBLOCK|O_CLOEXEC per B.7.3 (the original
        // code used O_RDWR alone).
        int find_and_open_hidraw(uint16_t vid, uint16_t pid)
        {
            struct udev* udev_ctx = udev_new();
            if (!udev_ctx)
                return -1;

            int fd = -1;
            struct udev_enumerate* enumerate = udev_enumerate_new(udev_ctx);
            udev_enumerate_add_match_subsystem(enumerate, "hidraw");
            udev_enumerate_scan_devices(enumerate);

            struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
            struct udev_list_entry* entry;

            udev_list_entry_foreach(entry, devices)
            {
                const char* sys_path = udev_list_entry_get_name(entry);
                struct udev_device* dev = udev_device_new_from_syspath(udev_ctx, sys_path);
                if (!dev)
                    continue;

                const char* dev_node = udev_device_get_devnode(dev);
                struct udev_device* usb_dev =
                    udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");

                if (usb_dev && dev_node)
                {
                    const char* vendor_str  = udev_device_get_sysattr_value(usb_dev, "idVendor");
                    const char* product_str = udev_device_get_sysattr_value(usb_dev, "idProduct");

                    if (vendor_str && product_str)
                    {
                        uint16_t v = static_cast<uint16_t>(strtol(vendor_str, nullptr, 16));
                        uint16_t p = static_cast<uint16_t>(strtol(product_str, nullptr, 16));

                        if (v == vid && p == pid)
                        {
                            fd = open(dev_node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
                            if (fd >= 0)
                            {
                                udev_device_unref(dev);
                                break;
                            }
                            if (errno == EACCES)
                                g_permission_denied = true;
                        }
                    }
                }
                udev_device_unref(dev);
            }

            udev_enumerate_unref(enumerate);
            udev_unref(udev_ctx);
            return fd;
        }
#endif

        // B.7.4 arming rule: a built-in profile matches on vendor (and product too, if the profile
        // pins one); the custom profile must match the target device exactly.
        bool builtin_matches(const HidRumbleProfile& p, uint16_t vid, uint16_t pid)
        {
            return p.vid == vid && (p.pid == 0 || p.pid == pid);
        }

        bool custom_matches(uint16_t vid, uint16_t pid)
        {
            const rumble_hid_custom_t& c = config.controls.rumble_hid_custom;
            return c.valid && c.vid == vid && c.pid == pid;
        }

        // Materialises config.controls.rumble_hid_custom into a HidRumbleProfile so render_frame()/
        // open() can treat CUSTOM identically to a built-in table entry.
        HidRumbleProfile custom_profile()
        {
            HidRumbleProfile p;
            const rumble_hid_custom_t& c = config.controls.rumble_hid_custom;
            p.id = "custom";
            p.name = "CUSTOM";
            p.vid = c.vid; p.pid = c.pid;
            p.report_len = static_cast<uint8_t>(std::min(64, std::max(1, c.len)));
            p.report_id  = static_cast<int16_t>(c.report_id);
            p.mode = (c.mode == 1) ? HidRumbleProfile::INTENSITY : HidRumbleProfile::PRESETS;
            std::memcpy(p.on,    c.on,    64);
            std::memcpy(p.off,   c.off,   64);
            std::memcpy(p.pulse, c.pulse, 64);
            std::memcpy(p.tmpl,  c.tmpl,  64);
            p.pos_low  = static_cast<int8_t>(c.low_byte);
            p.pos_high = static_cast<int8_t>(c.high_byte);
            p.scale = (c.scale == 1) ? HidRumbleProfile::BYTE : HidRumbleProfile::NIBBLE_HI;
            p.keepalive_ms = static_cast<uint16_t>(std::max(0, c.keepalive_ms));
            return p;
        }

        const HidRumbleProfile& builtin_at(int index) { return kBuiltins[index]; }

        // Resolves which profile a device should use: the user's configured choice if it matches
        // this device (B.7.4), else the first built-in that matches by vendor (B.3.3 auto-suggest).
        // Never silently arms CUSTOM unless it is itself the configured/matching choice - a
        // different device must not receive somebody else's custom byte image.
        int resolve_effective_index(uint16_t vid, uint16_t pid)
        {
            int configured = hid_catalogue_index_for_id(config.controls.rumble_hid_profile);
            if (configured >= 0 && hid_catalogue_vendor_match(configured, vid, pid))
                return configured;

            for (int i = 0; i < kBuiltinCount; i++)
                if (builtin_matches(kBuiltins[i], vid, pid))
                    return i;

            return -1;
        }

        // ------------------------------------------------------------------------------------
        // HidReportBackend (B.2.3/B.5): profile bytes -> hidraw (Linux) or SDL_hid_write (Windows
        // always; Linux too when SDL >= 2.0.18). Both the guarded (SDL_hid) and unguarded (hidraw)
        // paths are always compiled so this file builds against any SDL2 >= the project floor -
        // only which path is *reachable* at runtime/compile-time differs.
        // ------------------------------------------------------------------------------------
        class HidReportBackend : public RumbleBackend
        {
        public:
            bool open(const InputDevice& dev) override
            {
                int idx = resolve_effective_index(dev.vid, dev.pid);
                if (idx < 0)
                    return false; // B.7.4: no built-in or custom profile matches this device

                profile_ = (idx < kBuiltinCount) ? builtin_at(idx) : custom_profile();

                // Defensive re-validation of the resolved profile shape (a malformed custom block
                // is normally caught by config.cpp, which clears its .valid flag - this guards
                // against any other future caller of custom_profile()).
                if (profile_.report_len < 1 || profile_.report_len > 64)
                    return false;
                if (profile_.mode == HidRumbleProfile::INTENSITY &&
                    ((profile_.pos_low  >= 0 && profile_.pos_low  >= profile_.report_len) ||
                     (profile_.pos_high >= 0 && profile_.pos_high >= profile_.report_len)))
                    return false;

                name_ = std::string("HID: ") + profile_.name;
                g_permission_denied = false;

                bool opened = false;
#ifndef _WIN32
                hidraw_fd_ = find_and_open_hidraw(dev.vid, dev.pid);
                opened = (hidraw_fd_ >= 0);
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (!opened)
                {
                    hid_dev_ = SDL_hid_open(dev.vid, dev.pid, nullptr);
                    opened = (hid_dev_ != nullptr);
                }
#endif
#else // _WIN32 - no hidraw; SDL_hid is the only path (and only exists on SDL >= 2.0.18)
#if SDL_VERSION_ATLEAST(2, 0, 18)
                hid_dev_ = SDL_hid_open(dev.vid, dev.pid, nullptr);
                opened = (hid_dev_ != nullptr);
#endif
#endif
                return opened;
            }

            bool play(const RumbleFrame& frame) override
            {
                uint8_t payload[64];
                int len = render_frame(profile_, frame, payload);
                if (len <= 0)
                    return false;
                return write_payload(payload, len);
            }

            void stop() override
            {
                uint8_t payload[64];
                int len = render_frame(profile_, RumbleFrame{}, payload); // low=high=duration=0 -> OFF
                if (len > 0)
                    write_payload(payload, len);
            }

            void close() override
            {
#ifndef _WIN32
                if (hidraw_fd_ >= 0) { ::close(hidraw_fd_); hidraw_fd_ = -1; }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (hid_dev_) { SDL_hid_close(hid_dev_); hid_dev_ = nullptr; }
#endif
            }

            const char* name() const override { return name_.c_str(); }
            uint16_t keepalive_ms() const override { return profile_.keepalive_ms; }

        private:
            bool write_payload(const uint8_t* payload, int len)
            {
                uint8_t wire[65];
#ifndef _WIN32
                if (hidraw_fd_ >= 0)
                {
                    int wlen = prefix_report(profile_.report_id, false, payload, len, wire);
                    ssize_t n = ::write(hidraw_fd_, wire, static_cast<size_t>(wlen));
                    if (n == wlen)
                        return true;
                    if (errno == EACCES)
                        g_permission_denied = true;
                    return false;
                }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (hid_dev_)
                {
                    int wlen = prefix_report(profile_.report_id, true, payload, len, wire);
                    int n = SDL_hid_write(hid_dev_, wire, static_cast<size_t>(wlen));
                    return n == wlen;
                }
#endif
                return false;
            }

            HidRumbleProfile profile_;
            std::string      name_ = "HID";
#ifndef _WIN32
            int hidraw_fd_ = -1;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_hid_device* hid_dev_ = nullptr;
#endif
        };
    } // anonymous namespace

    RumbleBackend* create_hid_backend() { return new HidReportBackend(); }

    int hid_catalogue_count() { return kBuiltinCount + 1; } // + CUSTOM

    const char* hid_catalogue_id(int index)
    {
        if (index < 0 || index >= hid_catalogue_count()) return "";
        return index < kBuiltinCount ? kBuiltins[index].id : "custom";
    }

    const char* hid_catalogue_name(int index)
    {
        if (index < 0 || index >= hid_catalogue_count()) return "";
        return index < kBuiltinCount ? kBuiltins[index].name : "CUSTOM";
    }

    bool hid_catalogue_vendor_match(int index, uint16_t vid, uint16_t pid)
    {
        if (index < 0 || index >= hid_catalogue_count()) return false;
        return index < kBuiltinCount ? builtin_matches(kBuiltins[index], vid, pid)
                                      : custom_matches(vid, pid);
    }

    int hid_catalogue_index_for_id(const std::string& id)
    {
        for (int i = 0; i < kBuiltinCount; i++)
            if (id == kBuiltins[i].id)
                return i;
        if (id == "custom")
            return kBuiltinCount;
        return -1;
    }

    HidProfileEdit hid_catalogue_snapshot(int index)
    {
        HidProfileEdit e;
        if (index < 0 || index >= hid_catalogue_count())
            return e;

        const HidRumbleProfile p = (index < kBuiltinCount) ? builtin_at(index) : custom_profile();

        e.name = p.name;
        e.vid = p.vid; e.pid = p.pid;
        e.report_len = p.report_len;
        e.report_id  = p.report_id;
        e.mode = (p.mode == HidRumbleProfile::INTENSITY) ? HID_INTENSITY : HID_PRESETS;

        int n = std::min<int>(p.report_len, HidProfileEdit::MAX_LEN);
        std::memcpy(e.on,    p.on,    n);
        std::memcpy(e.off,   p.off,   n);
        std::memcpy(e.pulse, p.pulse, n);
        std::memcpy(e.tmpl,  p.tmpl,  n);
        e.pos_low  = p.pos_low;
        e.pos_high = p.pos_high;
        e.scale = (p.scale == HidRumbleProfile::BYTE) ? HID_BYTE : HID_NIBBLE_HI;
        e.keepalive_ms = p.keepalive_ms;
        return e;
    }

    // B.6.2 TEST ON/OFF/PULSE against the editor's scratch profile - independent of render_frame()
    // above because there is no RumbleFrame here, only an explicit requested action.
    int render_hid_edit_action(const HidProfileEdit& p, HidTestAction action, uint8_t* out)
    {
        int len = std::min(p.report_len, HidProfileEdit::MAX_LEN);
        if (len < 1)
            return 0;

        if (p.mode == HID_PRESETS)
        {
            const uint8_t* src =
                action == HID_TEST_OFF   ? p.off :
                action == HID_TEST_PULSE ? p.pulse : p.on;
            std::memcpy(out, src, len);
        }
        else // HID_INTENSITY
        {
            if ((p.pos_low  >= 0 && p.pos_low  >= len) ||
                (p.pos_high >= 0 && p.pos_high >= len))
                return 0;
            std::memcpy(out, p.tmpl, len);
            uint16_t level = (action == HID_TEST_OFF) ? 0 : 0xFFFF; // OFF vs full-scale ON/PULSE
            uint8_t v = (p.scale == HID_NIBBLE_HI) ? static_cast<uint8_t>((level >> 12) << 4)
                                                     : static_cast<uint8_t>(level >> 8);
            if (p.pos_low  >= 0) out[p.pos_low]  = v;
            if (p.pos_high >= 0) out[p.pos_high] = v;
        }
        return len;
    }

    HidWriteResult hid_raw_write(uint16_t vid, uint16_t pid, int16_t report_id, const uint8_t* payload, int len)
    {
        if (len <= 0 || len > HidProfileEdit::MAX_LEN)
            return HidWriteResult::FAILED;

        uint8_t wire[HidProfileEdit::MAX_LEN + 1];
        g_permission_denied = false;

#ifndef _WIN32
        int fd = find_and_open_hidraw(vid, pid);
        if (fd >= 0)
        {
            int wlen = prefix_report(report_id, false, payload, len, wire);
            ssize_t n = ::write(fd, wire, static_cast<size_t>(wlen));
            ::close(fd);
            return (n == wlen) ? HidWriteResult::OK : HidWriteResult::FAILED;
        }
        if (g_permission_denied)
            return HidWriteResult::PERMISSION_DENIED;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
        SDL_hid_device* h = SDL_hid_open(vid, pid, nullptr);
        if (h)
        {
            int wlen = prefix_report(report_id, true, payload, len, wire);
            int n = SDL_hid_write(h, wire, static_cast<size_t>(wlen));
            SDL_hid_close(h);
            return (n == wlen) ? HidWriteResult::OK : HidWriteResult::FAILED;
        }
#endif
        return HidWriteResult::FAILED;
    }

    bool hid_permission_denied() { return g_permission_denied; }
}
