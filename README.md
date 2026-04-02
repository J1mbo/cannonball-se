# CannonBall‑SE

*A performance tuned and easy-to-use fork of Chris White’s CannonBall OutRun engine with enhanced graphics and cabinet‑friendly enhancements.*

---

## Overview

CannonBall‑SE is a fork of the cross‑platform OutRun engine, focused on an authentic arcade‑cab experience.

* Graphics seem more natural thanks to the video filter, subtle screen curvature, shadow‑mask effects, attention to accurate colour reproduction, subtle noise and desaturation, and automatic 30/60fps selection.
* The re-written audio module tightens up stability, ensures accurate playback speed, and also supports MP3/WAV custom music.
* Gameplay is refined with various game bug fixes, additional rumble, performance improvements and stability fixes.
* The hardware watchdog is used (if available) to ensure reliable continuous operation.
* Play count and "machine hours" are tracked.

> **Note:** Can be used on all Raspberry Pi systems including the Pi Zero.

---

## Supported Platforms

* **All Raspberry Pi Boards** running Raspberry Pi OS.
* **x86/x64 PCs** (Intel/AMD) running a recent Linux distribution (e.g., Ubuntu).

A desktop is not required - use the command-line version of the OS.

---

## ROMs (Required)

CannonBall‑SE requires a copy of the original **OutRun revision B** ROM set. Copy the ROMs into the project’s `roms/` directory. You are expected to legally own the original ROMs; usage may be restricted by local law.

---

## Quick Start Guide

Use the included script 'install.sh' to install prerequisites, build the project, and set device permissions automatically. On a Pi4 or Intel machine, this will only take a few minutes.

```bash
# 1) Fetch sources
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se

# 2) Build & set up (installs deps, compiles, grants permissions)
chmod +x install.sh
./install.sh

# 3) Add your OutRun rev B ROMs
mkdir -p roms
# copy your ROM set into ./roms

# 4) Reboot, then run
build/cannonball-se
# Note: If running under a desktop, Wayland will likely improve frame-rate; start as:
# SDL_VIDEODRIVER=wayland build/cannonball-se
```

The script installs system packages, compiles the game (optimised for the system it's building on) with CMake into `./build/`, and applies permissions so it can access `/dev/watchdog` and `/dev/hidraw` for rumble.

---

## Command‑line Options (summary)

* `-h` — Show options and man‑page link, then quit
* `-list-audio-devices` — List available playback devices, then quit
* `-cfgfile <path>` — Use a specific `config.xml`
* `-file <layout>` — Load LayOut Editor track data (custom routes)
* `-30` or `-60` — Force 30 or 60 fps (disables auto selection)
* `-t [1–4]` — Override hardware thread detection (1–4 threads)
* `-x` - Disable single-core RaspberryPi board detection
* `-1` - Use single-core mode (game will run in one thread (plus sound)

---

## Default Controls (keyboard)

* **Start (free‑play)** `S`, **Coin-In** `C`
* **Accelerate** `A`, **Brake** `Z`, **Low/High Gear** `G` / `H`, **Steer** ← →, **Change View** `V`
* **Menu** `M`, **Up/Down** ↑ ↓, **Select/Confirm** `S`, **Quit** `Esc`

Controls (keyboard, wheels, pads) can be remapped in *Menu → Settings → Controls*.

Quit can be remapped to `F10` instead of `Esc` in *Menu → Settings → Master Break Key*.

---

## Wheels & Gamepads

USB steering wheels, joysticks, and gamepads are supported. For wheels without SDL rumble, CannonBall‑SE can drive rumble via `/dev/hidraw` when supported by the device.

---

## Custom Music (WAV/MP3/YM)

Place audio files in `./res/` using the scheme:

```
[01–99]_Track_Display_Name.[wav|mp3|ym]
# e.g. 04_AHA_Take_On_Me.mp3
```

Indexes **01–03** replace the built‑in tracks (01 = *Magical Sound Shower*); **04+** add entries to the radio list. Use **44.1 kHz, 16‑bit stereo** audio files.

---
## OpenTelemetry Integration

CannonBall-SE includes built-in OpenTelemetry support for tracking in-game events. Telemetry is **mandatory** and requires configuration.

### Prerequisites

The OpenTelemetry C++ SDK is installed automatically by `install.sh`. Manual installation requires:
- CMake 3.20+
- libcurl
- protobuf-compiler and libprotobuf-dev
- OpenTelemetry C++ SDK v1.14.2 with OTLP HTTP exporter and logs support

### Configuration

Edit `config.xml` and set the OTLP endpoint:

```xml
<telemetry>
  <otlp_endpoint>http://localhost:4318/v1/traces</otlp_endpoint>
</telemetry>
```

**The game will exit with an error if `otlp_endpoint` is not configured.**

The logs endpoint is derived automatically by replacing `/v1/traces` with `/v1/logs` in the configured URL, so no additional configuration is needed.

### Signals

CannonBall-SE emits two complementary OpenTelemetry signals:

**Traces** (→ Tempo / Jaeger) provide session narrative and timing context:

```
game_session (root span)
├── Attributes: game_mode, music_track, player_initials
├── stage_1 (child span)
│   ├── Events: stage_started, route_chosen, crash, vehicle_overtake
│   └── Attributes: score (updated periodically), time_remaining
├── stage_2 (child span)
│   └── ...
└── stage_N (final stage)
```

**Logs** (→ Loki) provide structured, queryable records of every game event. Each log record includes `trace_id` and `span_id` for correlation back to the corresponding trace, plus `player_initials` on all in-session events.

| Event | Severity | Key Attributes |
|-------|----------|----------------|
| `game.session.start` | INFO | `game_mode`, `player_initials`, `music_selection` |
| `game.session.end` | INFO | `completion_status`, `final_score`, `final_stage` |
| `game.stage.start` | INFO | `stage_number`, `score_start`, `speed_kph` |
| `game.stage.end` | INFO | `stage_number`, `score_end`, `time_remaining_seconds` |
| `game.post_game.start` | INFO | — |
| `game.post_game.end` | INFO | — |
| `game.coin_inserted` | INFO | `credits` |
| `game.high_score` | INFO | `position`, `initials`, `score` |
| `game.route_chosen` | INFO | `direction`, `stage`, `speed_kph`, `score` |
| `game.crash` | WARN | `crash_type` (bump/spin/flip), `speed_kph`, `score`, `stage_number` |
| `game.off_road` | WARN | `speed_kph`, `score`, `stage_number` |
| `game.vehicle_overtake` | INFO | `vehicle`, `color`, `vehicle_type`, `palette`, `speed_kph`, `score`, `stage_number` |

All logs also carry `trace_id` (which doubles as a session ID), `span_id`, `service.name`, and `host.name` as resource attributes.

**Example LogQL queries (Grafana → Loki):**

```logql
# All game events
{service_name="cannonball-se"}

# Crashes only
{service_name="cannonball-se"} | json | event="game.crash"

# High-speed crashes
{service_name="cannonball-se"} | json | event="game.crash" | speed_kph > 150

# All events for a specific player
{service_name="cannonball-se"} | json | player_initials="AAA"

# All events in one game session (click trace_id in a table panel to get the value)
{service_name="cannonball-se"} | json | trace_id="<trace_id>"
```

**Coin insert events** are also recorded as standalone trace spans (not attached to game sessions) for backwards compatibility.

`vehicle_overtake` events include a `vehicle_type` ID. Each vehicle appears multiple times as it has different IDs depending on the sprite variant or colour palette used.

| ID | Vehicle  |
|----|----------|
| 0  | truck    |
| 1  | truck    |
| 2  | truck    |
| 3  | truck    |
| 4  | truck    |
| 5  | pickup   |
| 6  | pickup   |
| 7  | pickup   |
| 8  | beetle   |
| 9  | beetle   |
| 10 | beetle   |
| 11 | beetle   |
| 12 | bmw      |
| 13 | bmw      |
| 14 | corvette |
| 15 | corvette |
| 16 | porsche  |
| 17 | porsche  |
| 18 | porsche  |
| 19 | porsche  |

### Running an OTLP Collector

Example Docker Compose setup with Jaeger backend:

```yaml
version: '3'
services:
  jaeger:
    image: jaegertracing/all-in-one:latest
    ports:
      - "16686:16686"  # Jaeger UI
      - "4318:4318"    # OTLP HTTP receiver
    environment:
      - COLLECTOR_OTLP_ENABLED=true
```

Then configure `config.xml` with:
```xml
<otlp_endpoint>http://<jaeger-host>:4318/v1/traces</otlp_endpoint>
```

View traces at `http://<jaeger-host>:16686`

### Grafana Cloud

CannonBall-SE works directly with Grafana Cloud's OTLP gateway. Configure `config.xml` with your stack's endpoint and credentials:

```xml
<telemetry>
  <otlp_endpoint>https://otlp-gateway-prod-<region>.grafana.net/otlp/v1/traces</otlp_endpoint>
  <instance_id>your-instance-id</instance_id>
  <auth_token>your-grafana-cloud-token</auth_token>
</telemetry>
```

Traces are routed to **Tempo** and logs to **Loki** automatically via the same gateway and credentials. No separate log endpoint configuration is needed.

---
## Board Selection & Performance Notes

* **Pi Zero-2W** is the cheapest way to full experience with 1280x1024 screens.
* **Pi 3 Model A** is the most compact board with onboard analogue audio.

| Board        | 30 fps                                                                                                   | 60 fps                                                                                                                                                                                    | Notes                                                                                                                     |
|--------------|---------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|
| 2014 Pi B+   | Requires:<br>1. CPU @ 1 GHz<br>2. GPU @ 400 MHz<br>3. Core @ 350 MHz<br>4. Sound 22 kHz / 16 ms callbacks<br>5. Original game resolution | Not supported                                                                                                                                                                            | Game settings are automatically applied at startup. Board clocks must be set in /boot/firmware/config.txt. |
| Pi Zero (W)  | As per 2014 Pi B+.<br>CPU clock is 1 GHz by default.                                                     | Not supported                                                                                                                                                                            | As above. |
| Pi 2 (v1.1)  | Supported at stock clocks                                                                               | Supported in hi-res mode with:<br>1. GPU @ 450 MHz<br>2. Fast shader<br>3. Overlay mask<br>Boards that can be overclocked to 1.1 GHz CPU + 500 MHz GPU run full shader close to 60 fps. | 32-bit ARMv7 CPU.                                                                                                         |
| Pi 2 (v1.2)  | Supported at stock clocks                                                                               | Supported with CPU @ 1 GHz with Fast shader + overlay mask.<br>Set GPU to 400 MHz to use Full shader + shader mask.                                                                      | 64-bit ARMv8 CPU (as Pi 3). When using USB audio device, set USB to full-speed. HDMI or analogue audio preferred.          |
| Pi 3 (all)   | Supported at stock clocks                                                                               | Supported at stock clocks with Fast shader + overlay mask.<br>Set GPU to 400 MHz to use Full shader + shader mask.                                                                       | When using USB audio device, set USB to full-speed. HDMI or analogue audio preferred.                                     |
| Pi Zero 2 W  | Supported at stock clocks                                                                               | As per Pi 2 (v1.2)                                                                                                                                                                       | When using USB audio device, set USB to full-speed. HDMI audio preferred.                                                 |
| Pi 4 & 5     | Supported at stock clocks                                                                               | Supported at stock clocks                                                                                                                                                                |                                                                                                                           |

---

## Audio Troubleshooting

* **USB audio on Pi2/3/Zero-2W**: Set USB to *full‑speed* to avoid drop-outs (append `dwc_otg.speed=1` to `/boot/firmware/cmdline.txt`). Be aware this may affect some USB keyboards; BT keyboards or SSH are alternatives.
* **Callback rate**: Callback rate can be halved to 16ms, which may help some systems (*Menu → Settings → Sound/Music → Callback Rate*).

---

## Watchdog

On hardware with a watchdog (all Raspberry Pi boards), the game integrates with it so the OS will auto‑reboot on hang (e.g., aggressive overclocks).

---

## License

* **Upstream CannonBall license**: non‑commercial use; modified redistributions must include full source; warranty disclaimer. See `license.txt` in the repo.
* **CannonBall‑SE additional terms**: this fork’s enhancements © 2020–2025 James Pearce; provided "as is"; not for sale/monetisation; preserve notices. See `CannonBall-SE-license.txt`.
* **Third‑party notices**: includes Blargg’s `snes_ntsc` under **LGPL‑2.1**; if statically linking, provide relinkable objects or equivalent. See `THIRD-PARTY-NOTICES.md` and `licenses/`.

*OutRun is a trademark of SEGA Corporation. This project is not affiliated with SEGA.*

---

## Credits

* Original engine by **Chris White** and contributors.
* NTSC Filter library by **Shay Green** ('Blargg').

---

## See Also

* Man page: `docs/cannonball-se.6` (accessible via `man -l ~/cannonball-se/docs/cannonball-se`
