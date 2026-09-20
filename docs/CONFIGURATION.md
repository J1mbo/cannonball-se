# Configuration Reference

This document describes every setting read from `config.xml` by CannonBall-SE.
It is a reference for hand-editing the file, and for headless/kiosk setups
where the in-game menu is not convenient to use.

## Where config.xml lives

On startup the game looks for `config.xml` in the current working directory.
If it is not found there, it falls back to `res/config.xml` (the copy shipped
in this repository) and prints a message saying it did so. If neither is
found, built-in defaults are used.

Whatever the source, **settings are always saved back to `config.xml` in the
current working directory** — not to `res/config.xml` — whenever the game
writes its configuration (see below). So after a first run from a fresh
checkout, you will usually find a new `config.xml` next to the executable,
seeded from `res/config.xml`.

The `-cfgfile <path>` command-line flag overrides the file name (and path)
used for both loading and saving, e.g. `cannonball-se -cfgfile mycab.xml`.

Most of the settings below can also be changed from the in-game menu
(enable it with `menu.enabled`, see the [Menu](#menu) section, or open it
with the configured menu key/button). Changes made in the menu are written
back to `config.xml` automatically when you choose "Enhanced Settings" /
"Original Settings" presets, or on exit from relevant screens. If you edit
the file by hand while the menu is also in use, hand edits can be
overwritten the next time the game saves.

A handful of keys read by the code are **not present** in the shipped
`res/config.xml`. These are called out under each table — you can add them
yourself if you need to override the compiled-in default.

### Video restart

Some video settings take effect immediately; others require the video
subsystem to be reinitialised, which the game does automatically (briefly
blanking the screen) whenever one of the following is changed from the
in-game menu: **window scale, screen mode (windowed/full-screen/stretched),
widescreen, hi-resolution mode, CRT shader mode, and enabling/disabling the
Blargg NTSC filter**. All other video settings covered in this document
(CRT look, mask tuning, warp, noise, offsets, Blargg tuning parameters, etc.)
apply live without a restart. This applies to changes made in-game; editing
`config.xml` directly and restarting the game naturally picks up every
setting.

---

## Top-level settings

| Key | Default | Values | What it does |
|---|---|---|---|
| `F10Escape` | `0` | `0`, `1` | Which key exits the game / returns to the front-end menu. `0` = Escape, `1` = F10. Also toggleable in-game (Master Break Key). |

`F10Escape` is **not present** in the shipped `res/config.xml`; the compiled-in default (Escape) applies unless you add it.

---

## data.\* settings

These control where the game looks for its resource files. They are read
very early, before most of the rest of the config file.

| Key | Default | Values | What it does |
|---|---|---|---|
| `data.rompath` | `roms/` | path | Directory the game searches for ROM files, or a `.zip` file to search directly. Loose files here take priority; MAME-format ZIP archives (matched by CRC32/size, not filename) are also searched here, then in the current working directory and the executable's directory (recognised MAME OutRun set names only). |
| `data.respath` | `res/` | path | Directory the game searches for resource files (shaders, fonts, custom music, this config file's fallback copy). |
| `data.savepath` | `./` | path | Directory used for hi-score files (`hiscores*.xml`), time-trial scores, and the play-stats file. |
| `data.crc32` | `1` | `0`, `1` | Whether ROMs are looked up by CRC32/size (`1`) or by exact filename (`0`). Loaded ROM data is CRC32-verified either way. |

None of the `data.*` keys are present in the shipped `res/config.xml`; all four take their compiled-in defaults unless added.

---

## Video

Controls display mode, resolution, the CRT shader pipeline (shape, shadow
mask, scanlines, warp, noise, colour), and the CPU-side Blargg NTSC filter.
This is the section with the most impact on frame rate — see
[docs/PERFORMANCE-TUNING.md](PERFORMANCE-TUNING.md) for a board-by-board
tuning guide built around the four keys marked **PERFORMANCE** below.

| Key | Default | Values | What it does |
|---|---|---|---|
| `video.mode` | `1` | `0` window, `1` full-screen, `2` full-screen stretched (no borders, aspect ratio lost, scanlines disabled) | Screen mode. Window mode may not be supported on all systems (this build uses OpenGL-ES). Requires video restart when changed in-game. |
| `video.window.scale` | `1` | `1`-`3` | Zoom factor used in windowed mode (cycles 1→2→3→1 from the menu). Requires video restart. |
| `video.widescreen` | `0` | `0`, `1` | Widescreen display mode. Requires video restart. |
| `video.fps_counter` | `0` | `0`, `1` | Shows an on-screen frame-rate counter. |
| `video.vsync` | `1` | `0`, `1` | Enable V-Sync where the backend supports it (OpenGL). The shipped `res/config.xml` ships with this set to `0`, overriding the compiled-in default of `1`. |
| `video.x_offset` | `0` | `-100` to `100` | Horizontal image offset in pixels from the calculated position. |
| `video.y_offset` | `0` | `-100` to `100` | Vertical image offset in pixels from the calculated position. |
| `video.fps` | `0` | `0` open at 30fps, `1` open at 60fps | Frame rate at startup. The game auto-switches upward if the hardware sustains it, so `0` is the safe default. The `-30`/`-60` command-line flags lock the rate instead. |
| `video.hires` | `1` | `0`, `1` | **PERFORMANCE.** Game engine resolution: `0` = original arcade resolution, `1` = double resolution. Roughly quadruples CPU pixel work when enabled — the single biggest lever on CPU-limited boards. Requires video restart (in-game this is changed via `hires_next`, applied on the next automatic restart). |
| `video.hiresprites` | `0` | `0`, `1` | **PERFORMANCE.** Render sprites at hi-res too. Only has an effect when `video.hires` is `1`. Moderate additional cost on the sprite renderer only; may cause slight misalignment of some objects. Automatically forced to `0` if `video.hires` is turned off. Toggleable in-game with F7. |
| `video.shader_mode` | `2` | `0` off (pass-through), `1` fast, `2` full | **PERFORMANCE.** CRT shader complexity. High GPU cost. Measured on a Pi 2 (VideoCore IV @ 500MHz, 1280x1024): full 9.03 ms/frame, fast 7.23 ms/frame — dropping from full to fast is the biggest lever on GPU-limited boards. Requires video restart. |
| `video.blargg` | `1` | `0` off, `1` composite, `2` s-video, `3` RGB | **PERFORMANCE.** CPU-side Blargg NTSC filter mode (see [docs/SCREEN-FILTERS.md](SCREEN-FILTERS.md)). Moderate CPU cost; automatically disabled on single-core boards. Toggling it off/on (crossing to/from `0`) requires a video restart; changing between composite/s-video/RGB does not. |
| `video.shadow_mask` | `2` | `0` off, `1` overlay-based, `2` shader-based | Shadow mask type. Overlay is cheapest; shader-based looks better but can over-saturate bright pixels to white. The shader mask pattern is baked into a texture at startup, so mode `2` is far cheaper than earlier builds. Applies live. |
| `video.mask_size` | `3` | `3` normal, `4`-`6` for high-DPI displays | Shadow mask pitch in output pixels. Higher values keep the mask visible rather than aliasing away on high-DPI screens. Re-bakes the mask texture; no video restart needed. |
| `video.maskDim` | `75` | `0`-`100` (used as a percentage) | Darkens the masked-off pixels by this percentage. |
| `video.maskBoost` | `135` | `100`-`195` (used as a percentage) | Brightens the lit pixels by this percentage. The shipped `res/config.xml` ships with `125`, differing from the compiled-in default of `135`. |
| `video.scanlines` | `0` | `0`-`3` | Scanline strength; `0` = off, `3` = maximum. |
| `video.crt_shape` | `1` | `0`, `1` | Screen curvature and bezel overlay. Implemented as an SDL2 texture overlay alongside the vignette in all three shader modes, so it does not require a restart. |
| `video.vignette` | `30` | `0`-`75` (percentage, steps of 5 from the menu) | Edge dimming amount. Implemented in the overlay/shader; no restart required. |
| `video.noise` | `5` | `0`-`20` | **PERFORMANCE (minor).** Analogue noise amount, as a percentage. Only available when `video.shader_mode` is `2` (full). This is the one "CRT look" setting with a real per-pixel, per-frame GPU cost. The shipped `res/config.xml` ships with `6`. |
| `video.warpX` | `3` | `0`-`10` | Barrel distortion along the X axis, as a percentage. The bezel overlay follows this automatically. Only available when a shader mode is enabled. The shipped `res/config.xml` ships with `1`. |
| `video.warpY` | `4` | `0`-`10` | Barrel distortion along the Y axis, as a percentage. The shipped `res/config.xml` ships with `3`. |
| `video.desaturate` | `5` | `0`-`10` | Desaturation applied across the whole image (raises the black level), as a percentage. Only available in full shader mode. |
| `video.desaturate_edges` | `4` | `0`-`10` | Additional desaturation towards the edges, as a percentage. Only available in full shader mode. |
| `video.brightboost` | `0` | `0`-`10` | Output brightness boost, as a percentage. The shipped `res/config.xml` ships with `1`. |
| `video.saturation` | `30` | `-50` to `50` | Blargg filter saturation. Only meaningful when `video.blargg` is not `0`. |
| `video.contrast` | `0` | Purpose not documented in code beyond the Blargg filter contrast control | Blargg filter contrast. |
| `video.brightness` | `0` | Purpose not documented in code beyond the Blargg filter brightness control | Blargg filter brightness. |
| `video.sharpness` | `0` | — | Blargg filter edge blurring. |
| `video.resolution` | `0` | `-2` to `0` | Blargg filter horizontal resolution. |
| `video.gamma` | `0` | `-3` to `3` | Blargg filter gamma. |
| `video.hue` | `-2` | `-10` to `10` (i.e. -0.1 to +0.1 internally) | Blargg filter hue. |

No `video.*` keys are missing from the shipped `res/config.xml`; every key above is present in the file. Where the shipped value differs from the compiled-in default, this is noted in the row.

Note: the code also has `video.filtering` and `video.shadow` fields in `config.hpp`, but neither is read from or written to `config.xml` — they are not configuration keys.

---

## Sound

Controls audio output: sample rate, playback device, volume of custom
tracks, and how attract-mode music behaves.

| Key | Default | Values | What it does |
|---|---|---|---|
| `sound.enable` | `1` | `0`, `1` | Master toggle for sound. |
| `sound.advertise` | `1` | `0`, `1` | Play music during attract mode. |
| `sound.preview` | `1` | `0`, `1` | Play music on the music/route-select screen. |
| `sound.fix_samples` | `1` | `0`, `1` | Use the corrected PCM sample set. |
| `sound.music_timer` | `0` | `0` (arcade default, 30s) or `1`-`99` | Countdown shown on the music/route-select screen, in seconds. `0` falls back to the arcade default. |
| `sound.rate` | `44100` | `31250`/`15625` (arcade-accurate), `22050`, `44100` | Audio sample rate. The arcade ran at 31,250Hz (or possibly 15,625Hz); if using custom MP3/WAV music, `22050` or `44100` must be used. |
| `sound.playback_device` | `-1` | device index, or `-1`/omitted for system default | Index of the SDL playback device to use. Available devices are listed on screen at startup; enter the number shown for the desired device. The shipped `res/config.xml` sets this to `0` (the first listed device) rather than the compiled-in default of `-1` (system default). Only used as a fallback when `sound.playback_device_name` below is empty or can't be matched. |
| `sound.playback_device_name` | *(empty)* | device name, or empty | Name of the SDL playback device, exactly as printed by `-list-audio-devices`. When set, takes priority over `sound.playback_device` above (matched exactly, then as a substring), so the chosen device keeps working even if its index changes between boots. Also selectable from the in-game SOUND menu (OUTPUT DEVICE), which updates both keys live. Leave empty to use the index. |
| `sound.callback_rate` | `2` | `0` (8ms), `1` (16ms), `2` (auto) | Audio callback interval. `1` doubles audio latency but may reduce dropouts on some systems (e.g. WSL2). `2` (auto, the default for fresh installs) resolves at startup to `1` (16ms) on Raspberry Pi 1/2/3-class CPUs (ARM1176, Cortex-A7, Cortex-A53, detected from the CPU part number in `/proc/cpuinfo`) and to `0` (8ms) everywhere else, including Windows and WSL; any value outside `0`-`2` is also treated as auto. Existing config files that already say `0` or `1` keep that explicit choice. |
| `sound.rt_priority` | `0` | `0`, `1` | Request `SCHED_FIFO` real-time scheduling for the audio threads (needs an rtprio grant — see `res/50-cannonball-audio.conf`). Off by default: on some boards RT audio preempts the render threads and costs frame rate. See [docs/PERFORMANCE-TUNING.md](PERFORMANCE-TUNING.md); prefer `sound.callback_rate=2` (auto) or `1` (16ms) to address dropouts instead. |
| `sound.wave_volume` | `4` | `1`-`8` | Playback volume applied to custom `.wav`/`.mp3` tracks. The code comment in `config.cpp` states `4` is unity gain; the field comment in `config.hpp` states `5` is unity gain — the two disagree, and this document cannot resolve which is correct without testing. |

No `sound.*` keys are missing from the shipped `res/config.xml`.

Custom music tracks themselves are not config keys: the game scans
`data.respath` for files named `NN_Track_Name.wav|mp3|ym` and loads them
automatically (indexes 01–03 replace the three built-in tracks; higher
indexes add tracks).

---

## Controls

Gear, digital/analogue steering behaviour, keyboard and gamepad button
mapping, analogue wheel/pedal axes, and force-feedback (haptic) tuning.
Button/axis mapping is easiest to set from the in-game control
configuration screens rather than by hand.

### General

| Key | Default | Values | What it does |
|---|---|---|---|
| `controls.gear` | `2` | `0` manual click-to-shift, `1` manual hold-to-shift (cabinet play), `2` automatic | Gear-change behaviour. |
| `controls.pad_id` | `0` | index | Which attached controller to use when more than one is present. `0` = the first one SDL enumerates. |
| `controls.steerspeed` | `3` | `1`-`9` | Digital-controls steering adjust speed. |
| `controls.pedalspeed` | `4` | `1`-`9` | Digital-controls pedal adjust speed. |
| `controls.rumble` | `1.25` | float, cycles `0`-`1.0` in `0.25` steps from the menu (shipped default exceeds that range) | Simple controller rumble/vibration strength. |

### controls.keyconfig (keyboard mapping)

Each value is an SDL keycode (`SDLK_*`). Defaults shown are the built-in
arrow keys plus A/Z/Space/S/C/M/G/H/V.

| Key | Default | What it does |
|---|---|---|
| `controls.keyconfig.up` | `1073741906` | Steer/menu up (SDLK_UP). |
| `controls.keyconfig.down` | `1073741905` | Steer/menu down (SDLK_DOWN). |
| `controls.keyconfig.left` | `1073741904` | Steer/menu left (SDLK_LEFT). |
| `controls.keyconfig.right` | `1073741903` | Steer/menu right (SDLK_RIGHT). |
| `controls.keyconfig.acc` | `97` | Accelerate ('a'). |
| `controls.keyconfig.brake` | `122` | Brake ('z'). |
| `controls.keyconfig.gear1` | `103` | Gear position 1 ('g'). |
| `controls.keyconfig.gear2` | `104` | Gear position 2 ('h'). |
| `controls.keyconfig.start` | `115` | Start ('s'). |
| `controls.keyconfig.coin` | `99` | Coin/credit ('c'). |
| `controls.keyconfig.menu` | `109` | Open menu ('m'). |
| `controls.keyconfig.view` | `118` | Change camera view ('v'). |

The shipped `res/config.xml` also contains `controls.keyconfig.gear`
(value `32`, i.e. Space) — this key is **not read by the code at all**. It
appears to be left over from an older single-button gear scheme; the game
now uses `gear1`/`gear2` instead, so setting it has no effect.

### controls.padconfig (gamepad/joystick mapping)

Each value is an SDL controller button/axis index, or `-1` for unbound.

| Key | Default | What it does |
|---|---|---|
| `controls.padconfig.acc` | `-1` | Accelerate button. |
| `controls.padconfig.brake` | `-1` | Brake button. |
| `controls.padconfig.gear1` | `-1` | Gear position 1 button. |
| `controls.padconfig.gear2` | `-1` | Gear position 2 button. |
| `controls.padconfig.start` | `-1` | Start button. |
| `controls.padconfig.coin` | `-1` | Coin/credit button. |
| `controls.padconfig.menu` | `-1` | Open menu button. |
| `controls.padconfig.view` | `-1` | Change camera view button. |
| `controls.padconfig.up` | `-1` | D-pad/stick up. |
| `controls.padconfig.down` | `-1` | D-pad/stick down. |
| `controls.padconfig.left` | `-1` | D-pad/stick left. |
| `controls.padconfig.right` | `-1` | D-pad/stick right. |
| `controls.padconfig.limit_l` | `-1` | Purpose not documented in code beyond "left limit" button binding. |
| `controls.padconfig.limit_c` | `-1` | Purpose not documented in code beyond "centre limit" button binding. |
| `controls.padconfig.limit_r` | `-1` | Purpose not documented in code beyond "right limit" button binding. |

`controls.padconfig.limit_l`, `.limit_c` and `.limit_r` are **not present**
in the shipped `res/config.xml`; they take the default of `-1` (unbound)
unless added. The shipped file also contains `controls.padconfig.gear`
(value `22`) which, like `keyconfig.gear` above, is **not read by the
code** and has no effect.

### controls.analog (analogue wheel/pedals and haptics)

| Key | Default | Values | What it does |
|---|---|---|---|
| `controls.analog.<xmlattr>.enabled` (the `enabled` attribute on `<analog>`) | `1` | `0` off, `1` on, `2` wheel only | Master switch for analogue input. |
| `controls.analog.axis.wheel` | `-1` | axis index, `-1` unbound | Axis used for the steering wheel. |
| `controls.analog.axis.accel` | `-1` | axis index, `-1` unbound | Axis used for the accelerator. Can share an axis with the brake on split-axis controllers, though this is best avoided. |
| `controls.analog.axis.accel.<xmlattr>.invert` | `0` | `0`, `1` | Inverts the accelerator axis. |
| `controls.analog.axis.brake` | `-1` | axis index, `-1` unbound | Axis used for the brake. |
| `controls.analog.axis.brake.<xmlattr>.invert` | `0` | `0`, `1` | Inverts the brake axis. |
| `controls.analog.axis.motor` | `-1` | axis index, `-1` unbound | Axis used to read motor position feedback on a SmartyPi moving cabinet (see `smartypi.cabinet`). Not present in the shipped `res/config.xml`. |
| `controls.analog.wheel.zone` | `75` | `0`+ | Amount of the wheel's turning circle to use. `0` means use the entire wheel (works best with haptics enabled); larger values progressively use less of the turning circle. The shipped `res/config.xml` ships with `0`. |
| `controls.analog.wheel.dead` | `0` | `0`-`128` | Dead zone in the centre of the wheel; total wheel range is 128. Useful if the centre accuracy of your wheel is poor. |
| `controls.analog.haptic.<xmlattr>.enabled` (the `enabled` attribute on `<haptic>`) | `1` | `0`, `1` | Force-feedback/haptic support toggle. |
| `controls.analog.haptic.max_force` | `9000` | `0`-`10000` | Maximum force-feedback force to apply. Make this and `min_force` negative together if feedback is in the wrong direction. The shipped `res/config.xml` ships with `8500`. |
| `controls.analog.haptic.min_force` | `8500` | `0` to `max_force` | Minimum force-feedback force to apply. The shipped `res/config.xml` ships with `7000`. |
| `controls.analog.haptic.force_duration` | `20` | — | Length of each force-feedback effect, in `1/x` seconds; `1/20` is suggested. |

The accelerator and brake `invert` attributes above are the legacy analogue-axis flags read from
`controls.analog.axis`. When a wheel or pedal is bound through the in-game Configure Inputs editor,
its direction is instead detected automatically during capture, and the resulting invert state is
stored per binding in the input-binding matrix rather than under `controls.analog.axis`.

`controls.analog.axis.motor` is not present in the shipped
`res/config.xml`. All other `controls.analog.*` keys are present.

---

## Engine

Gameplay and difficulty settings: timers, traffic, track selection, engine
bug-fixes, and handling tweaks.

| Key | Default | Values | What it does |
|---|---|---|---|
| `engine.time` | `0` | `0` Easy (80s), `1` Normal (75s), `2` Hard (72s), `3` Very Hard (70s), `4` Infinite Time | Game timer difficulty (arcade DIP-switch equivalent). Internally, `4` sets a separate "freeze timer" flag and the stored value is masked to `0`-`3`. |
| `engine.traffic` | `1` | `0` Easy, `1` Normal, `2` Hard, `3` Very Hard, `4` No Traffic | Traffic difficulty (arcade DIP-switch equivalent). `4` sets a separate "disable traffic" flag; the stored value is masked to `0`-`3`. |
| `engine.freeplay` | `1` | `0`, `1` | Free play (no coin required to start). |
| `engine.japanese_tracks` | `0` | `0`, `1` | Use the original Japanese release's track layout and course ordering. Requires the additional Japanese ROM files (`epr-10380.133`, `epr-10382.118`, `epr-10381.132`, `epr-10383.117`, `epr-10327.76`, `epr-10329.58`, `epr-10328.75`, `epr-10330.57`). |
| `engine.prototype` | `0` | `0`, `1` | Swap in the prototype Coconut Beach track. |
| `engine.levelobjects` | `1` | `0`, `1` | Increase the number of allowed level objects. Fixes the broken arches in Gateway, for example. |
| `engine.randomgen` | `1` | `0`, `1` | Use the original random-number-generator seed to match the original game's setup (affects initial traffic patterns in attract mode, for example). Disable for a truly random setup. |
| `engine.fix_bugs` | `1` | `0`, `1` | Fix minor bugs present in the original engine (e.g. a tile glitch on the music-select screen, checkpoint millisecond value). |
| `engine.fix_timer` | `0` | `0`, `1` | Purpose not fully documented beyond its name; grouped with `fix_bugs` as an engine timer-related fix. The shipped `res/config.xml` ships with `1`, overriding the compiled-in default of `0`. |
| `engine.fix_traffic` | `0` | `0`, `1` | Reset a newly spawned traffic car's "car ahead" speed so it cannot brake sharply on stale data when only the player is alongside it. `0` keeps the original arcade behaviour. |
| `engine.layout_debug` | `0` | `0`, `1` | Purpose not documented in code beyond its name (debug aid for level layout). Not present in the shipped `res/config.xml`. |
| `engine.new_attract` | `1` | `0`, `1` | Use the "new" attract-mode behaviour. The shipped `res/config.xml` ships with `0`, overriding the compiled-in default of `1`. |
| `engine.offroad` | `0` | `0`, `1` | Handling: allow driving off-road. |
| `engine.grippy_tyres` | `0` | `0`, `1` | Handling: stick to the track. |
| `engine.bumper` | `0` | `0`, `1` | Handling: smash into other cars without spinning out. |
| `engine.turbo` | `0` | `0`, `1` | Handling: faster car. |
| `engine.car_color` | `0` | palette index | Car colour/palette selection. |

`engine.layout_debug` is not present in the shipped `res/config.xml`.
Where the shipped value differs from the compiled-in default (`engine.fix_timer`, `engine.new_attract`), this is noted in the row above.

### scores.\* (high-score behaviour)

These are a separate top-level `<scores>` section, not nested under `<engine>`, but they feed the same `engine_settings_t` struct in code.

| Key | Default | Values | What it does |
|---|---|---|---|
| `scores.delete_last_entry` | `1` | `0`, `1` | Whether the last entry in a hi-score table can be deleted/overwritten. |
| `scores.hiscore_timer` | `0` | `0` (arcade default) or `1`-`99` | Countdown shown on the hi-score entry screen, in seconds. `0` falls back to the arcade default. |

Neither `scores.delete_last_entry` nor `scores.hiscore_timer` is present in the shipped `res/config.xml`.

---

## time_trial

Default settings applied when starting Time Trial mode.

| Key | Default | Values | What it does |
|---|---|---|---|
| `time_trial.laps` | `5` | `1`-`5` | Default number of laps. The shipped `res/config.xml` ships with `3`, overriding the compiled-in default of `5`. |
| `time_trial.traffic` | `3` | `0`-`8` (`0` = off) | Default amount of traffic. |

Both keys are present in the shipped `res/config.xml`.

---

## continuous

Default settings applied when starting Continuous mode.

| Key | Default | Values | What it does |
|---|---|---|---|
| `continuous.traffic` | `3` | — | Traffic level for Continuous mode. The shipped `res/config.xml` ships with `1`, overriding the compiled-in default of `3`. |

---

## menu

Controls whether the game boots into the in-built front-end menu system, and
that menu's cosmetic road-scroll speed.

| Key | Default | Values | What it does |
|---|---|---|---|
| `menu.enabled` | `0` | `0`, `1` | Whether the game starts in the in-built menu (`1`) or goes straight into the game (`0`). With it disabled, edit this config file directly instead. |
| `menu.roadspeed` | `50` | — | Scroll speed of the road background on the menu screen. Internally doubled when running at other than 60fps to keep the same visual speed. The shipped `res/config.xml` ships with `25`, overriding the compiled-in default of `50`. |

Both keys are present in the shipped `res/config.xml`.

---

## smartypi

Settings for CannonBall running inside a SmartyPi-equipped arcade cabinet.
Not relevant to a standard desktop/handheld install.

| Key | Default | Values | What it does |
|---|---|---|---|
| `smartypi.<xmlattr>.enabled` (the `enabled` attribute on `<smartypi>`) | `0` | `0`, `1` | Whether CannonBall is running in conjunction with a SmartyPi arcade cabinet controller. |
| `smartypi.outputs` | `1` | `0`, `1` | Write digital output states to the console (diagnostic). |
| `smartypi.cabinet` | `1` | `0` moving, `1` upright, `2` mini | Cabinet type, which determines how outputs (e.g. motor control) are driven. |

None of the `smartypi.*` keys are present in the shipped `res/config.xml`
(there is no `<smartypi>` section in the file at all); all three take
their compiled-in defaults unless added. Note also that `save()` only
writes `smartypi.cabinet` back to the file when `smartypi.enabled` is set —
so on a standard install this key will not appear even after the game has
saved its configuration once.

---

## Summary of keys absent from the shipped res/config.xml

The following keys are read by `config.cpp` but do not appear in the
shipped `res/config.xml`, so they silently take their compiled-in default
unless you add them by hand:

- `F10Escape`
- `data.rompath`, `data.respath`, `data.savepath`, `data.crc32`
- `controls.analog.axis.motor`
- `controls.padconfig.limit_l`, `controls.padconfig.limit_c`, `controls.padconfig.limit_r`
- `engine.layout_debug`
- `scores.delete_last_entry`, `scores.hiscore_timer`
- `smartypi.<xmlattr>.enabled`, `smartypi.outputs`, `smartypi.cabinet`

Two further keys that used to appear in the shipped file - `controls.keyconfig.gear`
and `controls.padconfig.gear` - have been removed, because the code never read
them; they were superseded by the separate `gear1`/`gear2` bindings. If your own
`config.xml` still contains them, they are simply ignored and can be deleted.
