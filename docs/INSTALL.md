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
epr-10380b.133   epr-10381b.132   epr-10382b.118   epr-10383b.117
mpr-10371.9      mpr-10372.13     mpr-10373.10     mpr-10374.14     mpr-10375.11
mpr-10376.15     mpr-10377.12     mpr-10378.16
opr-10185.11     opr-10186.47     opr-10188.71     opr-10189.70     opr-10190.69
opr-10191.68     opr-10192.67     opr-10193.66
opr-10230.104    opr-10231.103    opr-10232.102    opr-10266.101    opr-10267.100  opr-10268.99
```

Instead of loose files, you can drop a MAME-format ROM archive (a merged `outrun.zip`, or a
split set) straight into `roms/` — it does not need to be extracted. ROMs inside the archive are
matched by CRC32 and size rather than by filename, so subdirectories, renamed files and split
sets all work; any loose file already present in `roms/` still takes priority over the same ROM
found in an archive. `data.rompath` (see [CONFIGURATION.md](CONFIGURATION.md)) can also point
directly at a `.zip` file instead of a directory. Outside `roms/`, only archives using one of the
MAME set names are picked up — `outrun.zip`, `outrunra.zip`, `outrundx.zip`, `outrundxa.zip`,
`outrundxj.zip`, `outruneh.zip`, `outruneha.zip`, `outrundxeh.zip`, `outrundxeha.zip`,
`outrunb.zip` (case-insensitive) — searched first in the current working directory, then in the
directory containing the executable.

`roms/roms.txt` also lists optional files: an alternate set for the Japanese release (different
track order — these are the main/sub CPU ROMs of MAME's `outrundx` set, included in a merged
`outrun.zip`), and an optional "fixed audio" ROM (`opr-10188.71f`). Neither is required for the
game to run.

---

## Linux / Raspberry Pi

### Automated install (recommended)

`install.sh`, in the root of the repository, automates the whole process. It is interactive —
it asks for confirmation up front, and on Raspberry Pi OS whether to set up appliance mode — and
it **changes system configuration** (user groups, a udev rule, and a PAM limits file, plus, if
you opt into appliance mode, a boot splash theme, boot options and a systemd service), not just
the project directory, so it's worth knowing exactly what it does before you run it:

```bash
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se
chmod +x install.sh
./install.sh
```

Step by step, `install.sh`:

1. **Asks for confirmation**: `Do you wish to continue (you will be prompted for sudo)? [y/N]`.
   Answering anything other than y/Y aborts with no changes made.
2. **On Raspberry Pi OS only, offers appliance mode**:
   `Enable appliance mode (boot straight into the game, power off on quit)? [y/N]`. This prompt
   is skipped entirely on Ubuntu. Answering yes then asks
   `Splash image PNG [res/appliance/splash.png]:` for the image to show at boot and shutdown.
   See [Appliance mode](#appliance-mode) below for what this turns on.
3. **On Raspberry Pi 2 and Pi 3-class boards (Cortex-A7 or Cortex-A53), offers a recommended
   overclock**, independently of appliance mode: `Apply recommended overclocks for <model>? [y/N]`.
   This needs a **2.5A power supply and a heatsink on the CPU**; accepting it appends a `gpu_freq`
   setting to `/boot/firmware/config.txt` under a marker comment (450MHz on Pi 2, 350MHz on Pi 3).
   It is not reverted by `--remove-appliance` — remove the marked block by hand if you want it
   back out.
4. **Picks a build parallelism** automatically from available RAM (1 thread up to 460 MB, 2 up
   to 920 MB, 3 up to 1380 MB, otherwise 4). No prompt for this.
5. **Runs `sudo apt update -y`.**
6. **Adds your user to the `input`, `video` and `render` groups** (`sudo usermod -aG`), for
   access to input devices used by wheels/pads and to the GPU/DRM devices used for rendering.
7. **Installs a udev rule**, `/etc/udev/rules.d/99-cannonball.rules`, granting world read/write
   (`MODE="0666"`) on `hidraw*` (needed for force-feedback rumble on some wheels) and
   `watchdog*` devices, then reloads udev rules.
8. **Adds your user to the `audio` group and installs a PAM limits file**
   (`/etc/security/limits.d/50-cannonball-audio.conf`, copied from `res/50-cannonball-audio.conf`)
   granting the `audio` group real-time scheduling priority up to 80, `nice -20`, and unlimited
   `memlock`. This only matters if you later turn on `<rt_priority>1</rt_priority>` in
   `config.xml` (it's `0`/off by default) — without the grant, the game would silently fall back
   to normal scheduling for its audio threads.
9. **Installs build dependencies via apt**:
   `build-essential git cmake libsdl2-dev libglu1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev libmpg123-dev pkg-config alsa-utils libtinyxml2-dev`.
10. **Builds the project**: creates `build/` and `roms/`, runs `cmake -S . -B build`, then
    `cmake --build build --parallel <N>` using the thread count from step 4. No `config.xml` is
    created here — the game writes its own on first run (see [First run](#first-run)).
11. **If appliance mode was chosen, sets it up**: installs `plymouth`, `plymouth-themes` and
    `initramfs-tools` if they're not already present; installs a Plymouth theme that shows the
    splash image you chose (scaled to fit) during boot and during shutdown, with
    `i2c_bcm2835`/`vc4` ordered correctly in `/etc/initramfs-tools/modules` and
    `DeviceTimeout=20` set in `plymouthd.conf` so the graphical splash actually appears instead of
    a grey text screen, plus a `plymouth-quit` drop-in that keeps the splash on screen until the
    game itself draws; adds
    `quiet splash plymouth.ignore-serial-consoles loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0 systemd.show_status=0 rd.systemd.show_status=0`
    to `/boot/firmware/cmdline.txt` and `disable_splash=1` to `/boot/firmware/config.txt` (this
    removes the rainbow-square boot logo);
    and installs a `cannonball-se.service` systemd service that starts the game on `tty1`, as the
    user running the installer, once the splash has shown — disabling the normal login prompt on
    `tty1`. See [Appliance mode](#appliance-mode) below for full details.
12. **Prints a summary** of every step and its status.
13. **Offers to open the man page**: `Would you like to view the man page now? [y/N]` — if yes,
    runs `man -l docs/cannonball-se.6`.
14. Finishes with a reminder that you must **reboot** before running the game (the group
    membership changes only take effect on your next login), and that you still need to copy the
    ROMs into `roms/` if you haven't already. On a Raspberry Pi, it then checks `vcgencmd
    get_throttled` and prints a bright-yellow warning if under-voltage was detected at any point
    during the install — a sign the power supply may not be adequate to run CannonBall-SE at
    60fps.

Outside of appliance mode, `install.sh` does **not** set up any systemd service or auto-start —
it only builds the game and prepares device permissions. Nothing outside `apt`, the files listed
above, and the group memberships is touched.

After it finishes and you've rebooted, with the ROMs in place:

```bash
cd ~/cannonball-se
build/cannonball-se
```

The game starts with its built-in default settings and writes `config.xml` into the directory
you ran it from on first run — there's nothing to copy or edit beforehand. Sound output, and any
wheel or gamepad, are set up afterwards from the in-game menu (*Settings → Sound → Output
Device*, *Settings → Controls*). If you chose appliance mode, none of this applies — the Pi boots
straight into the game instead; see [Appliance mode](#appliance-mode) below.

### Appliance mode

Appliance mode turns a Raspberry Pi running CannonBall-SE into a dedicated arcade machine: it
boots straight into the game and powers off when you quit, with your own splash image shown
during both boot and shutdown instead of the normal console. It's offered by `install.sh` only
on Raspberry Pi OS — the prompt is skipped on Ubuntu — and only if you say yes to it; a plain
install behaves exactly as described above.

What happens after you reboot:

- The Pi shows your splash image while it boots, then the game starts automatically on `tty1`.
- Pressing **Escape** in-game (or **EXIT** from the menu) powers the Pi off, showing the splash
  image again while it shuts down.
- If the game crashes, it's restarted automatically after three seconds. If it crashes five times
  within five minutes, the Pi powers off instead of retrying indefinitely.
- If the game exits because of a configuration problem — for example, no ROMs found — it is
  **not** restarted; a normal login prompt is put back on `tty1` instead, so you can fix the
  problem. ROMs still need to be copied into `roms/` before appliance mode can start the game;
  see [ROMs](#roms) above.
- `tty2` to `tty6` (`Ctrl+Alt+F2` and so on) and SSH keep working normally for maintenance, since
  only `tty1`'s login prompt is replaced.

Useful commands, run over SSH or on another tty:

```bash
journalctl -u cannonball-se     # the game's console output
sudo systemctl status cannonball-se
sudo systemctl stop cannonball-se    # stop the game without powering off
sudo systemctl start cannonball-se   # start it again
```

To remove appliance mode and go back to a normal login on `tty1`:

```bash
./install.sh --remove-appliance
```

(equivalently, `sudo res/appliance/uninstall-appliance.sh`). This removes the
`cannonball-se.service` systemd service, the Plymouth theme, the boot options added to
`cmdline.txt`/`config.txt`, and any packages `install.sh` installed for appliance mode, then
re-enables the normal login prompt on `tty1`. A state file,
`/etc/cannonball-se/appliance.state`, records exactly what was changed so removal can undo it
precisely. The appliance assets themselves (theme, systemd unit template, splash image, the
uninstall script) live under `res/appliance/` in the repository.

### Manual build (without the script)

If you'd rather not hand a script `sudo` access, or you're on a distribution `install.sh`
doesn't target, you can do the equivalent steps yourself. This gets you a working build; the
udev/group/PAM steps above are only needed for hidraw rumble, watchdog integration, and
real-time audio scheduling — the game runs and plays fine without them.

```bash
# Install dependencies (same list install.sh uses)
sudo apt update
sudo apt install -y build-essential git cmake libsdl2-dev libglu1-mesa-dev \
  libegl1-mesa-dev libgles2-mesa-dev libmpg123-dev pkg-config alsa-utils libtinyxml2-dev

# Fetch and build
git clone https://github.com/J1mbo/cannonball-se.git
cd cannonball-se
mkdir -p build roms
cmake -S . -B build
cmake --build build --parallel $(nproc)

# Copy your ROMs into ./roms, then run:
build/cannonball-se
```

`config.xml` is not something you need to create yourself — the game writes one into the current
directory on first run, seeded from its built-in defaults.

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
   run without a valid OutRun revision B set. See [ROMs](#roms) above and check your file names
   exactly match `roms/roms.txt` — or, if using a ZIP archive, that it is a genuine MAME OutRun
   set (loose files in `roms/` always win over one). Startup prints which ROM sources were
   actually used; on failure it lists every location it searched — `roms/` (loose files and any
   `.zip` archives there), then the current working directory and the executable's directory
   (ZIP archives with a recognised MAME set name only).

If none of these explain it, run `cmake -S . -B build` on its own first and read the last few
lines of output — it will usually name the missing dependency directly.
