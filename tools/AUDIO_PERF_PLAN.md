# AUDIO_PERF_PLAN.md - sound underruns and audio footprint on Raspberry Pi 2 v1.1 (Cortex-A7)

Status: design only, no code changed. Written 2026-09-06 against the working tree
(`src/main/sdl2/audio.*`, `src/main/hwaudio/*`, `src/main/engine/audio/*`, `src/main/main.cpp`)
plus a read-only fact pass on the `pi2-local` test box (kernel, limits, ALSA, SDL, clocks).

Goal: eliminate ALSA underruns at 60 fps with the full CRT shader on the A7, then trim the audio
thread's CPU and memory-bus cost. Pure optimisations must be bit-identical; resilience changes may
alter timing but not samples. Nothing may regress A53 (Pi Zero 2 W) or x86.

---

## 0. Recommended order (one screen)

| # | Stage | Kind | Effort | Gate |
|---|-------|------|--------|------|
| S0 | Measure: preconditions, SDL/ALSA buffer facts, in-code counters, scheduler wait stats | tiny code + shell | 0.5 d | We know *which* of the three threads/links is late, and by how much |
| R1 | Zero-code knobs: `callback_rate=1`, `performance` governor, rtprio/nice limits + `chrt`, HDMI vs headphone jack, `-t 3` | none | 0.5 d | Which lever clears the xruns on the 10-minute soak |
| R2 | `SCHED_FIFO` on **both** audio threads (SDL's `SDLAudioP1` and `mixThread`), graceful fallback, provisioning docs | ~40 lines, Linux-only | 0.5 d | Soak passes with `callback_rate=0`; `ps -L` shows `FF` for both threads |
| R3 | Consumer robustness: timed acquire + silence fallback + starvation counters | ~30 lines, portable | 0.5 d | Zero alsa-lib xrun lines even with RT deliberately disabled |
| **GATE A** | Do R1-R3 clear the underruns on the soak? If yes, F-stages are optional housekeeping. | | | |
| R4 | Conditional defaults: `callback_rate=1` on multi-core Pi only where RT provisioning is unavailable; ring depth stays 4 | config | 0.25 d | Only if Gate A fails without RT |
| F1 | Free CPU wins, bit-identical: inline `read/write_buffer`, hoist PCM increment, drop dead YM `clear_buffer`; plus the deterministic `-audiodump` harness that proves it | ~60 lines | 1 d | Identical SHA-256 of dumped audio on x86 and A7; cycles/slot down |
| F2 | Cold-miss reduction, only if S0/F2.1 show miss stall dominates fill time: batch two slots per wake, `sin_tab`/`tl_tab` narrowing, PCM `PLD` | 0.5-1 d after measurement | 1-2 d | L2 refills/slot down >= 25%, audio hash unchanged |
| F3 | Output rate: keep 44100 default; 22050 stays a user option, not a fix | none | - | - |

**Single highest-value change:** R2 - real-time priority on the two audio threads, with the
`limits.d` provisioning that makes it possible (today `ulimit -r` is 0, so even SDL's own
TIME_CRITICAL request for its audio thread fails silently). Runner-up, zero code:
`callback_rate=1`, because it is the only knob that widens the SDL thread's deadline.

**Biggest risk:** that the xruns are on the far side of SDL - the bcm2835 headphone path
(firmware/VCHIQ) fed through SDL's fixed 2-period ALSA buffer - where thread priority cannot help
and only a longer period, HDMI/USB output, or a raw-ALSA backend can. S0 is designed to tell these
apart before any code is written.

---

## 1. What the code actually does (verified)

```
prepare_thread (or main, -t<4) --audio.tick()--> spawns ONCE: mixThread [std::thread, CFS nice 0]
                                                     |  mixing_loop: spaceAvailable.try_acquire_for(2ms)
                                                     |  fill_and_mix(): osoundint.tick(); pcm->stream_update();
                                                     |                  ym->stream_update(); mix (+wav) -> slot
                                                     v
                                          ringBuffer[BUFFER_COUNT=4] slots of 1 SDL period
                                                     |  samplesReady.acquire()  (unconditional, no timeout)
                                                     v
                              SDL audio thread "SDLAudioP1" [CFS nice 0 on this box]
                                 sdl_callback_trampoline: memcpy one slot -> SDL mixbuf
                                 SDL ALSA backend: snd_pcm_writei (blocking) -> ALSA hw ring (2 periods)
                                                     v
                              card0 bcm2835 Headphones (VCHIQ -> VPU firmware -> PWM)   <- playback_device=0 on the box
                              card1 vc4-hdmi (DMA)
```

Facts that matter, with line references:

* Period = `FREQ/125` frames (`audio.cpp:192-196`): 352 frames = 7.98 ms at 44100; 176 frames =
  7.98 ms at 22050. `callback_rate=1` doubles it to 704 frames = 16 ms. The period is rate-invariant
  in *time*, which is why 22 kHz alone changed nothing.
* Ring = 4 slots (`audio.hpp:144`) = 31.9 ms (63.9 ms with `callback_rate=1`).
* SDL2's ALSA backend requests `periods = 2` (`set_periods_min(2)` + `set_periods_first`), so the
  ALSA hardware ring is 2 periods = 16 ms. `snd_pcm_writei` returns when one period is free, i.e.
  when <= 1 period (8 ms) is still queued. **The SDL audio thread therefore has an ~8 ms deadline
  from wake-up to its next write, and BUFFER_COUNT does not touch that deadline.** (Verify with
  `SDL_AUDIO_ALSA_DEBUG=1`, which prints period/periods/buffer - S0.2.)
* The producer's slack is ring + that deadline: up to ~40 ms when the ring is full.
* Output latency, generation to DAC: ~32-48 ms (`callback_rate=0`), ~64-96 ms (`callback_rate=1`).
* The callback blocks in `samplesReady.acquire()` **while holding SDL's device mutex** (SDL calls
  the callback under `mixer_lock`). Any `SDL_LockAudioDevice` from the game threads
  (`clear_buffers()` on resume, `clear_wav()` on a track change) waits on the mix thread. A latent
  main-thread hitch, fixed for free by R3.
* No `sched_*`, `pthread_setschedparam`, `setpriority`, `nice` or affinity call exists in `src/`.
  `pin_thread_to_core()` (`main.cpp:451`) exists but every call site is commented out; all threads
  float. `mixThread` inherits the spawning thread's (unpinned) mask.
* On the Pi 2 the default is `-t 4` (`game_threads = omp_get_max_threads()`, `main.cpp:183`):
  main + prepare + 2 render threads, plus mixThread, SDLAudioP1, the wav loader, and GOMP worker
  threads created by the palette-regeneration `#pragma omp parallel` regions
  (`snes_ntsc.cpp:326/368`, `rendersurface.cpp:713` - setup-time, not per frame, but GOMP workers
  spin briefly after each region).
* Per-slot work, `fill_and_mix` (`audio.cpp:338-423`): one `osoundint.tick()` (Z80 sequencer port,
  cheap), `SegaPCM::stream_update` (16 channels x 352 samples, `segapcm.cpp:94-152`),
  `YM2151::stream_update` (352 x [advance_eg + 8 chan_calc + advance], `ym2151.cpp:2018-2106`),
  then a clamp-and-mix loop.

### 1.1 Cost model (estimates - S0 replaces them with measurements)

Warm, 1.1 GHz (the box runs `arm_freq=1050` + `arm_boost`):

| Item | Cycles / slot | Note |
|------|---------------|------|
| YM2151 | 0.12-0.25 M | ~350-700 cycles/sample; ENV_QUIET skips silent operators |
| SegaPCM | 0.10-0.20 M | ~40-60 cycles/sample/channel today: two non-inlined calls + `double` mul + `(int)` per sample (`segapcm.cpp:137-143`) |
| tick + mix | ~0.02 M | |
| **Total warm** | **0.25-0.45 M = 0.23-0.41 ms** | ~5% of one core per 8 ms period |

Cold-start penalty per slot: roughly 700-1200 L2 refills (YM state ~5 KB, `sin_tab` 4 KB, the
touched subset of `tl_tab`, EG tables, PCM RAM, ~50 PCM ROM lines, output buffers, ring slot,
and 10-15 KB of code). At an unloaded ~0.2 us per miss that is 0.15-0.25 ms; under a saturated
LPDDR2 controller with the VideoCore streaming ~1-2 GB/s, per-miss latency can reach 1-2 us on an
in-order A7 with almost no miss-level parallelism, i.e. 0.7-2.4 ms. **In-situ fill time is
plausibly 0.4-3 ms of an 8 ms period.** Even the worst case is < 40% of one core: the audio
thread does not lack throughput, it lacks *timely scheduling* and pays *latency* per miss.

Working set: `tl_tab` 26.6 KB (int32, values fit int16), `sin_tab` 4 KB (uint32, values <= ~8.6k),
`freq[]` 33.8 KB (touched only at key-on and when LFO phase modulation is active,
`ym2151.cpp:1947-1950`), `dt1_freq` 1 KB, EG tables ~0.5 KB, 32 operators ~4.7 KB, chip buffers
2 x 1.4 KB, ring slot 1.4 KB, PCM RAM 256 B. Per-sample hot set ~20-40 KB: marginal for the 32 KB
L1D, trivial for the 512 KB L2.

Audio DRAM *bandwidth* is irrelevant to the GPU: PCM ROM <= 0.7 MB/s plus table/state refills
<= ~10 MB/s, under 1% of the bus. The GPU's saturation matters to the audio thread, not the other
way round.

---

## 2. Facts from the box (pi2-local, 2026-09-06, game not running)

* Cortex-A7 x4 at 1.1 GHz (`arm_freq=1050`, `arm_boost=1`, `over_voltage=2`), `gpu_freq=500`
  (V3D and core both 500 MHz). `vcgencmd get_throttled` = **0x0** - the under-voltage noted on
  2026-09-03 is gone. Re-check the sticky bits after a play session (S0.1).
* Caches: L1D 32 KB 4-way 64 B lines; L1I 32 KB 2-way 32 B lines; L2 512 KB 8-way 64 B lines.
  (`getconf LEVEL1_DCACHE_LINESIZE` returns 0 here - use sysfs.)
* Governor **ondemand** (single policy for all four cores). Under a GPU-bound game with ~50% CPU
  the ARM clock can sit at 600 MHz for stretches.
* Kernel 6.18 Raspbian armv7: **PREEMPT_VOLUNTARY**, NO_HZ_IDLE. Not PREEMPT, not RT. A waking
  thread preempts user-mode code promptly via IPI, but not code inside a non-preemptible kernel
  section (e.g. vc4 BO cache maintenance, which on ARMv7 is a CPU loop over MBs).
* `ulimit -r` = 0, `ulimit -e` = 0, no rtkit, no rtprio/nice lines in `limits.conf`. Therefore SDL
  2.32.4's `SDL_SetThreadPriority(TIME_CRITICAL)` on its audio thread fails silently: **both audio
  threads are plain CFS nice 0 today.** `sysadmin` is in group `audio` (gid 29) and `sudo`.
* `perf_event_paranoid` = 2 (self-monitoring with `exclude_kernel` allowed); no `perf` tool
  installed; `chrt`/`taskset` present.
* ALSA direct (no PulseAudio/PipeWire running, no `.asoundrc`). Cards: 0 = bcm2835 Headphones,
  1 = vc4-hdmi. Deployed `config.xml`: `rate=44100`, `callback_rate=0`, `playback_device=0`,
  `hires=1`, `blargg=1`, `shader_mode=2`, `vsync=1`.
* Deployed tree is `master` @ 086591c (v1.41); the audio code there is the same as this tree's.

---

## 3. Failure-mode model - what S0 must discriminate

Three links can miss a deadline; each has a different fix.

| Hypothesis | Symptom in S0 counters | Fix family |
|------------|------------------------|------------|
| **H1 consumer late** - SDLAudioP1 (CFS) not scheduled within its ~8 ms window | Callback-to-callback interval gaps > 8 ms; ring occupancy at callback entry usually >= 1; xruns coincide with gaps | R2 (FIFO on the SDL thread); R1 `callback_rate=1` widens the window to 16 ms |
| **H2a producer late** - mixThread (CFS) starved | Ring occupancy hits 0 at callback entry; fill time histogram small (< 3 ms) | R2 (FIFO on mixThread); ring depth is a band-aid |
| **H2b producer slow** - fill time inflated by cold misses on a saturated bus and/or 600 MHz | Occupancy hits 0 *and* fill-time tails >= 4 ms | R2 + `performance` governor + F1/F2 |
| **H3 device side** - bcm2835 firmware path (coarse consumption granularity, VCHIQ round-trips) or SDL's 2-period ring is simply too small for this device | Callbacks regular, occupancy >= 1 always, no fill-time tails, **xruns still logged** | `callback_rate=1` (32 ms hw ring), HDMI or USB DAC, or a raw-ALSA backend with 4+ periods; priority cannot help |

The user's observation that xruns appeared when the GPU clock went up is consistent with all of
H1/H2b/H3: the higher GPU clock raises bus occupancy (miss latency) and, on the headphone path,
loads the VideoCore side too.

---

## 4. Where this plan disagrees with the discovery findings

1. **"GPU L2 pressure may evict the YM tables every tick."** Wrong mechanism, same outcome. On
   BCM2836 the 512 KB L2 belongs to the Cortex-A7 MPCore; the VideoCore IV goes straight to the
   SDRAM controller and cannot allocate in it. What evicts the audio tables every frame is the
   *CPU render pipeline* (S16 frame -> ~749 x 448 x 4 B NTSC output -> texture upload: >= 3 MB
   streams through a 512 KB L2 per frame). Consequence: GPU clock changes do not alter eviction,
   only miss latency; and core isolation would protect L1 only, never L2.
2. **"SegaPCM's scattered 384 KB ROM is the worst shape for a contended controller."**
   Overstated. Per 8 ms slot each active channel walks ~125-250 sequential bytes (2-4 lines);
   ~50 line fetches per slot across <= 16 cursors, ~0.5 MB/s. That is neither a bandwidth nor a
   transaction-count problem. "Keep active regions resident" is not worth building; one `PLD` per
   channel at loop entry is the most that is warranted (F2.4).
3. **"~70 KB YM working set > L1."** The per-sample hot set is ~20-40 KB; `freq[]` (34 KB) is
   touched only at key-on and under LFO PM. Capacity is not the issue; cold start per slot is.
4. **Buffer depth as the headline resilience number.** The binding constraint is the SDL audio
   thread's ~8 ms deadline set by SDL's 2-period ALSA buffer. Raising `BUFFER_COUNT` alone cannot
   fix H1 or H3; `callback_rate=1` helps because it lengthens *both* rings.
5. **"Pitch fixed-point."** Hoisting the loop-invariant `(int)(regs[7] * downsample)` to once per
   channel removes 5600 of 5616 double operations per slot; a fixed-point table adds nothing.
6. **"mixThread is the single real-time point of failure."** There are two unprotected threads,
   and the SDL one has the tighter deadline. SDL *tries* to raise its own thread and fails on this
   box (limits = 0, no rtkit).
7. **Missing hypothesis:** the output device itself (`playback_device=0` = bcm2835 headphones,
   a firmware-serviced path). Cheapest test on the list (R1.4).
8. Agreed: audio is not per-frame (one persistent free-running thread); the callback's unconditional
   acquire; no priority code anywhere; the three free wins are real and bit-identical.

---

## 5. Stages

### S0 - Measure first (0.5 day, no behavioural change)

**S0.1 Preconditions during play.** `vcgencmd get_throttled` before and after a 10-minute session
(bits 16-19 are sticky since boot); `vcgencmd measure_clock arm` and
`/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq` sampled every second during play; note
which SDL device index 0 resolves to from the game's "Available audio devices" banner.

**S0.2 Buffer facts.** Run with `SDL_AUDIO_ALSA_DEBUG=1`; expect
`ALSA: period size = 352, periods = 2, buffer size = 704`. If periods > 2 on the headphone card,
the consumer deadline is correspondingly wider and the numbers in section 1 must be revised.

**S0.3 In-code counters** (audio.cpp, ~60 lines, always compiled, printed only when
`fps_counter` is on, alongside the existing `PIPE ms/frame` line at `main.cpp:752`, every 2 s and
at exit):
* `pthread_setname_np` both threads (`cb-audiomix`; SDL already names its thread `SDLAudioP1`).
* fill_and_mix wall time per slot: histogram `<0.5 <1 <2 <4 <8 >=8 ms`, max.
* Ring occupancy at callback entry (0..4) histogram and running min - keep an atomic
  `prod - cons` counter, since semaphore counts are not readable.
* Max callback-to-callback interval.
* (After R3) fallback count.

**S0.4 Scheduler view, zero code.** `echo 1 | sudo tee /proc/sys/kernel/sched_schedstats`, then
read the `wait_max` line of `/proc/<pid>/task/<tid>/sched` for `SDLAudioP1` and `cb-audiomix`
over a soak. This is the worst-case run-queue wait, the number the whole resilience story turns on.

**Gate:** the decision table in section 3 assigns the xruns to H1/H2a/H2b/H3. Do not start R2 or
F-work without it. Expected outcome (my prediction): H1 dominant, H2b contributing, H3 possible on
the headphone card.

### R1 - Zero-code knobs (0.5 day of soak runs)

Run each on the soak protocol (section 6.1) and record the S0 counters:
1. `callback_rate=1` in config.xml. Widens the SDL deadline 8 -> 16 ms and the ring 32 -> 64 ms,
   halves the number of cold starts per second; costs ~+40 ms output latency.
2. `performance` governor (`echo performance | sudo tee .../cpu0/cpufreq/scaling_governor`) or
   `force_turbo=1`. Removes the 600 MHz stretches.
3. Provision limits (Appendix B) and re-login. With `nice -20` allowed, SDL's own TIME_CRITICAL
   request now *succeeds* (confirm `ps -L -o tid,cls,ni,comm` shows `SDLAudioP1` at ni -20). Then
   `sudo chrt -f -p 55 <tid of SDLAudioP1>` and `sudo chrt -f -p 50 <tid of cb-audiomix>` at
   runtime: this is R2 without code, for one session.
4. `playback_device` -> the vc4-hdmi index (if the attached screen has audio) or a USB DAC.
   Discriminates H3 outright.
5. `-t 3` (no separate prepare thread): fewer runnable CPU threads; check fps holds.
6. `OMP_WAIT_POLICY=passive` to rule out GOMP spin after palette regeneration.

**Gate:** which single lever clears the xruns. If (3) does, R2 is confirmed as the fix. If only
(1) or (4) does, H3 is in play and R4 / the raw-ALSA fallback moves up. If (2) alone helps a lot,
document `force_turbo`/`performance` as a recommended setting in install.sh.

### R2 - Real-time priority on both audio threads (~40 lines, Linux-only)

**Change.** In `mixing_loop()` on entry, and in `sdl_callback_trampoline()` on first entry
(`pthread_self()` there *is* SDL's audio thread; use a `std::once_flag`), call
`pthread_setschedparam(pthread_self(), SCHED_FIFO, {prio})` with prio 50 (mix) and 55 (SDL,
consumer above producer). On `EPERM` log one line naming the fix (Appendix B) and continue on
CFS; optionally also try `setpriority(PRIO_PROCESS, tid, -15)` as a middle rung. Guard with
`#ifdef __linux__`; no-op elsewhere. Also raise the producer's `try_acquire_for(2ms)` poll to 8 ms
(it exists only to re-check `running`; 500 wakeups/s is noise).

Provisioning: ship `res/50-cannonball-audio.conf` (Appendix B) and have `install.sh` copy it to
`/etc/security/limits.d/` (or add `LimitRTPRIO=80`/`LimitNICE=-20` to the systemd unit if the
game is launched by one). `setcap cap_sys_nice=ep` is the alternative for a bare binary.

**Why.** Both deadlines (8 ms consumer, ~40 ms producer) are violated only by scheduling delay,
which on PREEMPT_VOLUNTARY with 4-6 runnable CFS threads on 4 cores has 10-30 ms tails. FIFO
turns that into sub-millisecond, bounded only by non-preemptible kernel sections. The RT
scheduler's push/pull places a woken FIFO task on any core, preempting CFS work, so pinning is
unnecessary.

**Safety.** Both threads are self-throttling by construction: the mix thread blocks on
`spaceAvailable` (ring full) and does ~0.5-3 ms of work per 8 ms; the SDL thread blocks in
`snd_pcm_writei` on the hardware. A runaway is impossible without a bug that also breaks audio,
and the kernel's RT throttle (`/proc/sys/kernel/sched_rt_runtime_us`, expect 950000) leaves >= 5%
per core regardless; the hardware watchdog is petted by the CFS main thread on other cores. Never
apply FIFO to the process, main, render, prepare or wav-loader threads.

**Affinity.** Do not pin. With FIFO it buys nothing; without FIFO, pinning to a core a render
thread also uses is harmful (forces CFS time-slicing, forbids migration to an idle core). The only
benefit of a private core is L1 warmth between slots, which is an F2 question, and it would need
the game threads pinned away too (and probably costs fps). Leave as a measured experiment after
F2.1, not part of the fix.

**Known small inversion.** A FIFO mix thread can wait on a CFS wav-loader holding `wav_mutex`
during a <= 16 KB memcpy (`audio.cpp:590-602`), only while custom music plays - tens of us,
bounded. If S0 counters ever show it, shrink the critical section (copy outside the lock, swap
pointer) or use a PI mutex.

**Expected effect.** H1 and H2a vanish; H2b tails shrink (the thread runs as soon as it is
runnable). **Gate:** 10-minute soak with `callback_rate=0`, zero xruns, ring occupancy min >= 1,
callback interval max < 12 ms; `ps -L` shows `FF` for both threads; x86/A53 builds unchanged in
behaviour (EPERM path exercised on WSL by running with `ulimit -r 0`).

### R3 - Consumer robustness (~30 lines, portable)

**Change.** `sdl_callback_trampoline`: `samplesReady.try_acquire_for(period/2)` (4 ms at 8 ms
periods, 8 ms at 16 ms). On timeout: `memset(stream, 0, len)` (S16 silence), increment a
fallback counter, return without touching `consIndex`/`spaceAvailable` - the ring invariant holds
and the late slot plays next period. Stream time shifts by one period per event; it cannot
drift beyond the ring depth because the producer can never be more than 4 slots ahead.

**Why.** A transient stall becomes an 8 ms gap and a counted event instead of an alsa-lib xrun
(which is a gap *plus* `snd_pcm_recover`, `prepare`, and a stderr storm). Also removes the
"callback blocks holding SDL's device mutex" hazard in section 1. `std::counting_semaphore::
try_acquire_for` is already used on the producer side, so MSVC/libstdc++ behaviour is known.

**Gate:** with RT deliberately disabled (`ulimit -r 0`) and `callback_rate=0`, a soak logs zero
alsa-lib xrun lines; the fallback counter reports how often the stall path fired. With R2 on, the
counter should read 0 over 10 minutes.

### GATE A

If R2 + R3 give zero xruns and zero fallbacks over the soak at `callback_rate=0`, the underrun
problem is closed. F1 remains worthwhile (free, bit-identical, reduces the A7's per-slot cost by
roughly a third of the warm compute) but is no longer on the critical path. F2 is optional.

### R4 - Conditional defaults (config only)

Only if Gate A fails *without* RT (a target where `limits.d` cannot be provisioned): default
`callback_rate=1` on multi-core Raspberry Pi (extend the existing `singleCorePi()` detection in
`main.cpp:1036-1052`), or have `install.sh` set it when `ulimit -r` is 0. Ring depth: keep 4;
expose `sound.buffer_slots` only if S0 shows producer starvation with FIFO on (unlikely). A ring
beyond ~48 ms buys nothing because the consumer deadline, not producer slack, is the bottleneck.

### F1 - Free CPU wins, bit-identical (~1 day including the harness)

**F1.0 Harness first** (section 6.2): `-audiodump <file> <frames>` deterministic capture and
reference SHA-256 on x86 and A7. Nothing in F1/F2 lands without an unchanged hash.

**F1.1 Inline `read_buffer`/`write_buffer`** (`soundchip.cpp:57-66`): move to the header as
`inline`, or in `SegaPCM::stream_update` take `int16_t* buf = get_buffer()` and write
`buf[2*i] = (int16_t)(buf[2*i] + v*regs[2])` directly. Semantics identical (int arithmetic,
truncation to int16 on store). LTO is deliberately off only under `ENABLE_PROFILING`
(`CMakeLists.txt:545`); the default Release build has no IPO, so the cross-TU calls are real.

**F1.2 Hoist the PCM increment** (`segapcm.cpp:142-143`): `const int inc = (int)((double)regs[7]
* downsample);` once per channel before the sample loop; `regs[7]` is not written inside the loop
(the only register write is `regs[0x86] |= 1`). Same expression, same value, evaluated 16x
instead of 5632x per slot. No table needed.

**F1.3 Remove `SoundChip::clear_buffer()` in `YM2151::stream_update`** (`ym2151.cpp:2020`): the
loop at 2052-2105 writes both channels for every `i < length` with no early exit. SegaPCM's clear
must stay (it accumulates); turn its loop into `memset`. Dead work: 1408 stores per slot.

**Expected:** PCM inner loop from ~40-60 to ~12-15 cycles/sample; roughly 0.1-0.2 Mcycles per
slot, i.e. ~30-40% of warm compute. Not decisive for underruns, but it lowers the H2b exposure on
the A7 for free and helps the A53 identically.

**Gate:** identical dump hash on WSL x86 and on the A7 before/after each of F1.1-F1.3 (also x86
vs A7 should match each other: the only platform-specific code is `__builtin_arm_ssat` vs the
clamp, which are equivalent); S0 fill-time histogram shifts left; no build-flag change.

### F2 - Cold-miss reduction (conditional; measure first)

Enter only if S0/F2.1 show in-situ cycles per slot >> warm cycles per slot (miss stall dominates)
*and* margin after R2/R3 is thin.

**F2.1 Measurement** (section 6.3): PMU counters around `fill_and_mix` on the mix thread, in
situ vs warm (`-audiodump -perftest` mode, no rendering). Attribute per chip with an env toggle
that skips one `stream_update` in a measurement build. This decides whether YM tables, PCM ROM or
code dominate refills before any restructuring.

**F2.2 Batch two slots per wake.** In `mixing_loop`, wait until two slots are free and fill both
(the second runs warm). Halves cold starts per second without touching period or latency
structure; minimum producer slack becomes 16 ms, fine under FIFO. `callback_rate=1` achieves the
same at +40 ms latency, so this is the way to get the benefit while keeping 8 ms periods.

**F2.3 Table narrowing, bit-identical.** `sin_tab` uint32 -> uint16 (max value ~8.6k), `tl_tab`
int32 -> int16 (|v| <= 8192): halves their line count (30 KB -> 15 KB). Add an init-time range
check that aborts if any generated value does not fit. Values unchanged, loads just extend.
Optional: hot/cold split of `YM2151Operator` (~148 B, most fields hot) - low value, skip unless
F2.1 says operator state dominates.

**F2.4 PCM prefetch.** One `__builtin_prefetch(rom + ((addr>>8)&rgnmask) + 64)` at channel entry;
the A7 has no hardware data prefetcher and each cursor is sequential.

**F2.5 Not doing:** resident/pinned PCM regions (see section 4.2); YM restructure for SIMD (per-
sample dependent feedback and per-operator envelope logic; no headroom problem to justify it).

**Gate:** L2 refills per slot and in-situ cycles per slot down >= 25% on the A7; audio hash
unchanged; A53 numbers not worse.

### F3 - Output rate

Keep 44100 as the default. 22050 halves per-sample compute and output-buffer traffic but not the
per-slot table cold misses, and it costs quality: the MAME YM2151 core runs at the output rate
with no oversampling (the real chip runs at 55.9 kHz), so FM partials above ~11 kHz alias on
bright leads and hi-hats; the 8-bit PCM is barely affected. 31250 would make the PCM pitch exact
(`downsample = 1.0`) but mpg123 cannot output that rate, breaking custom music. Since the failure
is latency-bound (section 1.1), rate is not a fix, which matches the user's observation that
22 kHz did nothing.

---

## 6. Verification protocols

### 6.1 Underrun soak (used for every R gate)

Pi 2, Release build (`-march=native`), `performance` governor, deployed config (hires 1,
blargg 1, shader_mode 2, vsync 1, 60 fps, 44100, `callback_rate` as under test). 5 minutes of
attract mode plus one full game with music (~5 minutes). Record: alsa-lib "underrun occurred"
lines on stderr, S0 counters (fallbacks, min occupancy, fill-time max/histogram, max callback
interval), `wait_max` for both threads, `get_throttled` after. **Pass = 0 xrun lines and 0
fallbacks.** Run each configuration twice.

### 6.2 Bit-exactness dump (`-audiodump <file> <frames>`, ~40 lines)

* Requires `-t 1 -perftest` (game logic already deterministic in that mode - `CB_FRAME_CKSUM`
  relies on it, `main.cpp:694`). Audio device opened but left paused; `mixThread` not spawned.
* Each game frame: `fill_and_mix()` synchronously into a scratch buffer (bypass the
  `audio_paused` early return in dump mode), append to the file. One 8 ms slot per frame is fine
  for exactness; the sequencer just runs slower relative to game time, deterministically.
* At frame 120 call `osoundint.queue_sound_service(sound::MUSIC_MAGICAL)` so the YM music path
  is exercised even though attract mode suppresses music; engine and traffic PCM come from the
  attract demo (`advertise=1`).
* 3600 frames = ~5 MB. `sha256sum` it. Sanity: two runs of the same build must match; then WSL
  x86 must match the A7; then every F-change must match the baseline.
* No custom `.wav/.mp3` in `res/` during the dump (the wav path is timing-dependent by design).

### 6.3 PMU measurement (A7 vs A53 note)

In-process `perf_event_open`, opened from the mix thread on itself (`pid=0, cpu=-1,
exclude_kernel=1, exclude_hv=1` - required by `perf_event_paranoid=2`), one group read before and
after `fill_and_mix`, accumulated into the S0 print. Events, all ARM "common architectural"
numbers valid on both cores via `PERF_TYPE_RAW`:

| Event | A7 (ARMv7 PMUv2) | A53 (ARMv8 PMUv3) |
|-------|------------------|-------------------|
| CPU_CYCLES | 0x11 | 0x11 |
| INST_RETIRED | 0x08 | 0x08 |
| L1D_CACHE_REFILL | 0x03 | 0x03 |
| L2D_CACHE_REFILL | 0x17 | 0x17 |
| BUS_ACCESS | 0x19 | 0x19 |
| Counters available | 4 + cycle counter | 6 + cycle counter |

Differences to respect: the A7 has only 4 programmable counters (the four events above fit
exactly if cycles use the dedicated cycle counter via `PERF_COUNT_HW_CPU_CYCLES`); the A53's
implementation-defined 0xC0+ events do not exist on the A7; 32-bit ARMv7 needs the `armv7_
cortex_a7` PMU driver, which the RPi kernel ships. If preferred, `apt install linux-perf` and
`perf stat -e r11:u,r03:u,r17:u,r08:u -t <tid> -- sleep 10` gives the same numbers without code.

The decisive comparison is in-situ cycles/slot (game rendering at 60 fps) vs warm cycles/slot
(`-audiodump -perftest`, no rendering); the gap is the cache/bus penalty, and refill counts say
which chip pays it.

---

## 7. Deferred / not recommended

* **Raw-ALSA backend** (bypass SDL audio, 4-8 periods of 8 ms): the structural answer to H3 or to
  targets where RT cannot be provisioned. ~200 lines, Linux-only, keeps latency at the
  `callback_rate=1` level while widening the consumer deadline to 24 ms+. Only after S0 says H3.
* **Core reservation** (`-t 3` + pin audio to core 3 + pin game threads to 0-2): protects L1
  warmth only, likely costs fps; revisit only if F2.1 shows L1 cold start dominates.
* **BUFFER_COUNT increase**: not on the critical path (section 4.4).
* **YM2151 SIMD/restructure, resident PCM ROM, hot/cold operator split**: no headroom problem to
  justify them.

## 8. Risks, ranked

1. **H3 (device side).** If the bcm2835 headphone path is the xrun source, R2/R3 reduce but do
   not eliminate. Mitigation: R1.4 (HDMI/USB) and R1.1 are run *before* R2 is written.
2. **RT provisioning fails silently in the field.** Mitigation: explicit log line with the fix on
   EPERM; `install.sh` ships the limits file; R4 fallback default.
3. **FIFO misuse.** Only the two self-throttling audio threads; RT throttle and watchdog verified
   in R2's gate. Never the process.
4. **Harness determinism.** Relies on `-t 1 -perftest` determinism; sanity-checked by a double run
   before any comparison.
5. **F2.3 narrowing.** Init-time range assertion; hash gate.
6. **`wav_mutex` inversion** under FIFO: bounded, only with custom music; fix shape known.

---

## Appendix A - Timing summary

| | `callback_rate=0` | `callback_rate=1` |
|-|-------------------|-------------------|
| SDL period | 352 fr = 7.98 ms | 704 fr = 16 ms |
| ALSA hw ring (SDL, 2 periods) | 16 ms | 32 ms |
| SDL thread deadline | ~8 ms | ~16 ms |
| Ring (4 slots) | 31.9 ms | 63.9 ms |
| Producer slack (ring full) | ~40 ms | ~80 ms |
| Output latency | 32-48 ms | 64-96 ms |
| Cold starts per second | 125 | 62.5 |

## Appendix B - Provisioning

```
# /etc/security/limits.d/50-cannonball-audio.conf   (sysadmin is in group audio)
@audio   -   rtprio    80
@audio   -   nice     -20
@audio   -   memlock   unlimited
```
Takes effect at next login (pam_limits). systemd unit alternative: `LimitRTPRIO=80`,
`LimitNICE=-20`. Bare-binary alternative: `sudo setcap cap_sys_nice=ep cannonball-se`
(needs `libcap2-bin`). Check the RT throttle: `cat /proc/sys/kernel/sched_rt_runtime_us`
(expect 950000; `sysctl` is not installed on the box).

## Appendix C - Code sketches (for the implementer; not applied)

R2, Linux only:
```cpp
static void audio_set_rt(const char* who, int prio) {
#ifdef __linux__
    sched_param sp{}; sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc) std::cerr << "audio: " << who << ": SCHED_FIFO " << prio << " refused ("
                      << strerror(rc) << "). Add '@audio - rtprio 80' to "
                         "/etc/security/limits.d or LimitRTPRIO= to the unit.\n";
#endif
}
// mixing_loop(): pthread_setname_np(pthread_self(), "cb-audiomix"); audio_set_rt("mix", 50);
// sdl_callback_trampoline(): static std::once_flag f; std::call_once(f, []{ audio_set_rt("sdl", 55); });
```

R3:
```cpp
void Audio::sdl_callback_trampoline(void* udata, Uint8* stream, int len) {
    auto* self = static_cast<Audio*>(udata);
    if (!self->samplesReady.try_acquire_for(self->callback_wait)) {   // period/2
        memset(stream, 0, len);                                       // S16 silence
        self->stat_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return;                                                       // ring state untouched
    }
    auto& src = self->ringBuffer[self->consIndex.load()];
    memcpy(stream, src.data(), len);
    self->consIndex = (self->consIndex + 1) % BUFFER_COUNT;
    self->spaceAvailable.release();
}
```

F1.2:
```cpp
const int inc = (int)((double)regs[7] * downsample);   // once per channel, same expression
for (i = 0; i < frame_size; i++) { ... addr = (addr + inc) & 0xffffff; }
```

## Appendix D - Commands

```
SDL_AUDIO_ALSA_DEBUG=1 ./cannonball-se                     # period/periods/buffer
ps -L -o tid,cls,rtprio,ni,psr,pcpu,comm -p $(pgrep -o cannonball-se)
sudo chrt -f -p 55 <tid SDLAudioP1>; sudo chrt -f -p 50 <tid cb-audiomix>
echo 1 | sudo tee /proc/sys/kernel/sched_schedstats; grep wait_max /proc/<pid>/task/<tid>/sched
echo performance | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
vcgencmd get_throttled; vcgencmd measure_clock arm
./cannonball-se -t 1 -perftest -audiodump out.raw 3600 && sha256sum out.raw
```
