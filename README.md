# CannonBall-SE

*A fork of Chris White's incredible OutRun engine, CannonBall, with enhancements.*

Built for home-made arcade cabinets: an authentic CRT look on a modern LCD, with modest
hardware requirements. Anything from a Raspberry Pi Zero will run it.

![CannonBall-SE Start Line](screenshots/CannonBall-SE-Startline.jpg)

> **Windows executable available [in Releases](https://github.com/J1mbo/cannonball-se/releases/latest).**

---

## What it does

Modern LCDs render OutRun's low-res graphics as oversized, razor-sharp pixels, which makes the
game feel clinical. CannonBall-SE puts the picture through a two-stage filter — a
SIMD-accelerated NTSC filter on the CPU, then a CRT shader on the GPU — to give back the
softness, colour bleed, screen curvature, shadow mask and gentle analogue noise of an arcade
monitor. Everything is configurable, and it all runs fast enough to hold 60fps on a Raspberry
Pi.

Beyond the look:

- **Runs at full speed on modest hardware.** Multi-threaded engine with automatic frame-rate
  control, so the game never runs slow — it drops frames instead.
- **Rewritten audio module.** Arcade-accurate playback speed, no stutters or sync pops, and
  custom music from WAV, MP3 or YM files.
- **Built for cabinets.** Rumble on skidding, force feedback for steering wheels, machine-hours
  and play-count tracking, and hardware watchdog integration so a hung cabinet reboots itself.
- **Gameplay fixes.** Consistent wheel slip through stage transitions, no phantom cars near
  checkpoints, plus all the fixes from Chris White's original.

See [docs/SCREEN-FILTERS.md](docs/SCREEN-FILTERS.md) for what the filter pipeline actually does.

---

## Performance at a glance

**60fps on every quad-core Raspberry Pi. 30fps on the single-core boards.** On the slower
quad-core boards you may need the fast shader, or a GPU clock bump, to get there.

| Tier | Boards | 30fps | 60fps |
|---|---|---|---|
| Single-core | Pi 1, Pi Zero (W), 2014 Pi B+ | Yes, with a clock bump | No |
| Quad-core, 32-bit | Pi 2 v1.1 | Yes, stock | Fast shader + GPU at 450MHz |
| Quad-core, 64-bit | Pi 2 v1.2, Pi 3, Pi Zero 2 W | Yes, stock | Fast shader; full shader needs GPU at 400MHz |
| Quad-core, fast | Pi 4, Pi 5 | Yes, stock | Yes, stock, either shader |
| PC | Anything that runs Windows 11 or Ubuntu | Yes | Yes |

Assumes a 1280x1024 screen with hi-res mode on. Run `build/cannonball-se -perftest` to measure
what your own board sustains.

**If you are not hitting your target frame rate**, the settings to change are `video.hires`
(CPU) and `video.shader_mode` (GPU), in that order.
[docs/PERFORMANCE-TUNING.md](docs/PERFORMANCE-TUNING.md) has the full guide: which lever to pull,
how to tell whether you are CPU- or GPU-limited, and the clock recipes.

---

## Supported platforms

- **All Raspberry Pi boards** running Raspberry Pi OS **Bookworm or newer**. Bullseye ships GCC 10
  and cannot build this — the project needs GCC 12+.
- **x86/x64 PCs** running Ubuntu 24.04+ or Windows 11.

A desktop environment is not required on Linux; the Lite image works fine.

---

## ROMs (required)

CannonBall-SE needs a copy of the original **OutRun revision B** ROM set, which is not included.
Copy the ROM files into the `roms/` directory. See [roms/roms.txt](roms/roms.txt) for the file
names.

You are expected to legally own the original ROMs. Their use may be restricted by copyright or
other law depending on your jurisdiction.

---

## Quick start (Linux / Raspberry Pi)

```bash
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se
./install.sh          # installs dependencies, builds, sets permissions
# copy your OutRun rev B ROMs into ./roms
build/cannonball-se
```

`install.sh` is interactive: it asks for confirmation up front, prompts for `sudo`, and asks
which audio device to use at the end. It also adds you to the `input` and `audio` groups and
installs a udev rule, so **reboot before the first run**. The build takes a few minutes on a
Pi 4 or a PC, and 40+ minutes on a Pi Zero or Pi 1 — but needs no attention.

Running under a desktop? Wayland is noticeably faster:
`SDL_VIDEODRIVER=wayland build/cannonball-se`

Full details, including a manual build without the script:
[docs/INSTALL.md](docs/INSTALL.md).

## Quick start (Windows)

Download the executable from
[Releases](https://github.com/J1mbo/cannonball-se/releases/latest), or build it yourself: install
**Visual Studio 2026** with the "Desktop development with C++" workload, clone this repository
from within Visual Studio, and let the CMake integration do the rest. Step-by-step instructions
are in [docs/INSTALL.md](docs/INSTALL.md).

---

## Controls

**Keyboard defaults:** Start `S`, Coin `C`, Accelerate `A`, Brake `Z`, Gears `G` / `H`,
Steer ← →, Change view `V`, Menu `M`, Select `S`, Quit `Esc`.

**Function keys** (not remappable): `F1` pause, `F2` frame step, `F3` timer, `F5` menu,
`F7` hi-res sprites, `F8` toggle all video processing, `F9` toggle shadow mask.

`F8` is the quickest way to see what the filters are doing.

Everything else is remappable in *Menu → Settings → Controls*, and quit can be moved from `Esc`
to `F10`.

**Wheels and gamepads:** USB steering wheels, joysticks and gamepads are supported. For wheels
without SDL rumble, the game can drive rumble via `/dev/hidraw`.

---

## Custom music

Put WAV, MP3 or YM files in `res/`, named:

```
[01-99]_Track_Display_Name.[wav|mp3|ym]      e.g. 04_AHA_Take_On_Me.mp3
```

Indexes **01-03** replace the built-in tracks (01 = *Magical Sound Shower*); **04+** add new
entries to the radio list. Use 44.1kHz, 16-bit stereo.

---

## Documentation

| | |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Building on Linux, Raspberry Pi and Windows |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Every setting in `config.xml` |
| [docs/PERFORMANCE-TUNING.md](docs/PERFORMANCE-TUNING.md) | Board-by-board frame rate guide |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Audio, controller and video problems |
| [docs/SCREEN-FILTERS.md](docs/SCREEN-FILTERS.md) | What the CRT filter pipeline does |
| [docs/cannonball-se.6](docs/cannonball-se.6) | Man page — `man -l docs/cannonball-se.6` |
| [CHANGELOG](CHANGELOG) | Release history |

Developer-facing engineering notes live under [tools/](tools/).

---

## Licence

- **Upstream CannonBall licence** — non-commercial use; modified redistributions must include
  full source; warranty disclaimer. See [docs/license.txt](docs/license.txt).
- **CannonBall-SE additional terms** — this fork's enhancements © 2020-2025 James Pearce;
  provided "as is"; not for sale or monetisation; preserve notices. See
  [docs/CannonBall-SE-license.txt](docs/CannonBall-SE-license.txt).
- **Third-party components** — Blargg's `snes_ntsc` (LGPL-2.1), MAME's YM2151 core and
  Atari800's sound emulation (GPL-2.0+), and others. See
  [docs/THIRD-PARTY-NOTICES.md](docs/THIRD-PARTY-NOTICES.md).

*OutRun is a trademark of SEGA Corporation. This project is not affiliated with SEGA.*

---

## Credits

- Original engine by **Chris White** and contributors —
  [github.com/djyt/cannonball](https://github.com/djyt/cannonball)
- NTSC filter library by **Shay Green** ('Blargg')

Bugs and suggestions: <https://github.com/J1mbo/cannonball-se/issues>
