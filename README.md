# CannonBall‑SE

*An easy-to-use fork of Chris White’s CannonBall OutRun engine, with enhanced graphics and cabinet‑friendly enhancements.*

---

## Overview

CannonBall‑SE is a maintained fork of the cross‑platform OutRun engine, focused on an authentic arcade‑cab experience. Graphics seem more natural thanks to the video filter, subtle screen curvature, shadow‑mask effects, attention to accurate colour reproduction, subtle noise and desaturation, and automatic 30/60fps selection.

Authentic game audio is ensured with a re-written audio module, and this also supports MP3/WAV custom music.

Gameplay is refined with additional rumble and various performance and stability fixes (it also hooks a hardware watchdog where available to ensure reliable continuous operation). Play count and “machine hours” are also tracked.

> **Note:** SDL\_gpu is **no longer required**.

---

## Supported Platforms

* **Raspberry Pi (Pi 2/3/4/5/Zero-2W)** running Raspberry Pi OS.
* **x86/x64 PCs** (Intel/AMD with SSE4) running a recent Linux distribution (e.g., Ubuntu).

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

# 4) Run
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

---

## Default Controls (keyboard)

* **Start (free‑play)** `S`, **Coin-In** `C`
* **Accelerate** `A`, **Brake** `Z`, **Low/High Gear** `G` / `H`, **Steer** ← →, **Change View** `V`
* **Menu** `M`, **Up/Down** ↑ ↓, **Select/Confirm** `S`, **Quit** `Esc`

Controls (keyboard, wheels, pads) can be remapped in *Menu → Settings → Controls*.

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

* **Pi4** is a great sweet spot for 60 fps with the full shader at 1080p.
* **Pi2 (v1.2)/Pi3/Zero-2W** can reach 60 fps with the “Fast” shader (and with the "Full" shader when the GPU is overclocked to 400MHz)
* **Pi2 (v1.1)** is typically locked at 30 fps unless heavily overclocked.
* Higher‑DPI displays need more power - Pi5 or Intel/AMD.

---

## Audio Troubleshooting (Pi)

* **USB audio on Pi2/3/Zero-2W**: Set USB to *full‑speed* to avoid drop-outs (append `dwc_otg.speed=1` to `/boot/firmware/cmdline.txt`). Be aware this may affect some USB keyboards; BT keyboards or SSH are alternatives.
* **Callback rate**: On slower setups using analog output (Pi2 especially), switch the music callback from **8ms** to **16ms** (*Menu → Settings → Sound/Music → Callback Rate*).

---

## Watchdog

On hardware with a watchdog (all Raspberry Pi boards), the game integrates with it so the OS will auto‑reboot on hang (e.g., aggressive overclocks).

---

## License

* **Upstream CannonBall license**: non‑commercial use; modified redistributions must include full source; warranty disclaimer. See `license.txt` in the repo.
* **CannonBall‑SE additional terms**: this fork’s enhancements © 2020–2025 James Pearce; provided “as is”; not for sale/monetisation; preserve notices. See `CannonBall-SE-license.txt`.
* **Third‑party notices**: includes Blargg’s `snes_ntsc` under **LGPL‑2.1**; if statically linking, provide relinkable objects or equivalent. See `THIRD-PARTY-NOTICES.md` and `licenses/`.

*OutRun is a trademark of SEGA Corporation. This project is not affiliated with SEGA.*

---

## Credits

* Original engine by **Chris White** and contributors.
* CannonBall‑SE by **James Pearce**.
* NTSC Filter library by **Shay Green** ('Blargg').

---

## See Also

* Man page: `docs/cannonball-se.6` (accessible via `man -l ~/cannonball-se/docs/cannonball-se`
