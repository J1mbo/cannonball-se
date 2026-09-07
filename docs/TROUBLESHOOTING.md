# Troubleshooting

Covers the problems people actually hit. For frame-rate problems see
[PERFORMANCE-TUNING.md](PERFORMANCE-TUNING.md); for build failures see
[INSTALL.md](INSTALL.md).

---

## Audio

### No audio at all, or audio stopped working after a reboot

SDL enumerates audio devices in an order that can change between boots on some systems, so
the device index in `sound.playback_device` may no longer point at the device you chose.

Run:

```bash
build/cannonball-se -list-audio-devices
```

which prints the devices and their indices, then set `sound.playback_device` in
`res/config.xml` to the right number.

There is currently no way to select a device by name. This is a known limitation.

### Stuttering, crackling or dropouts

In rough order of likelihood:

**1. USB audio on a Pi 2, Pi 3 or Pi Zero 2 W.** The Raspberry Pi USB stack is known to
handle USB audio devices poorly. Put the USB ports into full-speed mode by appending
`dwc_otg.speed=1` to `/boot/firmware/cmdline.txt` (all on one line) and rebooting.

Be aware of the trade-off: on the Pi 2 v1.2, Pi 3 and Pi Zero 2 W this generally prevents USB
*keyboards* from working, though steering wheels usually still work. Use a Bluetooth keyboard,
or connect over SSH, if you need a keyboard on those boards.

**2. The sound callback rate is too aggressive for the board.** Most noticeable on a Pi 2 in
60fps mode with analogue audio. Switch from an 8ms to a 16ms callback:
*Menu → Settings → Sound/Music → Callback Rate*, or set `sound.callback_rate` to `1` in
`res/config.xml`. This doubles audio latency, which is not perceptible in play.

**3. The audio output itself is costing CPU.** HDMI audio on Raspberry Pi boards is handled by
the GPU and is close to free. Analogue and USB audio are processed on the CPU, which costs
frame rate on Pi 1 and Pi 2 boards and can introduce stutter. Prefer HDMI where you can.

**4. The `fkms` video driver.** It is needed for HDMI audio on Pi 1 boards, but it lowers
frame rates and causes stuttering audio. On those boards, use the `kms` driver with analogue
audio instead.

### Real-time audio scheduling

There is an optional mode that gives the audio threads real-time (`SCHED_FIFO`) priority,
enabled with `sound.rt_priority` set to `1`. It is **off by default and usually the wrong
lever** — on several boards the real-time audio threads pre-empt the render threads and cost
frame rate. Try `callback_rate=1` first.

If you do want it, the process needs an `rtprio` grant. `install.sh` copies
`res/50-cannonball-audio.conf` to `/etc/security/limits.d/` for you; the grant applies to
members of the `audio` group and takes effect at the next login, so log out and back in (or
reboot). Without the grant the game silently falls back to normal scheduling — audio still
works.

---

## Controllers

### Some buttons or controls on my wheel are not recognised

Some steering wheels expose more buttons or axes than SDL maps by default. The device needs a
mapping entry in `res/gamecontrollerdb.txt`. Entries for many devices are already present; if
yours is missing you can add one (the SDL community maintains this database upstream).

### The wrong controller is being used

If more than one controller is attached, set `controls.pad_id` in `res/config.xml` to select
which one. `0` is the first device SDL enumerates.

### Force feedback pulls the wrong way

Make `controls.analog.haptic.max_force` and `min_force` negative to reverse the direction.

### Rumble does not work

For wheels without SDL rumble support, the game can drive rumble through `/dev/hidraw`. This
needs device permissions, which `install.sh` sets up. If you built manually, check your user
can read and write the relevant `/dev/hidraw*` node.

---

## Video

### The game will not start in a window

The build uses OpenGL ES. Windowed mode (`video.mode` 0) is not supported on all systems.
Try full-screen (`video.mode` 1).

### Poor frame rate under a desktop session

Running under X11 is slower than Wayland for this workload. Start the game as:

```bash
SDL_VIDEODRIVER=wayland build/cannonball-se
```

Better still, run without a desktop at all — a desktop environment is not required.

### The shadow mask looks wrong or invisible on a high-DPI screen

The mask has a fixed pitch in output pixels. On a high-DPI display it can alias away. Raise
`video.mask_size` from `3` to `4`, `5` or `6`. This takes effect immediately; no video restart
is needed.

---

## System

### The machine rebooted on its own

CannonBall-SE hooks the hardware watchdog on Raspberry Pi boards, so that a hang — for example
from an aggressive overclock — reboots the system rather than leaving a cabinet stuck on a
frozen screen. If this happens repeatedly, back off any overclock and check cooling.

### The game quits unexpectedly when I press a key

`Escape` is the master break key and quits the game. It can be remapped to `F10` in
*Menu → Settings*, or by setting the top-level `F10Escape` key to `1` in `res/config.xml`.

---

## Reporting a problem

Please open an issue at <https://github.com/J1mbo/cannonball-se/issues>. Include your board
and OS version, your `res/config.xml`, and the console output — the game prints its detected
hardware and settings at startup.
