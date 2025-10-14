# CannonBall‑SE

*A fork of Chris White's incredible Outrun engine, CannonBall, with enhancements.*
Designed with home-made cabinets in mind, gives Outrun enthusiasts an improved experience on LCD screens
with minimal hardware requirements - anything from a RaspberryPi Zero will work!

> **Note:** Windows executable available for download [in Releases](https://github.com/J1mbo/cannonball-se/releases/tag/v1.3)

![CannonBall-SE Start Line](screenshots/CannononBall-SE-Startline.jpg)

---

## Overview

CannonBall‑SE trys to create what feels like an authentic arcade‑cab experience on modern hardware.
The game graphics seem more natural thanks to processing with a fully configurable 2-stage video filter
providing effects including NTSC filter, screen curvature, shadow‑mask effect, a hint of analogue noise,
vignette and desaturation.

Additionally:

- Gameplay bug fixes:
  - Random cars no longer appear near checkpoints momentarily
  - Wheel slip when cornering works consistently through stage transitions
  - Plus: all the fixes available in Chris White's original

- Gameplay enhancements:
  - Rumble is generated when skidding
  - Auto frame-rate control ensures game always runs at full speed
  - Multi-threaded engine - works at 60fps on a Pi3 or Zero2W in hi-res mode

- Re-written audio module:
  - Tightens up system stability, especially on low-spec RaspberryPi systems
  - Eliminates stutters
  - Provides arcade-accurate playback speed
  - Supports MP3 as well as WAV custom music (with playback volume control).

- Tracks 'machine hours' and games played.

- Hooks system watchdog on RaspberryPi boards for reliable continuous operation (e.g. in cabinets)

- Performance improvements and stability fixes.


---

## Supported Platforms

* **All RaspberryPi Boards** running RaspberryPi OS.
* **x86/x64 PCs** (Intel/AMD) running Ubuntu or Windows 11.

For Linux, a desktop is not required - use the command-line version of the OS. Compiling is fully automated
on both platforms.


---

## ROMs (Required)

CannonBall‑SE requires a copy of the original **OutRun revision B** ROM set. Copy the ROMs into the project’s `roms/` directory. You are expected to legally own the original ROMs; usage may be restricted by local law.

---

## Quick Start Guide (Linux)

Getting going on RaspberryPi systems or Ubuntu is super-easy - just run the included script `install.sh`. It will install
prerequisites, build the project, and set device permissions automatically. On a Pi4 or Intel machine, this will only take
a few minutes (the process takes 40+ minutes on a Pi Zero or Pi 1, but requires no human input).

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

The script installs system packages, compiles the game (optimised for the system it's building on) with CMake
into `./build/`, and applies permissions so it can access `/dev/watchdog` and `/dev/hidraw` for rumble.

---

## Quick Start Guide (Windows)

A Windows executable is available [in Releases](https://github.com/J1mbo/cannonball-se/releases/tag/v1.3).
CannonBall-SE can also be compiled easily with Visual Studio Community Edition as the process is fully
automated. Please see `docs/Compiling-On-Windows.txt`.

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

## Board Selection & Performance Notes

* **Pi Zero-2W** is the cheapest way to full experience with 1280x1024 screens.
* **Pi 3 Model A** is the most compact board with onboard analogue audio.

| Board        | 30 fps                                                | 60 fps                                                                                                                                                                                    | Notes                                                                                                                     |
|--------------|-------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|
| 2014 Pi B+   | Requires: CPU @ 1GHz, GPU @ 400MHz, Core @ 350MHz.    | Not supported                                                                                                                                                                            | Game settings are automatically applied at startup. Board clocks must be set in /boot/firmware/config.txt. |
| Pi Zero (W)  | As per 2014 Pi B+.<br>CPU clock is 1GHz by default.   | Not supported                                                                                                                                                                            | As above. |
| Pi 2 (v1.1)  | Supported at stock clocks                             | Supported in hi-res mode with GPU @ 450MHz & Fast shader. Boards overclocked to 1.1 GHz CPU & 500MHz GPU can run full shader close to 60 fps. | 32-bit ARMv7 CPU.                                                                                                         |
| Pi 2 (v1.2)  | Supported at stock clocks                             | Supported with CPU @ 1 GHz with Fast shader. Set GPU to 400 MHz to use Full shader.                                                                      | 64-bit ARMv8 CPU (as Pi 3). When using USB audio device, set USB to full-speed. HDMI or analogue audio preferred.          |
| Pi 3 (all)   | Supported at stock clocks                             | Supported at stock clocks with Fast shader. Set GPU to 400MHz to use Full shader.                                                                       | When using USB audio device, set USB to full-speed. HDMI or analogue audio preferred.                                     |
| Pi Zero 2 W  | Supported at stock clocks                             | As per Pi 2 (v1.2)                                                                                                                                                                       | When using USB audio device, set USB to full-speed. HDMI audio preferred.                                                 |
| Pi 4 & 5     | Supported at stock clocks                             | Supported at stock clocks                                                                                                                                                                |                                                                                                                           |

For those running on Windows, just about any PC capable of running Windows 11 should work fine.

---

## Audio Troubleshooting on Linux

* **USB audio on Pi2/3/Zero-2W**: Set USB to *full‑speed* to avoid drop-outs (append `dwc_otg.speed=1` to `/boot/firmware/cmdline.txt`). Be aware this may affect some USB keyboards; BT keyboards or SSH are alternatives.
* **Callback rate**: Callback rate can be halved to 16ms, which may help some systems (*Menu → Settings → Sound/Music → Callback Rate*).

---

## Watchdog (Linux only)

On hardware with a watchdog (all RaspberryPi boards), the game integrates with it so the OS will auto‑reboot on hang (e.g., aggressive overclocks).

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
* Windows compiling guide: `docs/Compiling-On-Windows.txt`
