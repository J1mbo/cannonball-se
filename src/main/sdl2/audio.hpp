/*********************************************************************
 * CannonBall‑SE – SDL Audio Module
 *
 * Description:
 *   Updated SDL‑based audio for CannonBall‑SE.
 *   Improves playback quality and reliability, and provides
 *   WAV/MP3 playback with background loading giving near‑instant
 *   start on low‑power SD‑card systems such as the Raspberry Pi 2.
 *
 * Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
 *
 * Derived from:
 *   • SDL audio code Copyright (c) 2012, 2020 Chris White
 *   • PCM/YM chip handling (c) 1998‑2008 Atari800 development team,
 *     licensed under GPL‑2.0‑or‑later (see docs/license_atari800.txt).
 *
 * This file is part of CannonBall‑SE.
 * The full text of the Cannonball licence is in docs/license.txt.
 *********************************************************************/

#pragma once

#include "globals.hpp"
#include "frontend/config.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <string>
#include <vector>
#include <SDL.h>
#include <semaphore>
#include <chrono>

#ifdef COMPILE_SOUND_CODE


// A Q15-style table mapping slider 1–8 to roughly –8 dB to +6 dB.
// This is used to allow the user to control the playback volume of
// custom .wav files using an 8-step scale in the menu.
// wav/mp3 file sample values are then multipled by these constants,
// and then >> 13 (/8192).
// This avoids expensive floating point division or mod() in the sound
// mixing loop.
static constexpr int32_t WAV_VOL_TABLE[9] = {
    0,      // unused index 0
    3261,   // 1 ≈  –8 dB → 0.40x
    4106,   // 2 ≈  –6 dB → 0.50x
    5169,   // 3 ≈  –4 dB → 0.63x
    6507,   // 4 ≈  –2 dB → 0.79x
    8192,   // 5 ≈  –0 dB → 1.00x
    10313,  // 6 ≈  +2 dB → 1.26×
    12983,  // 7 ≈  +4 dB → 1.58×
    16345   // 8 ≈  +6 dB → 2.00×
};

static constexpr int32_t WAV_THRESHOLD_TABLE[9] = {
    0,       // unused index 0
    24000,   // 1 ≈  –8 dB → 0.40x
    16345,   // 2 ≈  –6 dB → 0.50x
    12983,   // 3 ≈  –4 dB → 0.63x
    10313,   // 4 ≈  –2 dB → 0.79x
    8192,    // 5 ≈  –0 dB → 1.00x
    6507,    // 6 ≈  +2 dB → 1.26×
    5169,    // 7 ≈  +4 dB → 1.58×
    4106     // 8 ≈  +6 dB → 2.00×
};


class Audio
{
public:
    // Enable/Disable Sound
    bool sound_enabled;

    // R3: consumer-side robustness counter. Incremented by
    // sdl_callback_trampoline() each time the ring buffer wasn't ready within
    // its timed wait and the callback fell back to silence instead of
    // blocking. Zero on a healthy system; readable from anywhere (e.g. the
    // periodic diagnostics in main.cpp) for field diagnosis.
    std::atomic<uint64_t> stat_fallbacks{0};

    // In-class Constructor
    Audio()
      : sound_enabled(false),
        FREQ(0),
        bits_per_sample(BITS),
        mix_buffer_bytes(0),
        audio_paused(1),
        dev(0),
        wavfile{ /*filename*/ "", /*data*/ nullptr, /*total_length*/0, /*loaded_length*/ 0,
                 /*pos*/0, /*streaming*/ false, /*fully_loaded*/false, /*stopping*/false }
    {}

    ~Audio();

    void init();
    void start_audio(bool list_devices_only = false);
    void pause_audio();
    void resume_audio();
    void stop_audio();
    void clear_wav();
    void load_audio(const char* filename);
    void tick();
    void fill_and_mix(uint8_t *stream, int len);

    // Playback device enumeration/selection, shared by start_audio() and the
    // SOUND menu's OUTPUT DEVICE entry. The list persists for the process
    // lifetime; refresh_playback_devices() re-enumerates on request (e.g.
    // start_audio(), or the menu when the list is still empty because sound
    // was never enabled).
    void refresh_playback_devices();
    const std::vector<std::string>& playback_devices() const { return playback_devices_; }
    // Name of the currently open device, resolved by start_audio(); empty when
    // using the SDL default (or when no device is open).
    const std::string& active_device_name() const { return active_device_name_; }
    bool device_open() const { return dev != 0; }

    // Resolves the configured sound.callback_rate (0/1/2=AUTO) to the 0 (8ms)
    // or 1 (16ms) value actually used to size the SDL audio buffer. 0/1 pass
    // through unchanged; AUTO resolves to 16ms when running single-core-Pi
    // mode or on a detected "slow" CPU tier (see cputier.hpp), else 8ms. Any
    // other configured value is treated as AUTO. Static so the SOUND menu can
    // preview the resolved value without an Audio instance.
    static int resolve_callback_rate(int configured);

    // The callback rate (0/1) actually in effect, fixed by start_audio() and
    // used by fill_and_mix() every callback - see resolve_callback_rate().
    int effective_callback_rate() const { return callback_rate_effective_; }

private:
    // Stereo. Could be changed, requires some recoding.
    static const uint32_t CHANNELS = 2;
    // 16-Bit Audio Output. Could be changed, requires some recoding.
    static const uint32_t BITS = 16;

    // Sample Rate
    uint32_t FREQ;
    int bits_per_sample;
    uint32_t mix_buffer_bytes; // Each frame is 250 samples and 8ms at 31250kHz sample rate.
    int audio_paused;
    SDL_AudioDeviceID dev;

    // Playback device list (capped at 32), filled by refresh_playback_devices();
    // and which rule start_audio() used to pick the currently open device.
    std::vector<std::string> playback_devices_;
    std::string active_device_name_;
    enum { RULE_DEFAULT = 0, RULE_BY_NAME, RULE_BY_INDEX };
    int active_device_rule_ = RULE_DEFAULT;

    // R3: how long sdl_callback_trampoline() will wait on samplesReady before
    // giving up and filling the callback with silence. Set from the actual
    // negotiated SDL period in start_audio() (period / 2); defaulted here to
    // a sane value (4ms, i.e. an 8ms period) in case it's ever read before
    // start_audio() runs.
    std::chrono::microseconds callback_wait{4000};

    // Callback rate (0=8ms, 1=16ms) actually used, resolved from
    // config.sound.callback_rate by start_audio() via resolve_callback_rate().
    int callback_rate_effective_ = 0;

    void clear_buffers();

    // wave file related
    struct wav_t {
        std::string filename = "";
        int16_t*   data                 = nullptr;   // interleaved PCM samples
        uint32_t   total_length         = 0;         // full sample‐count (int16_t) for entire file
        uint32_t   loaded_length        = 0;         // how many samples have actually been read so far
        uint32_t   pos                  = 0;         // read‐cursor for playback
        uint32_t   fade_pos             = 0;         // position where we start to loop
        bool       streaming            = false;     // true once we've buffered the initial threshold
        bool       fully_loaded         = false;     // true after the loader thread finishes
        bool       stopping             = false;
    };

    // threading
    std::thread        wav_loader_thread;
    std::mutex         wav_mutex;
    std::atomic<bool>  wav_job_pending{false};

    // temporary storage for loaded buffer
    int16_t*           new_data    = nullptr;
    size_t             new_length  = 0;
    bool               new_loaded  = false;

    wav_t wavfile;

    void thread_load_wav(std::string filename);
    void load_wav(const char* filename);

    // new semaphore queue based approach
    // Ring buffer for PCM frames (8ms each)
    static constexpr int BUFFER_COUNT = 4;
    std::vector<int16_t> ringBuffer[BUFFER_COUNT];
    std::atomic<int> prodIndex{0}, consIndex{0};
    std::counting_semaphore<BUFFER_COUNT> spaceAvailable{BUFFER_COUNT};
    std::counting_semaphore<BUFFER_COUNT> samplesReady{0};
    std::atomic<bool> running{false};

    std::thread mixThread;
    void mixing_loop();
    static void sdl_callback_trampoline(void* udata, Uint8* stream, int len);
};
#endif
