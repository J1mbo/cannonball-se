# CannonBall‑SE

*An easy-to-use fork of Chris White’s CannonBall OutRun engine, with enhanced graphics and cabinet‑friendly enhancements.*

---

## Overview

CannonBall‑SE is a fork of the cross‑platform OutRun engine, focused on an authentic arcade‑cab experience.

* Graphics seem more natural thanks to the video filter, subtle screen curvature, shadow‑mask effects, attention to accurate colour reproduction, subtle noise and desaturation, and automatic 30/60fps selection.
* The re-written audio module tightens up stability, ensures accurate playback speed, and also supports MP3/WAV custom music.
* Gameplay is refined with various game bug fixes, additional rumble, performance improvements and stability fixes.
* The hardware watchdog is used (if available) to ensure reliable continuous operation.
* Play count and "machine hours" are tracked.

> **Note:** Can now be used on all Raspberry Pi systems including the Pi Zero.

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

Use the included script 'install.sh' to install prerequisites, build the project, and set device permissions automatically.

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

The script installs system packages, configures the linker as needed, compiles the game (optimised for the system it's building on) with CMake into `./build/`, and applies permissions so it can access `/dev/watchdog` and `/dev/hidraw` for rumble.

---

## Command‑line Options (summary)

* `-h` — Show options and man‑page link, then quit
* `-list-audio-devices` — List available playback devices, then quit
* `-cfgfile <path>` — Use a specific `config.xml`
* `-file <layout>` — Load LayOut Editor track data (custom routes)
* `-30` or `-60` — Force 30 or 60 fps (disables auto selection)
* `-t [1–4]` — Override hardware thread detection (1–4 threads)
* `-x` - Disable single-core RaspberryPi board detection
* `-1` - Use single-core mode (game with run in one thread (plus sound)

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

## Performance Notes

The **Pi4** can easily provide 60 fps with the full shader at 1080p. Higher‑DPI displays will need more power - Pi5 or Intel/AMD.

* **Pi2 (v1.2)/Pi3/Zero-2W** can reach 60 fps (use "Fast" shader, or "Full" shader if GPU is overclocked to 400MHz)
* **Pi2 (v1.1)** can run at 30fps in hires or 60fps in standard res ("Full" shader). Overclocked (1050MHz CPU, 450MHz GPU) can reach 60 fps in high-res with "Fast" shader.

**Single-Core Pi Models (Pi-Zero(W), Pi B+)**

The following settings are automatically applied at launch:

* The game engine runs in standard resolution mode (same as the original arcade)
* The Blarrg filter is disabled.
* The "Full" shader is used.

HDMI audio is lightest as it's handled by the GPU, so is best on Pi-Zero(W). USB audio requires CPU so may reduce frame rate.

HDMI audio on the original Pi boards (e.g. B+) can be problematic (and requires fkms video driver); use on-board analogue audio on these boards. Running at 1GHz CPU and 350MHz GPU, 30fps is nevertheless achievable. 

---

## Audio Troubleshooting (Pi)

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
