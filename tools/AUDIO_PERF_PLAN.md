# Audio path: architecture, underrun analysis, and outstanding work

Audience: maintainers and tools (human or AI) working on `src/main/sdl2/audio.cpp`,
`src/main/hwaudio/*`, or investigating audio dropouts.

Reference platform: Raspberry Pi 2 v1.1 (Cortex-A7, `arm_freq=1050` + `arm_boost`), the tightest
target the project supports. The analysis applies unchanged to the A53 boards, which have more
headroom.

## Status

The real-time scheduling work this document originally planned **has shipped**:

- Optional `SCHED_FIFO` on both audio threads, opt-in via `sound.rt_priority` in `config.xml`,
  default off (commits `5ac3472`, `4da25b6`). It is off by default because on some boards the
  real-time audio threads pre-empt the render threads and cost frame rate.
- Consumer robustness in the SDL callback.
- `res/50-cannonball-audio.conf` ships the PAM limits grant, installed by `install.sh`.

**Section 4 below holds the only work that has not shipped.** Everything else here is retained
because it is the verified description of how the audio path behaves, and because sections 2 and 3
correct conclusions that look plausible and are wrong.

---

## 1. What the code actually does (verified)

```
prepare_thread (or main, -t<4) --audio.tick()--> spawns ONCE: mixThread [std::thread, CFS nice 0]
                                                     |  mixing_loop: spaceAvailable.try_acquire_for(2ms)
                                                     |  fill_and_mix(): osoundint.tick(); pcm->stream_update();
                                                     |                  ym->stream_update(); mix (+wav) -> slot
                                                     v
                                          ringBuffer[BUFFER_COUNT=4] slots of 1 SDL period
                                                     |  samplesReady.acquire()
                                                     v
                              SDL audio thread "SDLAudioP1"
                                 sdl_callback_trampoline: memcpy one slot -> SDL mixbuf
                                 SDL ALSA backend: snd_pcm_writei (blocking) -> ALSA hw ring (2 periods)
                                                     v
                              card0 bcm2835 Headphones (VCHIQ -> VPU firmware -> PWM)
                              card1 vc4-hdmi (DMA)
```

Facts that matter:

* Period = `FREQ/125` frames (`audio.cpp:192-196`): 352 frames = 7.98 ms at 44100; 176 frames =
  7.98 ms at 22050. `callback_rate=1` doubles it to 704 frames = 16 ms. **The period is
  rate-invariant in time**, which is why changing to 22 kHz alone changes nothing.
* Ring = 4 slots (`audio.hpp:144`) = 31.9 ms, or 63.9 ms with `callback_rate=1`.
* SDL2's ALSA backend requests `periods = 2`, so the hardware ring is 2 periods = 16 ms.
  `snd_pcm_writei` returns when one period is free, i.e. when <= 1 period (8 ms) is still queued.
  **The SDL audio thread therefore has an ~8 ms deadline from wake-up to its next write, and
  `BUFFER_COUNT` does not touch that deadline.** This is the binding constraint on the whole path.
* The producer's slack is the ring plus that deadline: up to ~40 ms when the ring is full.
* Output latency, generation to DAC: ~32-48 ms at `callback_rate=0`, ~64-96 ms at `callback_rate=1`.
* The callback blocks in `samplesReady.acquire()` while holding SDL's device mutex, so any
  `SDL_LockAudioDevice` from the game threads waits on the mix thread.
* `pin_thread_to_core()` (`main.cpp:451`) exists but every call site is commented out; all threads
  float, and `mixThread` inherits the spawning thread's unpinned mask.

### Timing summary

| | `callback_rate=0` | `callback_rate=1` |
|-|-------------------|-------------------|
| SDL period | 352 fr = 7.98 ms | 704 fr = 16 ms |
| ALSA hw ring (SDL, 2 periods) | 16 ms | 32 ms |
| SDL thread deadline | ~8 ms | ~16 ms |

### Cost model

Warm, at 1.1 GHz, per 8 ms slot:

| Item | Cycles / slot | Note |
|------|---------------|------|
| YM2151 | 0.12-0.25 M | ~350-700 cycles/sample; `ENV_QUIET` skips silent operators |
| SegaPCM | 0.10-0.20 M | ~40-60 cycles/sample/channel: two non-inlined calls plus a `double` multiply and an `(int)` conversion per sample (`segapcm.cpp:137-143`) |
| tick + mix | ~0.02 M | |
| **Total warm** | **0.25-0.45 M = 0.23-0.41 ms** | ~5% of one core per 8 ms period |

Cold-start costs roughly 700-1200 L2 refills per slot. Unloaded that is 0.15-0.25 ms; with the
VideoCore streaming 1-2 GB/s through a saturated LPDDR2 controller, per-miss latency can reach
1-2 us on an in-order A7 with little miss-level parallelism, i.e. 0.7-2.4 ms. In-situ fill time is
plausibly 0.4-3 ms of an 8 ms period.

**The conclusion that matters: the audio thread does not lack throughput. Even the worst case is
under 40% of one core. It lacks timely scheduling, and it pays latency per miss.**

Working set: `tl_tab` 26.6 KB, `sin_tab` 4 KB, `freq[]` 33.8 KB (touched only at key-on and under
LFO phase modulation), `dt1_freq` 1 KB, EG tables ~0.5 KB, 32 operators ~4.7 KB, chip buffers
2 x 1.4 KB, ring slot 1.4 KB, PCM RAM 256 B. Per-sample hot set is ~20-40 KB — marginal for the
32 KB L1D, trivial for the 512 KB L2.

---

## 2. Plausible hypotheses that are wrong

Recorded so they are not re-derived. Each of these was proposed, investigated and disproved.

1. **"GPU L2 pressure evicts the YM tables every tick."** Wrong mechanism. On BCM2836 the 512 KB
   L2 belongs to the Cortex-A7 MPCore; the VideoCore IV goes straight to the SDRAM controller and
   cannot allocate in it. What evicts the audio tables every frame is the *CPU render pipeline* —
   at least 3 MB streams through a 512 KB L2 per frame. **Consequence: changing the GPU clock
   alters miss latency, not eviction, and core isolation would protect L1 only, never L2.**
2. **"SegaPCM's scattered 384 KB ROM is the worst shape for a contended controller."** Overstated.
   Per 8 ms slot each active channel walks ~125-250 sequential bytes (2-4 cache lines); ~50 line
   fetches per slot across at most 16 cursors, about 0.5 MB/s. Neither a bandwidth nor a
   transaction-count problem. Keeping active regions resident is not worth building.
3. **"The ~70 KB YM working set exceeds L1."** The per-sample hot set is ~20-40 KB. Capacity is not
   the issue; cold start per slot is.
4. **"Buffer depth is the headline resilience number."** No. The binding constraint is the SDL
   audio thread's ~8 ms deadline, set by SDL's 2-period ALSA buffer. Raising `BUFFER_COUNT` alone
   cannot help. `callback_rate=1` helps because it lengthens *both* rings.
5. **"Pitch needs fixed-point."** Hoisting the loop-invariant `(int)(regs[7] * downsample)` to once
   per channel removes 5600 of 5616 double operations per slot. A fixed-point table adds nothing
   beyond that (see F1.2 below).
6. **"mixThread is the single real-time point of failure."** There are two unprotected threads, and
   the SDL one has the tighter deadline.

Audio DRAM *bandwidth* is irrelevant to the GPU: PCM ROM at most 0.7 MB/s plus table and state
refills at most ~10 MB/s, under 1% of the bus. The GPU's saturation matters to the audio thread,
not the other way round.

---

## 3. Bit-exactness harness (`-audiodump <file> <frames>`)

Not implemented. Any change in section 4 needs it, because those changes are meant to be
bit-identical and that claim has to be checkable.

* Requires `-t 1 -perftest` (game logic is already deterministic in that mode; `CB_FRAME_CKSUM`
  relies on it, `main.cpp:694`). Audio device opened but left paused; `mixThread` not spawned.
* Each game frame, call `fill_and_mix()` synchronously into a scratch buffer, bypassing the
  `audio_paused` early return, and append to the file. One 8 ms slot per frame is fine for
  exactness — the sequencer simply runs slower relative to game time, deterministically.
* At frame 120 call `osoundint.queue_sound_service(sound::MUSIC_MAGICAL)` so the YM music path is
  exercised, since attract mode suppresses music. Engine and traffic PCM come from the attract
  demo.
* 3600 frames is about 5 MB. Take its SHA-256. Two runs of the same build must match; then x86
  must match the A7; then every change must match the baseline.
* No custom `.wav`/`.mp3` in `res/` during a dump — the wav path is timing-dependent by design.

---

## 4. Outstanding: free CPU wins, bit-identical

Two of the three are **not implemented**; the third has since landed. They are
worthwhile regardless of scheduling, help the A53 identically, and cannot change output.

**F1.1 — inline `read_buffer`/`write_buffer`** (`soundchip.cpp:57-66`). Move them to the header as
`inline`, or in `SegaPCM::stream_update` take `int16_t* buf = get_buffer()` and write
`buf[2*i] = (int16_t)(buf[2*i] + v*regs[2])` directly. Semantics are identical — integer
arithmetic, truncation to `int16` on store. Note that LTO is off in the default Release build
(it is enabled only under `ENABLE_PROFILING`, `CMakeLists.txt:545`), so these cross-translation-unit
calls are real, not inlined away.

**F1.2 — hoist the PCM increment** (`segapcm.cpp:142-143`). Compute
`const int inc = (int)((double)regs[7] * downsample);` once per channel before the sample loop.
`regs[7]` is not written inside the loop; the only register write there is `regs[0x86] |= 1`. Same
expression, same value, evaluated 16 times per slot instead of 5632.

**F1.3 — SHIPPED.** Removing the dead `SoundChip::clear_buffer()` call from
`YM2151::stream_update` landed as Tier 1 item 3 in commit `c94f086` (see
`tools/PERFRAME_MEMORY_INVENTORY.md`). It saved 1408 dead stores per slot.

A related item remains: `SoundChip::clear_buffer()` (`soundchip.cpp:51-55`) is still a byte loop.
SegaPCM's use of it must stay, because SegaPCM accumulates into the buffer — but the loop itself
can become a `memset`.

Expected effect of F1.1 and F1.2 together: the PCM inner loop drops from ~40-60 to ~12-15 cycles
per sample, roughly 0.1-0.2 Mcycles per slot, or about 30-40% of warm compute. Not decisive for
underruns on its own.

Gate: an identical dump hash before and after each change, on both x86 and the A7.

---

## 5. Deferred, and not recommended

* **Raw-ALSA backend** (bypass SDL audio, 4-8 periods of 8 ms). This is the structural answer if
  real-time scheduling cannot be provisioned on a target. Roughly 200 lines, Linux-only; keeps
  latency at the `callback_rate=1` level while widening the consumer deadline to 24 ms or more.
* **Core reservation** (`-t 3` plus pinning audio to core 3 and game threads to 0-2). Protects L1
  warmth only, and will likely cost frame rate.
* **`BUFFER_COUNT` increase.** Not on the critical path — see section 2.4.
* **YM2151 SIMD or restructuring, resident PCM ROM, hot/cold operator split.** There is no
  throughput problem to justify any of these.

## 6. If you are debugging a report of dropouts

The user-facing guidance is in `docs/TROUBLESHOOTING.md`. In order: USB audio full-speed mode on
Pi 2/3/Zero 2 W, then `callback_rate=1`, then HDMI instead of analogue or USB output. Suggest
`sound.rt_priority=1` last, and only after establishing that frame rate has headroom to give up.

Soak protocol for verifying a fix: Release build with `-march=native`, `performance` governor,
deployed config (hires 1, blargg 1, shader_mode 2, vsync 1, 60 fps, 44100). Five minutes of
attract mode plus one full game with music. Record alsa-lib "underrun occurred" lines on stderr
and `get_throttled` afterwards. Pass is zero of both. Run each configuration twice.
