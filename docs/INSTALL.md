# Installing and Building CannonBall-SE

This is the complete build-and-install guide for CannonBall-SE. If you just want the short
version, see the Quick Start in the main [README](../README.md); this document covers the
detail, the platform-specific steps, and what to do if something goes wrong.

Contents:

- [Requirements](#requirements)
- [ROMs](#roms)
- [Linux / Raspberry Pi](#linux--raspberry-pi)
- [Windows](#windows)
- [First run](#first-run)
- [Build options](#build-options)
- [If the build fails](#if-the-build-fails)

---

## Requirements

CannonBall-SE is written in C++23 and CMake enforces a minimum compiler version at configure
time. If your compiler is older than the versions below, `cmake` will stop with a
`FATAL_ERROR` rather than attempting to build:

| Compiler | Minimum version |
|----------|------------------|
| GCC      | 12               |
| Clang    | 15               |
| MSVC     | 19.36 (Visual Studio 2022 version 17.6, or Visual Studio 2026) |

**This is the most likely reason a build fails on a Raspberry Pi, so check it first.**
Raspberry Pi OS's default compiler tracks the underlying Debian release:

- **Raspberry Pi OS Bullseye** (Debian 11) ships **GCC 10**. This is below the floor — CannonBall-SE
  **cannot** be built on Bullseye. `cmake` will fail immediately with `C++23 support requires GCC 12+`.
- **Raspberry Pi OS Bookworm** (Debian 12) ships **GCC 12**, which meets the floor exactly. This
  (or newer) is what you need. If you're not sure which you have, run `cat /etc/os-release` — if
  it says `bullseye`, re-flash the SD card with the current Raspberry Pi OS image using Raspberry
  Pi Imager before doing anything else.

The same applies to Ubuntu: **Ubuntu 22.04 LTS ships GCC 11**, which also fails the check.
**Ubuntu 24.04 LTS ships GCC 13**, which is fine. `install.sh` (see below) explicitly targets
Ubuntu 24.04 and Raspberry Pi OS for this reason; other releases are not tested.

On Windows, the MSVC floor of 19.36 corresponds to Visual Studio 2022 17.6 or later. In
practice, use **Visual Studio 2026** — see [Windows](#windows) below for why.

Supported platforms: any Raspberry Pi board running Raspberry Pi OS (a desktop is not
required — the command-line/Lite image works), and x86/x64 PCs running Ubuntu 24.04 or
Windows 11.

---

## ROMs

CannonBall-SE needs a copy of the original **OutRun revision B** arcade ROM set to run — it
does not ship with any ROM data. You are expected to legally own the original ROMs; use of ROM
images may be restricted by copyright or other law depending on your jurisdiction, and you take
full responsibility for how you obtain and use them.

Copy the ROM files into the `roms/` directory inside your CannonBall-SE checkout (for example
`~/cannonball-se/roms` if you cloned the repository into your home directory, as the examples
below do). The required file names are listed in [`roms/roms.txt`](../roms/roms.txt); at the
time of writing they are:

```
epr-10187.88     epr-10327a.76    epr-10328a.75    epr-10329a.58    epr-10330a.57
epr-10380b.133   epr-10381a.132   epr-10382b.118   epr-10383b.117
mpr-10371.9      mpr-10372.13     mpr-10373.10     mpr-10374.14     mpr-10375.11
mpr-10376.15     mpr-10377.12     mpr-10378.16
opr-10185.11     opr-10186.47     opr-10188.71     opr-10189.70     opr-10190.69
opr-10191.68     opr-10192.67     opr-10193.66
opr-10230.104    opr-10231.103    opr-10232.102    opr-10266.101    opr-10267.100  opr-10268.99
```

`roms/roms.txt` also lists optional files: an alternate set for the Japanese release (different
track order), and an optional "fixed audio" ROM (`opr-10188.71f`). Neither is required for the
game to run.

---

## Linux / Raspberry Pi

### Automated install (recommended)

`install.sh`, in the root of the repository, automates the whole process. It is interactive —
it asks for confirmation up front and for your audio device near the end — and it **changes
system configuration** (user groups, a udev rule, and a PAM limits file), not just the project
directory, so it's worth knowing exactly what it does before you run it:

```bash
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se
chmod +x install.sh
./install.sh
```

Step by step, `install.sh`:

1. **Asks for confirmation**: `Do you wish to continue (you will be prompted for sudo)? [y/N]`.
   Answering anything other than y/Y aborts with no changes made.
2. **Picks a build parallelism** automatically from available RAM (1 thread up to 460 MB, 2 up
   to 920 MB, 3 up to 1380 MB, otherwise 4). No prompt for this.
3. **Runs `sudo apt update -y`.**
4. **Adds your user to the `input` group** (`sudo usermod -aG input "$USER"`), for access to
   input devices used by wheels/pads.
5. **Installs a udev rule**, `/etc/udev/rules.d/99-cannonball.rules`, granting world read/write
   (`MODE="0666"`) on `hidraw*` (needed for force-feedback rumble on some wheels) and
   `watchdog*` devices, then reloads udev rules.
6. **Adds your user to the `audio` group and installs a PAM limits file**
   (`/etc/security/limits.d/50-cannonball-audio.conf`, copied from `res/50-cannonball-audio.conf`)
   granting the `audio` group real-time scheduling priority up to 80, `nice -20`, and unlimited
   `memlock`. This only matters if you later turn on `<rt_priority>1</rt_priority>` in
   `config.xml` (it's `0`/off by default) — without the grant, the game would silently fall back
   to normal scheduling for its audio threads.
7. **Installs build dependencies via apt**:
   `build-essential git cmake libsdl2-dev libglu1-mesa-dev libmpg123-dev pkg-config alsa-utils libtinyxml2-dev`.
8. **Builds the project**: creates `build/` and `roms/`, runs `cmake -S . -B build`, then
   `cmake --build build --parallel <N>` using the thread count from step 2.
9. **Creates a default `config.xml`** by copying `res/config.xml` into the repository root.
10. **Lists audio devices and asks you to pick one**: it runs
    `build/cannonball-se -list-audio-devices`, then prompts
    `Enter the number of the audio device to use for CannonBall [0]:`, and writes your answer
    into `config.xml`'s `<playback_device>` element.
11. **Prints a summary** of every step and its status.
12. **Offers to open the man page**: `Would you like to view the man page now? [y/N]` — if yes,
    runs `man -l docs/cannonball-se.6`.
13. Finishes with a reminder that you must **reboot** before running the game (the `input`/`audio`
    group membership changes only take effect on your next login), and that you still need to
    copy the ROMs into `roms/` if you haven't already.

`install.sh` does **not** set up any systemd service or auto-start — it only builds the game and
prepares device permissions. Nothing outside `apt`, the two files above, and the two group
memberships is touched.

After it finishes and you've rebooted, with the ROMs in place:

```bash
cd ~/cannonball-se
build/cannonball-se
```

### Manual build (without the script)

If you'd rather not hand a script `sudo` access, or you're on a distribution `install.sh`
doesn't target, you can do the equivalent steps yourself. This gets you a working build; the
udev/group/PAM steps above are only needed for hidraw rumble, watchdog integration, and
real-time audio scheduling — the game runs and plays fine without them.

```bash
# Install dependencies (same list install.sh uses)
sudo apt update
sudo apt install -y build-essential git cmake libsdl2-dev libglu1-mesa-dev \
  libmpg123-dev pkg-config alsa-utils libtinyxml2-dev

# Fetch and build
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se
mkdir -p build roms
cmake -S . -B build
cmake --build build --parallel $(nproc)

# Default config file
cp res/config.xml .

# Copy your ROMs into ./roms, then run:
build/cannonball-se
```

Check your compiler version first if you're on a Pi (`g++ --version`) — see
[Requirements](#requirements) above.

### Build time

Not independently benchmarked for this document, so treat these as **approximate**, per the
project's own guidance: a Pi 4, Pi 5, or an Intel/AMD machine should build in a few minutes; a
Pi Zero (original) or Pi 1 can take 40 minutes or more, but needs no input once started, so it's
fine to leave running.

### Running under a desktop session

If you're running a Raspberry Pi desktop (rather than the CLI/Lite image), starting under
Wayland instead of the default X11 session tends to give a better frame rate:

```bash
SDL_VIDEODRIVER=wayland build/cannonball-se
```

---

## Windows

CannonBall-SE compiles under Windows via the CMake integration built into Visual Studio; most of
the process is automated.

1. Install **Visual Studio 2026**, making sure to select **"Desktop development with C++"**
   under **Workloads** in the installer. (The project's `CMakePresets.json` pins the
   `Visual Studio 18 2026` generator, so this is the version to use for the one-click flow
   described here; a plain command-line build with an older toolchain that still meets the
   MSVC 19.36+ / VS2022 17.6+ floor has not been verified against these presets.)

2. Open Visual Studio, choose **"Clone a Repository"**, and enter
   `https://github.com/J1mbo/CannonBall-SE`. Note the path shown in the "Path" box (it defaults
   to `C:\Users\[username]\Source\Repos`). Click **Clone** — Visual Studio downloads the source
   directly from GitHub.

3. In the **Solution Explorer** pane, Visual Studio should show "Folder View" at the cloned
   path. Double-click it to start CMake configuration.

   This step also resolves the project's dependencies (SDL2, TinyXML2, mpg123, ANGLE — see
   `vcpkg.json`) via Visual Studio's built-in vcpkg integration, and can take a long time — around
   20 minutes on some machines. Visual Studio may misleadingly show "Ready" in the bottom-left
   corner while CMake is still running; check **View → Output → CMake** for progress.

   If Visual Studio fails to start CMake correctly on first open (no CMake output, or an error
   such as "Root element is missing"), close and reopen Visual Studio on the same folder.

   The process isn't finished until the Output window shows:

   ```
   1> CMake generation finished.
   ```

4. While CMake runs, copy your ROMs into the `roms` folder under the cloned project path, e.g.
   `C:\Users\[username]\Source\Repos\CannonBall-SE\roms\`.

5. Once CMake has finished, make sure **windows-release** is selected in the configuration
   drop-down next to "Local Machine", then click the green play button beside it to build.

6. The build itself doesn't take long, and the game should start automatically afterwards.

To build a debug configuration instead, switch the centre drop-down to **windows-debug** and the
target drop-down to `cannonball-se.exe (Debug\cannonball-se.exe)`, then click the green play
button again.

A pre-built Windows executable is also published under
[Releases](https://github.com/J1mbo/cannonball-se/releases) if you'd rather skip building it
yourself.

---

## First run

From the directory you built in (Linux) or after the build completes (Windows), with your ROMs
in place:

```bash
build/cannonball-se
```

On a Raspberry Pi or Linux desktop session, starting under Wayland can improve frame rate:

```bash
SDL_VIDEODRIVER=wayland build/cannonball-se
```

If you want to know whether your board has headroom for a heavier shader or hi-res mode before
tuning settings, run the built-in benchmark and quit:

```bash
build/cannonball-se -perftest
```

If you get no audio, check the output device order hasn't changed with
`build/cannonball-se -list-audio-devices`, and check the volume isn't at zero (`alsamixer` on
Linux).

---

## Build options

These are CMake options a user might reasonably want to set, e.g.
`cmake -S . -B build -DWITH_MARCH_NATIVE=OFF`.

| Option | Default | Effect |
|--------|---------|--------|
| `WITH_MARCH_NATIVE` | `ON` | Builds with `-march=native`, tuned to the exact CPU doing the build. Turn this **off** if you intend to copy the resulting binary to a different machine/board — a `-march=native` binary built on one Pi model is not guaranteed to run on another (e.g. a Pi 5 / Cortex-A76 build can `SIGILL` on a Pi 3 / Cortex-A53). This is why the install workflow above builds on each target board rather than copying a binary between them; it would become a blocker if prebuilt Linux binaries were ever distributed, since that would need separate per-tier `-mcpu`/`-march` builds. |
| `PIXEL_ACCURACY` | `ON` | Enables pixel-accurate sprite rendering (reproduces a System 16 hardware sprite-glow quirk). Can also be toggled at runtime with `-glow`/`-noglow`. |
| `WITH_MP3` | `ON` | Enables MP3 playback via `mpg123`, for custom music. Falls back to a warning (not a hard failure) if `mpg123` isn't found. |
| `QUIET_BUILD` | `ON` | Reduces build log noise (suppresses per-file "Building CXX object..." messages and a handful of common warnings). |
| `SUPER_QUIET` | `OFF` | Silences *all* compiler warnings in Release-like configs. |
| `INSTALL_MANPAGE` | `ON` | Installs the `cannonball-se(6)` man page under `cmake --install` (Linux only). |
| `WITH_NEON32` | `OFF` | Forces `-mfpu=neon-vfpv4` on 32-bit ARMv7 when *not* using `-march=native` — only relevant if cross-compiling or otherwise building without native tuning on a NEON-capable 32-bit board. |
| `WITH_RVV` | `OFF` | Enables RISC-V Vector (RVV) SIMD on `riscv64` targets. |

Dev/diagnostic-only options (not needed to play the game, briefly noted for completeness):
`ENABLE_PROFILING`, `PROFILE_WITH_PERF`, `PROFILE_WITH_GPROF`, `PROFILE_WITH_CALLGRIND`,
`PROFILE_WITH_GPERFTOOLS` (profiling build flags), and `SPRITE_DUMP` / `SPRITE_REF_PX` (sprite
renderer test aids — `SPRITE_DUMP` enables the `-dumpspr` capture flag documented in the man
page).

---

## If the build fails

Most build failures fall into one of three categories:

1. **Compiler too old.** `cmake` fails with `C++23 support requires GCC 12+` (or the Clang/MSVC
   equivalent). Check your version with `g++ --version`. On a Raspberry Pi, this almost always
   means you're on Bullseye — see [Requirements](#requirements); re-flash with a current
   (Bookworm or newer) Raspberry Pi OS image. On Ubuntu, it means you're on 22.04 or older;
   upgrade to 24.04 or newer.

2. **Missing development packages.** If you followed the manual build path and skipped a
   package, `cmake -S . -B build` will fail with a `pkg-config`/`find_package` error naming the
   missing module (SDL2, TinyXML2, EGL, GLESv2, or mpg123). Re-run the `apt install` line from
   [Manual build](#manual-build-without-the-script) above — it's the exact list `install.sh`
   uses. `EGL`/`GLESv2` and `libudev` are pulled in via other packages on Raspberry Pi OS/Ubuntu
   desktop images in the cases this has been tested; if `pkg-config` can't find `egl` or
   `glesv2` on a minimal/server install, you'll need to add the relevant Mesa EGL/GLES
   development package for your distribution (not independently verified here — check the exact
   package name for your OS release).

3. **Missing ROMs.** The project builds and starts fine without them, but the game itself won't
   run without a valid OutRun revision B set in `roms/`. See [ROMs](#roms) above and check your
   file names exactly match `roms/roms.txt`.

If none of these explain it, run `cmake -S . -B build` on its own first and read the last few
lines of output — it will usually name the missing dependency directly.
