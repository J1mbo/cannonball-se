# Playing

## Starting a game

The game boots into attract mode, like the arcade cabinet. Press **C** to insert a coin, then
**S** to start. Free play is on by default, so **S** alone will do it.

## Keyboard

| | |
|---|---|
| Steer | ← → |
| Accelerate / Brake | `A` / `Z` |
| Low / High gear | `G` / `H` |
| Change view | `V` |
| Coin / Start | `C` / `S` |
| Menu | `M` |
| Quit | `Esc` |

All of these can be remapped in *Menu → Settings → Controls → Redefine Keys*, or by editing
`controls.keyconfig` in `config.xml` (see [CONFIGURATION.md](CONFIGURATION.md)).

Quit can be moved from `Esc` to `F10` in *Menu → Settings*, which is what you want in a cabinet
where a stray `Esc` would end someone's game.

## Function keys

These are fixed and cannot be remapped:

| | |
|---|---|
| `F1` | Pause |
| `F2` | Advance one frame while paused |
| `F3` | Toggle the game timer |
| `F5` | Open the menu |
| `F7` | Toggle hi-res sprites (only while the hi-res engine is active) |
| `F8` | Toggle **all** video processing on and off |
| `F9` | Toggle the shadow mask |

`F8` is the one to try first — it flips between the full CRT look and a raw, unfiltered image, so
you can see exactly what the filter pipeline is doing. It is also the quickest way to judge what
the effects are costing you in frame rate.

## Wheels and gamepads

USB steering wheels, joysticks and gamepads are supported. Set them up in
*Menu → Settings → Controls → Redefine Gamepad*.

Force feedback works on wheels that support it. If the wheel pulls the wrong way, make both
`max_force` and `min_force` negative in `config.xml`. For wheels without SDL rumble, the game can
drive rumble through `/dev/hidraw` — `install.sh` sets up the permissions for this.

If some buttons are not recognised, the device needs a mapping entry in `res/gamecontrollerdb.txt`.
See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Custom music

As well as the three tracks in the ROMs, you can add your own. Put `.wav`, `.mp3` or `.ym` files
in `res/`, named:

```
[01-99]_Track_Display_Name.[wav|mp3|ym]

01_My_Replacement_Track.mp3      replaces Magical Sound Shower
04_AHA_Take_On_Me.mp3            adds a new entry to the radio
```

Indexes **01-03** replace the built-in tracks (01 = *Magical Sound Shower*, 02 = *Passing Breeze*,
03 = *Splash Wave*); **04 and above** add new entries to the radio list on the track-select screen.

Use **44.1kHz, 16-bit stereo** files, and leave `sound.rate` at its default of 44100. Playback
volume for WAV files is set by `sound.wave_volume` (1-8, where 5 is unity gain).

## Game options

Time limits, traffic density, the Japanese track layout, the prototype Coconut Beach track,
free play and the original random-number seed are all in *Menu → Settings*, or under `engine.*`
in `config.xml`. [CONFIGURATION.md](CONFIGURATION.md) lists every option with its default.

## See also

- [CONFIGURATION.md](CONFIGURATION.md) — every setting in `config.xml`
- [SCREEN-FILTERS.md](SCREEN-FILTERS.md) — what the CRT pipeline does
- [PERFORMANCE-TUNING.md](PERFORMANCE-TUNING.md) — getting to 60fps on your board
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — when something is wrong
