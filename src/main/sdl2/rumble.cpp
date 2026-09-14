/***************************************************************************
    Rumble / force-feedback subsystem - mixer, backend selection and safety.

    See sdl2/rumble.hpp for the public API and tools/INPUT_BINDING_DESIGN.md
    Deliverable B for the design this implements. Stage 4a: CORE + the
    SDL_CONTROLLER/SDL_HAPTIC/FFB backends. HID_REPORT (profiles table,
    hidraw/SDL_hid_*) is Stage 4b and is not implemented here - selecting it
    behaves exactly like OFF.

    Safety (B.7), all enforced here rather than in any backend:
      1. Edge-triggered writes - a frame identical to the last one actually
         sent is not re-sent.
      2. Hard rate cap - at most one backend write per tick() call.
      3. 3 consecutive backend failures disable it for the session; AUTO
         falls through to the next candidate, an explicit choice becomes OFF.
      4. Always-OFF guarantees - stop_all() on backend/device changes and on
         shutdown(); a stale-tick guard forces OFF if tick() is not called
         for over 100 ms (a stalled main loop).
      5. The TEST path shares this same limiter and cannot be re-triggered
         while a test is already running.

    Copyright (c) 2026 James Pearce.
    See license.txt for more details.
***************************************************************************/

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include <SDL.h>

#include "sdl2/rumble.hpp"
#include "sdl2/rumble_backend.hpp"
#include "sdl2/input.hpp"       // InputDevice, extern Input input, Input::get_devices()
#include "frontend/config.hpp"  // extern Config config, device_binding_t

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

        bool     test_running = false;
        uint32_t test_deadline_ms = 0;

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
                case FFB:            b = create_ffb_backend();            break;
                default: return nullptr; // AUTO/HID_REPORT/OFF are not directly openable here
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

        void reset_slots()
        {
            for (auto& s : slots)
                s = EffectSlot{};
            test_running = false;
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

            // AUTO order (Stage 4a: no HID_REPORT step - that is Stage 4b):
            // SDL_CONTROLLER -> SDL_HAPTIC -> FFB (if haptic-enabled and not a moving cabinet) -> OFF.
            static const Backend order[] = { SDL_CONTROLLER, SDL_HAPTIC, FFB };
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

        // B.7.1/B.7.2/B.7.3: edge-triggered, at most one write per tick, 3-strikes disable.
        void write_frame(const RumbleFrame& frame)
        {
            if (!active)
                return;

            if (same_frame(frame, last_sent))
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

        if (test_running && now >= test_deadline_ms)
            test_running = false;

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
}
