/***************************************************************************
    Rumble / force-feedback subsystem - mixer, backend selection and safety.

    See sdl2/rumble.hpp for the public API and tools/INPUT_BINDING_DESIGN.md
    Deliverable B for the design this implements. Stage 4a implemented the
    CORE + SDL_CONTROLLER/SDL_HAPTIC/FFB backends, with HID_REPORT behaving
    like OFF (no backend behind it yet). Stage 4b wires in the profile-driven
    HID_REPORT backend (rumble_backend_hid.cpp) and adds the menu-facing
    hid_* wrappers at the bottom of this file (B.3, B.6).

    Safety (B.7), all enforced here rather than in any backend:
      1. Edge-triggered writes - a frame identical to the last one actually
         sent is not re-sent, except for a backend that reports a non-zero
         keepalive_ms() (Stage 4b, B.3.1: some HID rumble reports time out
         and switch off if not periodically repeated).
      2. Hard rate cap - at most one backend write per tick() call.
      3. 3 consecutive backend failures disable it for the session; AUTO
         falls through to the next candidate, an explicit choice becomes OFF.
      4. Always-OFF guarantees - stop_all() on backend/device changes and on
         shutdown(); a stale-tick guard forces OFF if tick() is not called
         for over 100 ms (a stalled main loop).
      5. The TEST path shares this same limiter and cannot be re-triggered
         while a test is already running; the Stage 4b editor's raw HID
         TEST ON/PULSE actions share it too (see hid_test() below).

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>

#include <SDL.h>

#include "sdl2/rumble.hpp"
#include "sdl2/rumble_backend.hpp"
#include "sdl2/input.hpp"       // InputDevice, extern Input input, Input::get_devices()
#include "frontend/config.hpp"  // extern Config config, device_binding_t

// Stage 4c (tools/INPUT_BINDING_DESIGN.md B.2.2): the optional richer effects poll these engine globals
// directly, exactly as the DX build does (gamepad_rumble_state.hpp), so no engine file is touched.
// Header-only reads of existing externs.
#include "engine/ocrash.hpp"    // extern OCrash ocrash;  int16_t crash_counter
#include "engine/oinputs.hpp"   // extern OInputs oinputs; bool gear; int16_t acc_adjust
#include "engine/outrun.hpp"    // extern Outrun outrun;   int8_t game_state; GS_START1..GS_INGAME

namespace rumble
{
    namespace
    {
        // ------------------------------------------------------------------------------------
        // NullBackend: the OFF backend. Always "succeeds" so tick()'s failure counter never
        // fires for it, and its name() is what the menu shows when nothing is driving rumble.
        // ------------------------------------------------------------------------------------
        class NullBackend : public RumbleBackend
        {
        public:
            bool open(const InputDevice&) override { return true; }
            bool play(const RumbleFrame&) override { return true; }
            void stop() override {}
            void close() override {}
            const char* name() const override { return "NONE"; }
        };

        // ------------------------------------------------------------------------------------
        // Per-effect shape (B.2.2): the mixed frame for an active effect is
        //   low  = low_pct  * intensity01 * master
        //   high = high_pct * intensity01 * master
        // GEAR (unused until Stage 4c) linearly ramps its shape from the *_start values to the
        // *_end values over its own duration; every other effect has a fixed shape.
        // ------------------------------------------------------------------------------------
        struct EffectShape
        {
            float    low_pct_start, high_pct_start;
            float    low_pct_end,   high_pct_end;
            uint16_t duration_ms;
            bool     ramps;
        };

        // Indexed by Effect (MOTOR, SKID, CRASH, GEAR, START_REV, TEST).
        const EffectShape kShapes[EFFECT_COUNT] = {
            /* MOTOR     */ { 1.00f, 0.60f, 1.00f, 0.60f,  45, false },
            /* SKID      */ { 0.00f, 1.00f, 0.00f, 1.00f,  45, false },
            /* CRASH     */ { 1.00f, 1.00f, 1.00f, 1.00f, 160, false },
            /* GEAR      */ { 0.60f, 0.25f, 0.20f, 0.40f,  95, true  },
            /* START_REV */ { 0.50f, 1.00f, 0.50f, 1.00f,  45, false },
            /* TEST      */ { 1.00f, 1.00f, 1.00f, 1.00f, 500, false },
        };

        // CRASH > GEAR > START_REV > MOTOR > SKID (B.2.2). TEST is a deliberate, exclusive menu
        // action and outranks every gameplay effect.
        const Effect kPriority[] = { TEST, CRASH, GEAR, START_REV, MOTOR, SKID };

        const int kFailureLimit = 3;       // B.7.3: 3 consecutive failures disables a backend
        const uint32_t kStaleGuardMs = 100; // B.7.5: force OFF if tick() stalls this long
        const uint16_t kTestDurationCapMs = 500;

        float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

        bool same_frame(const RumbleFrame& a, const RumbleFrame& b)
        {
            return a.low == b.low && a.high == b.high && a.duration_ms == b.duration_ms;
        }

        // ------------------------------------------------------------------------------------
        // Module state
        // ------------------------------------------------------------------------------------
        struct EffectSlot
        {
            float    intensity = 0.f;  // last requested 0..1
            bool     active    = false;
            bool     continuous = false; // set_level() (caller refreshes each tick) vs event() (self-expiring)
            uint32_t start_ms  = 0;
            uint32_t until_ms  = 0;    // event()/test() only; 0 = no pending expiry
        };

        EffectSlot slots[EFFECT_COUNT];

        float    master = 1.0f;
        RumbleBackend* active = nullptr;
        Backend  active_kind = OFF;
        const InputDevice* target = nullptr;

        std::set<Backend> disabled_this_session;

        RumbleFrame last_sent{ 0xFFFF, 0xFFFF, 0xFFFF }; // sentinel: cannot match any real frame
        int      fail_count = 0;
        uint32_t last_tick_ms = 0;
        uint32_t last_write_ms = 0; // Stage 4b: last time write_frame() actually wrote a frame

        bool     test_running = false;
        uint32_t test_deadline_ms = 0;

        // Stage 4b (B.6.2): when the ADVANCED HID SETUP editor's TEST ON/PULSE writes a report
        // directly (hid_test(), independent of the active backend/mixer), this remembers the OFF
        // bytes to send when the shared `test_running` window (above) expires in tick().
        bool     raw_test_pending = false;
        uint16_t raw_test_vid = 0, raw_test_pid = 0;
        int16_t  raw_test_report_id = -1;
        uint8_t  raw_test_off_bytes[HidProfileEdit::MAX_LEN] = { 0 };
        int      raw_test_off_len = 0;

        // ------------------------------------------------------------------------------------
        // Target-device selection (B.2.3): the device holding the active group's STEER cell;
        // else the first device with any cell in the active group; else the legacy device;
        // else none. `controls.rumble_device`, when set, overrides all of this by signature.
        // ------------------------------------------------------------------------------------
        const InputDevice* select_target_device()
        {
            const auto& devices = input.get_devices();

            if (!config.controls.rumble_device.empty())
            {
                for (const auto& dev : devices)
                    if (dev.signature == config.controls.rumble_device)
                        return &dev;
                // Override doesn't match anything currently attached - fall through rather than
                // refuse rumble outright, so an unplugged override device doesn't go silent.
            }

            if (config.controls.bindings_active)
            {
                int active_group = config.controls.input_mode;

                const device_binding_t* steer = config.find_binding(active_group, device_binding_t::TARGET_STEER);
                if (steer)
                {
                    for (const auto& dev : devices)
                        if (dev.signature == steer->device)
                            return &dev;
                }

                for (const auto& dev : devices)
                {
                    for (const auto& b : config.controls.device_bindings)
                    {
                        if (b.group == active_group && b.device == dev.signature)
                            return &dev;
                    }
                }
            }

            for (const auto& dev : devices)
                if (dev.legacy_device)
                    return &dev;

            return nullptr;
        }

        RumbleBackend* try_open(Backend which, const InputDevice* dev)
        {
            if (!dev)
                return nullptr;

            RumbleBackend* b = nullptr;
            switch (which)
            {
                case SDL_CONTROLLER: b = create_sdl_controller_backend(); break;
                case SDL_HAPTIC:     b = create_sdl_haptic_backend();     break;
                case HID_REPORT:     b = create_hid_backend();            break;
                case FFB:            b = create_ffb_backend();            break;
                default: return nullptr; // AUTO/OFF are not directly openable here
            }
            if (!b)
                return nullptr;

            if (b->open(*dev))
                return b;

            b->close();
            delete b;
            return nullptr;
        }

        void teardown_active()
        {
            if (active)
            {
                active->stop();
                active->close();
                delete active;
                active = nullptr;
            }
            active_kind = OFF;
        }

        // ------------------------------------------------------------------------------------
        // Stage 4c (tools/INPUT_BINDING_DESIGN.md B.2.2): optional richer effects (CRASH/GEAR/START_REV)
        // derived by polling engine globals here rather than firing them from main.cpp. This is
        // the SE equivalent of DX's gamepad_rumble_state.hpp, adapted to the slot API above.
        //
        // Edge-tracking state for the poll. `extra_baseline_valid` makes the first poll after any
        // reset a pure baseline capture (no fire), so a re-selected/hot-plugged device or a game
        // restart can never spuriously fire CRASH/GEAR on the next tick. `start_level01` is the
        // ramped START_REV intensity in 0..1 (see poll_engine_effects()).
        // ------------------------------------------------------------------------------------
        int16_t last_crash_counter = 0;
        bool    last_gear          = false;
        float   start_level01      = 0.f;
        bool    was_in_start       = false;
        bool    extra_baseline_valid = false;
        bool    was_extras_on      = false; // tracks on->off edge so START_REV can be cleared once

        void reset_extra_state()
        {
            last_crash_counter   = 0;
            last_gear            = false;
            start_level01        = 0.f;
            was_in_start         = false;
            extra_baseline_valid = false;
            // `was_extras_on` is deliberately NOT reset: the on->off clear of the continuous
            // START_REV slot is keyed on it and must survive a stop_all()/device change so a
            // level set while extras were on is always cleared once when they are turned off.
        }

        // Called at the top of tick() (tools/INPUT_BINDING_DESIGN.md B.2.2). A true no-op when extras are
        // off, except for a single clear of the continuous START_REV slot on the on->off edge so
        // toggling extras off mid-start cannot leave a stuck level.
        void poll_engine_effects()
        {
            if (!config.controls.rumble_extras)
            {
                if (was_extras_on)
                {
                    set_level(START_REV, 0.f);
                    start_level01 = 0.f;
                    was_extras_on = false;
                }
                return;
            }
            was_extras_on = true;

            const int16_t crash = ocrash.crash_counter;
            const bool    gear  = oinputs.gear;
            const int8_t  gs    = outrun.game_state;

            // First poll after any reset: capture baselines only, don't fire.
            if (!extra_baseline_valid)
            {
                last_crash_counter   = crash;
                last_gear            = gear;
                extra_baseline_valid = true;
            }

            // CRASH: rising edge from zero (see tools/INPUT_BINDING_DESIGN.md B.2.2); 160 ms one-shot.
            // Re-baselines automatically as crash_counter returns to 0, so a wrap/reset or a game
            // restart re-arms cleanly and fires only on a genuine new crash.
            if (crash > 0 && last_crash_counter <= 0)
                event(CRASH, 1.0f, kShapes[CRASH].duration_ms);
            last_crash_counter = crash;

            // GEAR: one short bump on a gear change while racing (DX :128-140); 95 ms one-shot.
            // `last_gear` is updated every poll (also outside GS_INGAME), so re-entering gameplay
            // never fires from a change that happened off the road.
            if (gs == GS_INGAME && gear != last_gear)
                event(GEAR, 1.0f, kShapes[GEAR].duration_ms);
            last_gear = gear;

            // START_REV: continuous half/level rev during the start line, scaled by the throttle
            // and ramped (DX :151-177,257-261). DX works in master units; here the ramp is kept in
            // normalised 0..1 (master cancels) and fed to set_level(), letting the mixer apply the
            // START_REV shape (low = half, high = full) and master. Full throttle targets 0.90 to
            // match DX's 90%-of-master peak; ramp-up ~0.3 s at 30 Hz (DX master/10), down ~master/6.
            const bool in_start = (gs == GS_START1 || gs == GS_START2 || gs == GS_START3);

            float target01 = 0.f;
            if (in_start)
            {
                int pedal = static_cast<int>(oinputs.acc_adjust);
                pedal = pedal < 0 ? 0 : (pedal > 0xFF ? 0xFF : pedal);
                if (pedal >= 8) // DX ignores the very bottom of pedal travel
                    target01 = (static_cast<float>(pedal) / 255.0f) * 0.90f;
            }

            const float ramp_up   = 0.10f;
            const float ramp_down = 1.0f / 6.0f;
            if (start_level01 < target01)
                start_level01 = std::min(target01, start_level01 + ramp_up);
            else if (start_level01 > target01)
                start_level01 = std::max(target01, start_level01 - ramp_down);

            if (in_start)
                set_level(START_REV, start_level01);
            else if (was_in_start)
            {
                // Leaving the start range: clear the continuous slot once so nothing sticks.
                set_level(START_REV, 0.f);
                start_level01 = 0.f;
            }
            was_in_start = in_start;
        }

        void reset_slots()
        {
            for (auto& s : slots)
                s = EffectSlot{};
            test_running = false;
            raw_test_pending = false; // Stage 4b: abandon any pending editor raw-HID auto-off
            reset_extra_state();      // Stage 4c: re-baseline the polled-effect edge state too
        }

        // Re-select the target device and backend. Always lands on a non-null `active` (NullBackend
        // for OFF). B.7.10: always stops everything first so a hot-plug/backend-change can never
        // leave a stale write in flight on a handle that is about to be torn down.
        void select_and_open()
        {
            reset_slots();
            teardown_active();
            last_sent = RumbleFrame{ 0xFFFF, 0xFFFF, 0xFFFF };
            fail_count = 0;

            target = select_target_device();

            Backend cfg = static_cast<Backend>(config.controls.rumble_backend);

            if (cfg == OFF)
            {
                active = new NullBackend();
                active->open(InputDevice{}); // NullBackend::open() ignores its argument
                active_kind = OFF;
                return;
            }

            if (cfg != AUTO)
            {
                // Explicit choice: honour it if possible, otherwise land on OFF rather than
                // silently substituting a backend the user didn't ask for (AUTO exists for that).
                if (!disabled_this_session.count(cfg))
                {
                    if (RumbleBackend* b = try_open(cfg, target))
                    {
                        active = b;
                        active_kind = cfg;
                        return;
                    }
                }
                active = new NullBackend();
                active->open(InputDevice{});
                active_kind = OFF;
                return;
            }

            // AUTO order (B.2.1): SDL_CONTROLLER -> SDL_HAPTIC -> HID_REPORT (only if a built-in or
            // custom profile matches the target's VID/PID - HidReportBackend::open() itself refuses
            // otherwise, B.7.4) -> FFB (if haptic-enabled and not a moving cabinet) -> OFF.
            static const Backend order[] = { SDL_CONTROLLER, SDL_HAPTIC, HID_REPORT, FFB };
            for (Backend b : order)
            {
                if (disabled_this_session.count(b))
                    continue;

                if (RumbleBackend* be = try_open(b, target))
                {
                    active = be;
                    active_kind = b;
                    return;
                }
            }

            active = new NullBackend();
            active->open(InputDevice{});
            active_kind = OFF;
        }

        // B.7.1/B.7.2/B.7.3: edge-triggered, at most one write per tick, 3-strikes disable. Stage 4b
        // addition: a backend that reports a non-zero keepalive_ms() (some HID rumble reports time
        // out and switch off if not periodically repeated, B.3.1) gets the identical frame re-sent
        // once that many ms have elapsed, even though nothing upstream changed.
        void write_frame(const RumbleFrame& frame)
        {
            if (!active)
                return;

            bool identical = same_frame(frame, last_sent);
            uint32_t now = SDL_GetTicks();
            bool keepalive_due = identical
                && !(frame.low == 0 && frame.high == 0) // never keepalive an OFF frame
                && active->keepalive_ms() > 0
                && (now - last_write_ms) >= active->keepalive_ms();

            if (identical && !keepalive_due)
                return;

            bool ok;
            if (frame.low == 0 && frame.high == 0)
            {
                active->stop();
                ok = true;
            }
            else
            {
                ok = active->play(frame);
            }

            last_sent = frame;
            last_write_ms = now;

            if (ok)
            {
                fail_count = 0;
                return;
            }

            if (++fail_count >= kFailureLimit)
            {
                std::cerr << "rumble: backend '" << active->name()
                          << "' failed " << kFailureLimit << " times in a row; disabling for this session." << std::endl;
                disabled_this_session.insert(active_kind);
                select_and_open(); // AUTO falls through; an explicit selection becomes OFF
            }
        }

        // B.7.5 stale-tick guard: a stalled main loop must not resume by replaying whatever the
        // mix last computed (a one-shot's clock may have jumped far past its window).
        void hard_off()
        {
            if (active)
                active->stop();
            last_sent = RumbleFrame{};
            reset_slots();
        }
    } // anonymous namespace

    void init()
    {
        set_master(config.controls.rumble);
        disabled_this_session.clear();
        select_and_open(); // no devices exist yet at this point in startup; lands on OFF until
                            // on_devices_changed() (from the first SDL_JOYDEVICEADDED) re-resolves
    }

    void shutdown()
    {
        stop_all();
        teardown_active();
    }

    void on_devices_changed()
    {
        select_and_open();
    }

    void set_master(float strength01)
    {
        master = clamp01(strength01);
    }

    Backend configured_backend()
    {
        int idx = config.controls.rumble_backend;
        if (idx < AUTO || idx > OFF)
            idx = AUTO;
        return static_cast<Backend>(idx);
    }

    void set_backend(Backend backend)
    {
        config.controls.rumble_backend = static_cast<int>(backend);
        disabled_this_session.clear(); // an explicit re-selection deserves a fresh 3-strikes count
        select_and_open();
    }

    const char* active_backend_name()
    {
        // B.7.7: if HID_REPORT was configured (directly or via AUTO) but the last hidraw/SDL_hid
        // attempt failed with EACCES, say so explicitly rather than silently reporting "NONE" -
        // the fix (a udev rule / group membership) is different from "no rumble device attached".
        if (active_kind == OFF)
        {
            Backend cfg = static_cast<Backend>(config.controls.rumble_backend);
            if ((cfg == HID_REPORT || cfg == AUTO) && hid_permission_denied())
                return "HID - PERMISSION DENIED";
        }
        return active ? active->name() : "NONE";
    }

    void set_level(Effect effect, float intensity01)
    {
        if (effect < 0 || effect >= EFFECT_COUNT)
            return;

        float v = clamp01(intensity01);
        EffectSlot& s = slots[effect];
        s.intensity  = v;
        s.continuous = true;
        s.until_ms   = 0;
        if (v > 0.f)
        {
            if (!s.active)
                s.start_ms = SDL_GetTicks();
            s.active = true;
        }
        else
        {
            s.active   = false;
            s.start_ms = 0;
        }
    }

    void event(Effect effect, float intensity01, uint16_t duration_ms)
    {
        if (effect < 0 || effect >= EFFECT_COUNT)
            return;

        float v = clamp01(intensity01);
        EffectSlot& s = slots[effect];
        s.intensity  = v;
        s.continuous = false;
        s.start_ms   = SDL_GetTicks();
        s.until_ms   = s.start_ms + duration_ms;
        s.active     = v > 0.f;
    }

    bool test(Effect effect, uint16_t duration_ms)
    {
        if (test_running)
            return false; // B.7.6: cannot retrigger while a test is running
        if (effect < 0 || effect >= EFFECT_COUNT)
            return false;
        if (!active || active_kind == OFF)
            return false;

        uint16_t ms = std::min(duration_ms, kTestDurationCapMs);
        event(effect, 1.0f, ms);
        test_running = true;
        test_deadline_ms = SDL_GetTicks() + ms;
        return true;
    }

    void stop_all()
    {
        reset_slots();
        if (active)
            active->stop();
        last_sent = RumbleFrame{};
    }

    void tick()
    {
        uint32_t now = SDL_GetTicks();

        if (last_tick_ms != 0 && (now - last_tick_ms) > kStaleGuardMs)
            hard_off(); // B.7.5: main loop stalled - do not resume with a possibly-stale mix

        last_tick_ms = now;

        // Stage 4c (tools/INPUT_BINDING_DESIGN.md B.2.2): derive the optional CRASH/GEAR/START_REV effects
        // from the engine globals before the one-shot expiry loop below. A true no-op (bar a single
        // START_REV clear on the extras on->off edge) when config.controls.rumble_extras is off.
        poll_engine_effects();

        if (test_running && now >= test_deadline_ms)
        {
            test_running = false;

            // Stage 4b (B.6.2): the ADVANCED HID SETUP editor's TEST ON/PULSE wrote raw bytes
            // directly (hid_test()), bypassing the Effect/mixer path entirely - send the matching
            // OFF report now that the shared test window has expired.
            if (raw_test_pending)
            {
                hid_raw_write(raw_test_vid, raw_test_pid, raw_test_report_id,
                               raw_test_off_bytes, raw_test_off_len);
                raw_test_pending = false;
            }
        }

        // Expire one-shot events (event()/test()) whose window has elapsed. Continuous slots
        // (set_level()) have no expiry of their own - the caller drives them to zero directly.
        for (int i = 0; i < EFFECT_COUNT; ++i)
        {
            EffectSlot& s = slots[i];
            if (!s.continuous && s.until_ms != 0 && now >= s.until_ms)
            {
                s.active   = false;
                s.intensity = 0.f;
                s.until_ms  = 0;
                s.start_ms  = 0;
            }
        }

        RumbleFrame frame{};
        Effect chosen = EFFECT_COUNT;
        for (Effect e : kPriority)
        {
            if (slots[e].active)
            {
                chosen = e;
                break;
            }
        }

        if (chosen != EFFECT_COUNT)
        {
            const EffectShape& shape = kShapes[chosen];
            float low_pct = shape.low_pct_start;
            float high_pct = shape.high_pct_start;

            if (shape.ramps && shape.duration_ms > 0)
            {
                float t = std::min(1.0f, static_cast<float>(now - slots[chosen].start_ms) / shape.duration_ms);
                low_pct  += (shape.low_pct_end  - shape.low_pct_start)  * t;
                high_pct += (shape.high_pct_end - shape.high_pct_start) * t;
            }

            float scale = slots[chosen].intensity * master;
            float low_f  = std::min(1.0f, low_pct  * scale);
            float high_f = std::min(1.0f, high_pct * scale);

            frame.low         = static_cast<uint16_t>(std::lround(low_f  * 65535.0f));
            frame.high        = static_cast<uint16_t>(std::lround(high_f * 65535.0f));
            frame.duration_ms = shape.duration_ms;
        }

        write_frame(frame);
    }

    // ============================================================================================
    // Stage 4b - raw-HID profile surface for the FEEDBACK menu and the ADVANCED HID SETUP editor
    // (tools/INPUT_BINDING_DESIGN.md B.3, B.6). The catalogue itself (built-ins + CUSTOM) lives in
    // rumble_backend_hid.cpp; everything here just forwards to it plus the module state (`target`,
    // `test_running`) already owned by this file.
    // ============================================================================================

    HidTargetInfo hid_target_info()
    {
        HidTargetInfo info;
        if (target)
        {
            info.present = true;
            info.name    = target->name;
            info.vid     = target->vid;
            info.pid     = target->pid;
        }
        return info;
    }

    int hid_profile_count() { return hid_catalogue_count(); }

    const char* hid_profile_name(int index) { return hid_catalogue_name(index); }

    bool hid_profile_matches_target(int index)
    {
        if (!target)
            return false;
        return hid_catalogue_vendor_match(index, target->vid, target->pid);
    }

    int hid_selected_profile_index()
    {
        return hid_catalogue_index_for_id(config.controls.rumble_hid_profile);
    }

    void set_hid_profile_index(int index)
    {
        if (index < 0 || index >= hid_catalogue_count())
            return;

        config.controls.rumble_hid_profile = hid_catalogue_id(index);
        disabled_this_session.erase(HID_REPORT); // a deliberate profile change deserves a fresh try
        select_and_open();                        // B.7.5: stop_all()s first, then re-resolves
    }

    HidProfileEdit hid_profile_snapshot(int index)
    {
        return hid_catalogue_snapshot(index);
    }

    void save_custom_hid_profile(const HidProfileEdit& profile)
    {
        rumble_hid_custom_t& c = config.controls.rumble_hid_custom;
        int len = std::min<int>(profile.report_len, HidProfileEdit::MAX_LEN);
        len = std::max(1, len);

        c.valid       = true;
        c.vid         = profile.vid;
        c.pid         = profile.pid;
        c.len         = len;
        c.report_id   = profile.report_id;
        c.mode        = (profile.mode == HID_INTENSITY) ? 1 : 0;
        c.low_byte    = profile.pos_low;
        c.high_byte   = profile.pos_high;
        c.scale       = (profile.scale == HID_BYTE) ? 1 : 0;
        c.keepalive_ms= std::max(0, profile.keepalive_ms);

        std::memset(c.on,    0, sizeof(c.on));
        std::memset(c.off,   0, sizeof(c.off));
        std::memset(c.pulse, 0, sizeof(c.pulse));
        std::memset(c.tmpl,  0, sizeof(c.tmpl));
        std::memcpy(c.on,    profile.on,    len);
        std::memcpy(c.off,   profile.off,   len);
        std::memcpy(c.pulse, profile.pulse, len);
        std::memcpy(c.tmpl,  profile.tmpl,  len);

        config.controls.rumble_hid_profile = "custom";
        disabled_this_session.erase(HID_REPORT);
        select_and_open();
    }

    bool hid_test(const HidProfileEdit& profile, HidTestAction action)
    {
        if (test_running)
            return false; // B.7.6: shared TEST limiter

        // B.3.2/B.7.4: the editor may only ever write bytes to a device whose VID/PID matches the
        // profile being tested, regardless of which backend rumble::AUTO/explicit selection has
        // open right now.
        if (!target || profile.vid != target->vid || profile.pid != target->pid)
            return false;

        uint8_t payload[HidProfileEdit::MAX_LEN];
        int len = render_hid_edit_action(profile, action, payload);
        if (len <= 0)
            return false;

        HidWriteResult wr = hid_raw_write(profile.vid, profile.pid,
                                           static_cast<int16_t>(profile.report_id), payload, len);
        if (wr != HidWriteResult::OK)
            return false;

        if (action != HID_TEST_OFF)
        {
            // B.6.2: TEST ON/PULSE auto-off after the same 500 ms cap as the ordinary TEST effect.
            test_running     = true;
            test_deadline_ms = SDL_GetTicks() + kTestDurationCapMs;

            raw_test_pending    = true;
            raw_test_vid        = profile.vid;
            raw_test_pid        = profile.pid;
            raw_test_report_id  = static_cast<int16_t>(profile.report_id);
            raw_test_off_len    = render_hid_edit_action(profile, HID_TEST_OFF, raw_test_off_bytes);
        }
        return true;
    }
}
