# CannonBall-SE: Input Binding Matrix + Rumble Subsystem — Low-Level Design

This document describes the design of the CannonBall-SE input binding matrix and
rumble subsystem as implemented. It names the SE files/functions involved, the config
keys, the menu entries and the staging.

Line numbers below are SE unless prefixed `DX`, which refer to the sibling cannonball-dx
fork cited here as prior art.

---

## 0. Overview

### 0.1 What is being built

1. **Deliverable A** — a persistent **KEYBOARD / GAMEPAD / WHEEL binding matrix** re-implemented
   onto SE's single `Input` class. The WHEEL column live-captures raw `SDL_Joystick`
   axis/button/hat events from *every* opened joystick, so any wheel, pedal set, shifter or arcade
   encoder binds with **no `gamecontrollerdb.txt` entry**. The GAMEPAD column uses the
   `SDL_GameController` abstraction (so a DB-known pad has stable, named buttons and works with zero
   setup). A GAMEPAD/WHEEL **input-mode** selector decides which family drives the car in-game
   and where feedback is routed.
2. **Deliverable B** — a proper **rumble/haptic subsystem** replacing the hard-coded Thrustmaster
   hidraw bytes in `Input::set_rumble()` (input.cpp:614-678): selectable backends, named raw-HID
   **profiles**, an **advanced/custom** profile editor with live test, VID/PID auto-suggest, a
   platform-agnostic game-side API, and write validation/rate limiting.

### 0.2 What is deliberately NOT built (scope guard)

DX's `MenuBase`/`config_base`/`input_base` scaffolding (DX compiles the old file under a macro-renamed
class and wraps it — DX `input.cpp:12-44`, `menu.cpp:36-38`, `config.cpp:35-37`); the Windows-gated
SDL_Haptic FFB rewrite (`DX src/main/directx/ffeedback_base.cpp`); `radio_button`; VIEW1-3 direct
view buttons; adaptive-fps removal; ultrawide; engine-resolution-scale; multiplayer; cabinet output.
`src/main/directx/ffeedback.cpp` is **not modified** — it is wrapped as one rumble backend.

### 0.3 SE post-fork edits that must survive (where they sit vs. this design)

| Commit | Where | How this design keeps it |
|---|---|---|
| `29258db` F7 hi-res-sprite guard | `input.cpp:335-354` inside `Input::handle_key()` (plus the F8 `videoRestartRequired` guard at :360) | `handle_key()` is **not touched** by any stage. All new input code lives in new member functions; the keyboard path is unchanged. |
| `f82ac81` `sound.rt_priority` | `config.hpp:149`, `config.cpp:254-258` (load), `config.cpp:437` (save) | New keys are added only inside the **controls** load block (config.cpp:286-332) and save block (config.cpp:444-483). The sound block is not edited. |
| `688f83e` headers / dead-code | `input.cpp:10`, `config.cpp:10-16`, `menu.cpp:8`, `config.hpp:10` copyright lines | Headers are kept; add a bullet to the existing "Modifications for Cannonball-SE" list rather than rewriting the block. |

### 0.4 Two latent SE defects found while reading (fix in passing, flagged so they are not silent)

- **Wheel zone maths divides instead of multiplies.** `input.cpp:459-460`:
  `adjusted = adjusted / (100 - wheel_zone)` (comment says "multiply up"). With `zone=75` the
  steering range collapses to `0x7D..0x82`. The shipped `res/config.xml:209` sets `zone=0`, so
  default installs are unaffected — but `config.cpp:326` defaults the key to **75** when it is
  missing from a user's config.xml. Stage 2 adopts DX's `raw*100/(100-zone)` (DX `input.cpp:888-892`)
  and changes the config.cpp default to 0.
- **`RUMBLE_LABELS` out-of-bounds read.** `menu.cpp:1468` indexes `RUMBLE_LABELS[5]`
  (menulabels.hpp:156) with `(int)(rumble/0.25f)`; the shipped default `rumble=1.25`
  (`res/config.xml:227`, `config.cpp:290`) yields index 5. Stage 1 clamps master strength to
  `0..1` on load and the label index to `0..4`.

---

## Deliverable A — Binding matrix re-implementation

### A.1 Data model

#### A.1.1 `device_binding_t` (reused from DX `config_base.hpp:155-185`, one field added)

Lives in **`src/main/frontend/config.hpp`**, immediately above `controls_settings_t` (config.hpp:155).

```cpp
struct device_binding_t
{
    enum Type   { TYPE_BUTTON = 0, TYPE_AXIS = 1, TYPE_HAT = 2 };
    enum Group  { GROUP_GAMEPAD = 0, GROUP_WHEEL = 1, GROUP_COUNT = 2 };
    enum Target
    {
        TARGET_STEER = 0,           // axis only
        TARGET_ACCEL, TARGET_BRAKE, // axis OR button/hat
        TARGET_GEAR1, TARGET_GEAR2, TARGET_START, TARGET_COIN, TARGET_MENU, TARGET_VIEW,
        TARGET_UP, TARGET_DOWN, TARGET_LEFT, TARGET_RIGHT,   // menu nav + digital steering
        TARGET_COUNT                // 13 == matrix rows
    };

    int         group  = GROUP_GAMEPAD;   // ADDED vs DX: explicit, not a "G:"/"W:" prefix in `device`
    int         target = TARGET_STEER;
    int         type   = TYPE_BUTTON;
    int         index  = -1;              // GC axis/button enum (GAMEPAD) or raw index (WHEEL)
    int         value  = 0;               // HAT direction mask; unused otherwise
    std::string device;                   // signature, see A.1.2 ("" never stored; "*" not supported)
    uint16_t    vid = 0, pid = 0;         // ADDED: fallback match + rumble-profile suggestion
    std::string name;                     // ADDED: display only (never matched on)
};
```

Deviation from DX, and why: DX encodes the group as a `"G:"`/`"W:"` prefix inside `device`
and re-parses it in three places (`DX input.cpp:119-150`, `DX menu.cpp:273-282`,
`DX config.cpp:192-196`). SE's XML schema (A.2) carries the group structurally, so the field is
explicit. `TARGET_UP..RIGHT` replace DX's hard-wired D-pad/hat handling because Pi cabinets with
button-wired joysticks (no hat) need bindable navigation; SE already has `padconfig[8..11]` for
this but the wizard never set them (menu.cpp:1577-1616 only walks slots 0..7).

**Cell rule:** exactly **one** binding per `(group, target)` — a matrix cell is a single
assignment (DX `input.cpp:637-648`). Re-capturing replaces; DEL clears. Consequence: ACCEL/BRAKE
are *either* an axis *or* a digital control per group. SE's current default binds both
`TRIGGERRIGHT` and `RIGHTSHOULDER` to accelerate (input.cpp:84-87); the new default keeps the
trigger (analog). Users who want shoulder-button pedals rebind — `analog` is then derived as
"wheel only" (A.3.4) and `OInputs::digital_pedals()` runs, so it just works.

#### A.1.2 Device signature (survives reconnects and enumeration-order changes)

`signature = SDL_JoystickGetGUIDString() + "|A" + axes + "|B" + buttons + "|H" + hats`
— DX `input_base.cpp:42-49` verbatim. The GUID encodes bus/VID/PID/version, so the same wheel
re-plugged into another port or enumerated in a different order matches. `name` is **not** part of
the signature: the same device reports different names between SDL's evdev and hidapi backends and
between Windows and Linux.

Matching order at runtime (`Input::rebuild_binding_cache()`):
1. exact `signature` match;
2. else `vid == vid && pid == pid && A/B/H equal` (SDL ≥2.0.6 `SDL_JoystickGetVendor/Product`).
   Covers GUID-byte drift between SDL versions (SDL ≥2.0.14 stamps a CRC of the name into GUID
   bytes 2-3 on Linux; a distro SDL upgrade must not orphan bindings);
3. else the binding is dormant for this session (still displayed in the editor, greyed).

Two physically identical devices share a signature; both drive the same bindings. Acceptable and
documented (a second identical pad is a rare cabinet case).

#### A.1.3 `controls_settings_t` additions (config.hpp:155-178)

```cpp
    // --- existing fields unchanged: gear, steer_speed, pedal_speed, padconfig[15], keyconfig[12],
    //     pad_id, analog, axis[4], asettings[2], invert[3], rumble, haptic, max_force, min_force,
    //     force_duration ---

    int input_mode;                          // device_binding_t::GROUP_GAMEPAD / GROUP_WHEEL
    bool bindings_active;                    // true iff <controls><bindings> existed in config.xml
    std::vector<device_binding_t> device_bindings;
```

**Coexistence with the legacy arrays:** `padconfig[]`, `keyconfig[]`, `axis[]`, `invert[]`,
`asettings[]`, `pad_id`, `analog` stay exactly as they are. They remain the **only** source when
`bindings_active == false` (legacy config.xml) and stay live for the SmartyPi-only slots the matrix
does not cover: `padconfig[12..14]` (motor limit switches, input.cpp:599-601) and `axis[3]`
(cabinet motor axis, input.cpp:493-497). `keyconfig[]` is *the* keyboard-column store — the matrix
edits it in place (no new keyboard schema).

### A.2 Config-file schema (config.xml, via SE's existing `xml_parser::ptree`)

The whole schema is expressible with the existing path API (`get_int/get_string/put_*` and
`<xmlattr>` attribute paths, `xml_parser.h:195-249,393-443`) — no raw tinyxml2 iteration is needed,
because one-binding-per-cell maps to one path-addressable element per cell.

```xml
<controls>
    <!-- existing keys untouched: gear, steerspeed, pedalspeed, keyconfig.*, padconfig.*,
         analog/@enabled, pad_id, analog.axis.*, analog.wheel.zone, analog.wheel.dead,
         analog.haptic.*, rumble -->

    <input_mode>0</input_mode>            <!-- 0 = GAMEPAD, 1 = WHEEL. Absent -> migrated (A.2.2) -->

    <bindings>                            <!-- Absent -> legacy padconfig/axis path (bindings_active=0) -->
        <gamepad>
            <steer type="axis"   index="0" device="030000005e0400008e02000014010000|A6|B11|H1"
                   vid="045e" pid="028e" name="Xbox 360 Controller" invert="0"/> <!-- axis only; set automatically at capture -->
            <accel type="axis"   index="5" device="..." vid="045e" pid="028e" name="..."/>
            <brake type="axis"   index="4" device="..." vid="045e" pid="028e" name="..."/>
            <gear1 type="button" index="0" device="..." vid="045e" pid="028e" name="..."/>
            <!-- one element per bound target; unbound cells are simply absent -->
        </gamepad>
        <wheel>
            <steer type="axis"   index="0" device="0300...b66d...|A5|B13|H1" vid="044f" pid="b66d" name="T150 Racing Wheel"/>
            <accel type="axis"   index="2" device="0300...|A3|B0|H0"        vid="1dd2" pid="0001" name="Sim Pedals"/>
            <start type="hat"    index="0" value="1" device="..." vid="044f" pid="b66d" name="..."/>
        </wheel>
    </bindings>
</controls>
```

Element names are the `Target` enum lower-cased (`steer accel brake gear1 gear2 start coin menu
view up down left right`); `type` is `axis|button|hat`; `value` only for `hat` (SDL_HAT_* mask).
Human-editable on a headless Pi over ssh, which is the point of attributes rather than DX's single
`;`/`,`-joined string (`DX config.cpp:392-416`).

#### A.2.1 Load / save (config.cpp)

- **Load** (append to the controls block after config.cpp:332):
  `controls.input_mode = cfg.get_int("controls.input_mode", -1)`;
  `bindings_active = cfg.find_node("controls.bindings") != nullptr`;
  for `group in {gamepad, wheel}` × `target in TARGET names`:
  `path = "controls.bindings."+group+"."+target`; if `cfg.find_node(path)` → read attributes
  `type,index,value,invert,device,vid,pid,name`; validate (`index>=0`, `type` in range, `device`
  non-empty, STEER must be `axis`); push. Malformed cells are skipped with one `std::cerr` line.
  `invert` (axis only) defaults from the legacy `controls.invert[1]/[2]` flags for ACCEL/BRAKE, 0
  otherwise, when absent (A.3.4).
- **Save** (append after config.cpp:483): `put_int("controls.input_mode")`; if `bindings_active`:
  for every cell path either `put_attribute` the eight attributes or `cfg.erase(path)` when unbound.
  Never write `<bindings>` unless `bindings_active` (so an untouched legacy config.xml round-trips
  byte-identical apart from `input_mode`).
- `Config` gets three small helpers (config.hpp): `const device_binding_t* find_binding(group,
  target) const`, `void set_binding(const device_binding_t&)` (replaces the cell), `void
  clear_binding(group, target)`. Menu and Input use these — no direct vector surgery in three files
  as in DX.

#### A.2.2 Backward compatibility and migration

| Situation | Behaviour |
|---|---|
| Old config.xml, no `<bindings>`, no `<input_mode>` | `bindings_active=false`: runtime uses the legacy single-device path against joystick index `pad_id` exactly as today (A.3.3). `input_mode` defaults GAMEPAD. Nothing is written until the user saves. |
| First time the matrix editor is opened with `bindings_active=false` | Editor **pre-populates** cells from the legacy arrays for the `pad_id` device so nothing is lost: if that device `SDL_IsGameController` → `padconfig[0..11]`/`axis[0..2]` become GAMEPAD cells (they hold GC enums after `bind_*` defaulting, input.cpp:226-234); else they become WHEEL cells (raw indices). `bindings_active=true` from this point; the legacy arrays are left as-is in the file. |
| `<bindings>` present, `<input_mode>` absent | DX rule (`DX config_base.hpp:316-335`): WHEEL if any wheel-group cell exists, else GAMEPAD. |
| Recognised GameController attached with **zero** GAMEPAD cells for its signature | `Input::materialise_default_gamepad_profile()` creates the SE default profile in memory (LEFTX→STEER, TRIGGERRIGHT→ACCEL, TRIGGERLEFT→BRAKE, A→GEAR1, B→GEAR2, START→START, Y→COIN, BACK→MENU, X→VIEW, DPAD→UP/DOWN/LEFT/RIGHT — the table at input.cpp:83-97) so a fresh pad works out of the box; persisted on the next `config.save()`. Equivalent of DX `materialize_default_gamepad_bindings` (`DX config.cpp:263-327`). |
| Raw (non-DB) device only, no cells | Nothing implicit except the **navigation defaults** (A.3.2) so the menu is reachable to bind it. |

### A.3 `Input` class changes (`src/main/sdl2/input.hpp` / `input.cpp`)

#### A.3.1 Device model

```cpp
struct InputDevice
{
    SDL_Joystick*       joystick   = nullptr;
    SDL_GameController* controller = nullptr;   // non-null iff SDL_IsGameController(index) at open
    SDL_JoystickID      instance_id = -1;
    std::string         guid_string, signature, name;
    uint16_t            vid = 0, pid = 0;
    int                 axes = 0, buttons = 0, hats = 0;
    bool                legacy_device = false;  // opened at index == controls.pad_id (fallback path)

    // Rebuilt by rebuild_binding_cache(); evaluated per event without any allocation.
    struct Resolved { uint8_t type, index; uint8_t hat_mask; uint8_t target; };
    std::vector<Resolved> resolved[device_binding_t::GROUP_COUNT];
};
```

`Input` replaces `SDL_Joystick* stick; SDL_GameController* controller; SDL_Haptic* haptic; int
hidraw_device` (input.hpp:99-102) with `std::vector<InputDevice> devices` plus:

```cpp
    void open_joy();                              // Stage 2: == scan_joysticks()
    void add_joystick(int device_index);          // SDL_JOYDEVICEADDED (which = device index)
    void remove_joystick(SDL_JoystickID id);      // SDL_JOYDEVICEREMOVED (which = instance id)
    void close_joy();                             // closes all
    const InputDevice* find_device(SDL_JoystickID) const;   // linear over <= a handful
    void rebuild_binding_cache();                 // after load/edit/hot-plug; also derives `analog`
    void reset_keys();                            // clears keys[] contributions from joystick groups
```

`main.cpp:269-275` changes to pass `event.jdevice.which` to `add_joystick`/`remove_joystick`.
SDL delivers `SDL_JOYDEVICEADDED` for already-attached devices on the first poll, so `add_joystick`
de-duplicates on instance id (DX `input.cpp:345-352`). `SDL_CONTROLLERDEVICE*` events are ignored;
the controller handle is opened inside `add_joystick` when `SDL_IsGameController(index)`.

#### A.3.2 Event routing and evaluation

| SDL event | Handler (existing names kept, main.cpp:241-267 unchanged) | Group evaluated |
|---|---|---|
| `SDL_JOYAXISMOTION/BUTTON*/HATMOTION` | `handle_joy_axis/down/up/hat` | **WHEEL** — raw index against `resolved[GROUP_WHEEL]` of `find_device(evt->which)` |
| `SDL_CONTROLLERAXISMOTION/BUTTON*` | `handle_controller_axis/down/up` | **GAMEPAD** — GC enum against `resolved[GROUP_GAMEPAD]` |
| keyboard | `handle_key_down/up` → `handle_key()` | unchanged (29258db lives here) |

A GameController device emits **both** streams for the same physical control (SDL always does).
This is fine because each stream is only matched against its own group's cells.

**Group gating (DX semantics, `DX input.cpp:157-164`):** in `cannonball::STATE_MENU` both groups
are evaluated (any device navigates); in `STATE_GAME` only `controls.input_mode`'s group is. The
keyboard is always live. On entering the game and on input-mode change, `reset_keys()` clears any
held contribution from the now-inactive group. When `capture_group != -1` (editor listening) neither
joystick group updates `keys[]`/axes.

**Navigation defaults** (so a fresh raw device can reach the editor): if a device has **no**
`TARGET_UP/DOWN/LEFT/RIGHT` cells in a group, hat 0 (WHEEL) / D-pad buttons (GAMEPAD) map to
UP/DOWN/LEFT/RIGHT implicitly — this is exactly SE's current `handle_joy_hat` (input.cpp:604-612)
and D-pad default (input.cpp:94-97), retained as fallback rather than removed.

Per-target action (`Input::apply_target(target, pressed)`): ACCEL/BRAKE/GEAR1/GEAR2/START/COIN/
MENU→`keys[MENU]`/VIEW→`keys[VIEWPOINT]`/UP/DOWN/LEFT/RIGHT set `keys[]`. `keys[]` remains a plain
bool array (no reference counting) — two bindings to one target is legal but release of either
releases the target, same as today's `handle_joy()`.

#### A.3.3 Legacy fallback path (`bindings_active == false`)

The existing `handle_axis()` (input.cpp:444-499), `handle_joy()` (input.cpp:583-602),
`store_last_axis()` / `get_axis_config()` (input.cpp:520-556) and `scale_trigger()` are **kept
verbatim** and are called only for the device flagged `legacy_device` (the one at index `pad_id`,
as `open_joy` opens today, input.cpp:72-75). This is what makes Stage 2 ship without a behaviour
change for existing users. They are deleted in Stage 3 cleanup only after the editor migrates
configs (A.2.2). `SW_LEFT/CENTRE/RIGHT` motor limits (`padconfig[12..14]`) and `axis[3]` stay on this
path permanently (SmartyPi cabinet only).

#### A.3.4 Axis mapping — steering, pedals, deadzone/zone

`Input::apply_axis(const InputDevice&, group, target, int16_t value)`:

- **STEER** (DX `input.cpp:880-921`, symmetric, reaches both `0x40` and `0xC0`, `0` → exactly `0x80`):
  ```
  raw = value
  if (group == GROUP_WHEEL && 0 < wheel_zone < 100)  raw = raw * 100 / (100 - wheel_zone)   // saturation zone
  raw = clamp(raw, -32768, 32767)
  adjusted = 0x80 + (raw >= 0 ? raw * 0x40 / 32767 : raw * 0x40 / 32768)
  adjusted = clamp(adjusted, 0x40, 0xC0)
  if (group == GROUP_WHEEL && wheel_dead && |0x80 - adjusted| <= wheel_dead)  adjusted = 0x80
  a_wheel = adjusted
  ```
  `wheel_zone/wheel_dead` come from `asettings[]` as today (input.cpp:65-66) and apply to the
  **WHEEL group only** (DX `4490269`) — a thumbstick with a 75 % zone is unusable.
- **ACCEL/BRAKE axis:** GAMEPAD `TRIGGERLEFT/RIGHT` are `0..32767` → `value/0x80`; anything else
  is `-32768..32767` → `(value+0x8000)/0x100`; clamp `0..0xFF`.
- **Invert:** the per-binding `device_binding_t::invert` flag negates `value` before any of the
  mapping above runs - `apply_axis` takes it as a parameter and applies it uniformly to STEER as
  well as ACCEL/BRAKE, so a pedal that rests high and goes negative when pressed, or a wheel that
  reports increasing values when turned left, works without hand-editing config.xml. It is set
  automatically at capture (A.3.5) and is authoritative on this path; the legacy `invert[1]/[2]`
  flags only seed it at migration/load (A.2.1) and remain in use on the legacy `handle_axis()` path
  (input.cpp, unchanged), which never gained a STEER invert.
- **Derived `analog`** (replaces the manual ANALOG OFF/ON/ON-WHEEL-ONLY entry when
  `bindings_active`): in `rebuild_binding_cache()`, for the active group
  `analog = steer_axis_bound ? (accel_axis_bound && brake_axis_bound ? 1 : 2) : 0`. `oinputs.cpp:54-75`
  consumes `input.analog` unchanged. `gamepad = !devices.empty()` (oinputs.cpp:54 gate).

#### A.3.5 Capture API (Stage 3) — used only by the editor

```cpp
    struct Captured { enum Kind { NONE, AXIS, BUTTON, HAT } kind = NONE;
                      SDL_JoystickID device = -1; int index = -1; int hat_value = 0;
                      int delta = 0; };  // AXIS only: value - baseline at qualification (sign = direction)
    void     begin_capture(int group);            // snapshots every axis baseline, flushes queued
                                                  // SDL_JOYAXISMOTION/SDL_CONTROLLERAXISMOTION, clears latch
    void     end_capture();                       // capture_group = -1
    Captured poll_capture();                      // first qualifying event since begin; cleared on read
    bool     control_released(const Captured&) const;   // for the wait-for-release phase
```

- GAMEPAD capture listens to `SDL_CONTROLLER*` events only (index = GC enum); WHEEL capture listens
  to `SDL_JOY*` events only (raw index). The other stream is ignored while capturing — this is what
  keeps a dual-stream device from binding the wrong index (DX `menu.cpp:1084-1087`).
- **Axis qualification** is delta-from-baseline: `|value - baseline| >= SDL_JOYSTICK_AXIS_MAX/5`
  (DX `input.cpp:947-1060`). A pedal resting at `-32768`, a noisy centred wheel, or a stale
  multi-axis report queued before the cell opened cannot self-bind. First crossing wins; later events
  are ignored until `end_capture`.
- Only STEER accepts AXIS. ACCEL/BRAKE accept AXIS, BUTTON or HAT. Others accept BUTTON or HAT.
- `key_press` (input.hpp:60) is reused unchanged for the keyboard column.
- `Captured.delta` records the signed movement from baseline, and the editor derives the
  per-binding `invert` flag from its sign when the accepted control is an axis: for ACCEL/BRAKE,
  pressing must increase the value (`invert = delta < 0`); for STEER, the capture prompt asks for a
  LEFT turn, which is negative in SDL's convention (`invert = delta > 0`). Every other target is
  digital-only and always gets `invert = false`.

Existing public latches `joy_button`, `get_axis_config()` remain for the legacy wizard until it is
deleted in Stage 3.

### A.4 Menu / UI (built on SE's `Menu`, `menu.cpp` — no `MenuBase`)

#### A.4.1 Controls menu (`Menu::populate_controls()`, menu.cpp:248-267)

```
GEAR <label>                       (existing)
INPUT MODE <GAMEPAD|WHEEL>         NEW  ENTRY_INPUT_MODE   — cycles controls.input_mode; refresh; input.reset_keys()
CONFIGURE INPUTS                   NEW  ENTRY_CONFIGINPUTS — state = STATE_REDEFINE_JOY (matrix editor)
REDEFINE KEYS                      existing wizard (menu.cpp:1535-1575) — kept in Stage 3, retire later
DIGITAL STEER SPEED n / DIGITAL PEDAL SPEED n   (existing)
RUMBLE AND FEEDBACK...             NEW  ENTRY_FEEDBACK     — submenu (Deliverable B, B.6)
BACK
```
`menu_controls_gp` (ANALOG / RUMBLE STRENGTH / REDEFINE GAMEPAD, menu.cpp:260-266) is retired;
its vector in `menu.hpp:98` becomes `menu_feedback`. `ENTRY_CONFIGUREGP` gating on `input.gamepad`
(menu.cpp:254) goes — CONFIGURE INPUTS is always shown (the keyboard column is always editable).
`tick_menu()` handlers at menu.cpp:1066-1118 and `refresh_menu()` at menu.cpp:1459-1469 are
updated accordingly. New labels in `menulabels.hpp`:
`ENTRY_INPUT_MODE "INPUT MODE "`, `ENTRY_CONFIGINPUTS "CONFIGURE INPUTS"`,
`ENTRY_FEEDBACK "RUMBLE AND FEEDBACK..."`, `INPUT_MODE_LABELS[2] = {"GAMEPAD","WHEEL"}`.

#### A.4.2 Matrix editor — `Menu::redefine_joystick()` rewritten (menu.cpp:1577-1616), same state

Reached via `state = STATE_REDEFINE_JOY` and dispatched from `tick_ui()` (menu.cpp:427-430) as
today. Editor state moves from DX's function `static`s (`DX menu.cpp:810-822`) into `Menu` members
(`menu.hpp`): `int16_t bind_row, bind_col; uint8_t bind_phase; Input::Captured bind_wait;
int32_t bind_msg_timer; std::string bind_msg;`.

**Layout** (40×28 text grid, `ohud.blit_text_new(x, y, text, colour)`; DX `menu.cpp:869-978`):

```
y=2   x=12  CONTROL BINDINGS
y=4   x=1   CONTROL     x=14 KEYBOARD   x=24 GAMEPAD   x=33 WHEEL        (selected column PINK)
y=6.. x=1   STEERING    (kbd cell "-", not selectable)   AX0/LX    AX0
      ...   ACCELERATE, BRAKE, GEAR LOW, GEAR HIGH, START, COIN, MENU, VIEW CHANGE,
      y=18  UP, DOWN, LEFT, RIGHT                       (13 rows, y = 6 + row)
y=20  x=18  BACK
y=22  x=1   status line 1:  ARROWS - SELECT   ENTER - CHANGE
y=23  x=1   status line 2:  DEL/BSP - CLEAR   MENU - EXIT
y=24  x=1   status line 3:  WHEEL = ALL RAW DEVICES, NO DB NEEDED
```

Row → keyboard slot: rows 1..12 map to `keyconfig[]` as `ACC=4 BRAKE=5 GEAR1=6 GEAR2=7 START=8
COIN=9 MENU=10 VIEW=11 UP=0 DOWN=1 LEFT=2 RIGHT=3` (config.cpp:291-302 order). Cell text ≤ 8 chars:
keyboard `SDL_GetKeyName` compacted (DX `menu.cpp:224-246`); GAMEPAD cells use
`SDL_GameControllerGetStringForAxis/Button` upper-cased and clipped (`LEFTX`, `RIGHTTR`, `A`,
`START`); WHEEL cells `AX0`, `B12`, `H0U`; unbound `-`; bound-but-absent device → same text in GREY.

**Phases** (`bind_phase`): `BROWSE` → `WAIT_RELEASE` → `CAPTURE` → `WAIT_RELEASE` → `BROWSE`.

- **BROWSE:** `has_pressed(UP/DOWN)` moves rows (wraps; BACK row forces col 0), `LEFT/RIGHT` moves
  columns (not on BACK row; STEERING row skips the keyboard column). `select_pressed()`
  (menu.cpp:1215-1223: START/ACCEL/GEAR1 or cabinet select) **or** `key_press == SDLK_RETURN`
  activates the cell — but first records the activating control into `bind_wait` and enters
  `WAIT_RELEASE` so the button that opened the cell cannot be captured as the binding (DX
  `menu.cpp:1329-1359`). `key_press == SDLK_DELETE/BACKSPACE` clears the cell
  (`config.clear_binding` or `keyconfig[slot] = -1`) and marks `input.rebuild_binding_cache()`.
  `has_pressed(MENU)` or BACK exits (`Menu::leave_editor()` → `end_capture`, `rebuild_binding_cache`,
  `state = STATE_MENU`, `refresh_menu()`).
- **WAIT_RELEASE:** waits until `input.control_released(bind_wait)` (keyboard: scancode up via
  `SDL_GetKeyboardState`; button/hat: latch cleared). Then `CAPTURE` if a capture was requested,
  else BROWSE. Status shows `RELEASE CONTROL`.
- **CAPTURE:** keyboard column: waits on `input.key_press != -1`, writes `keyconfig[slot]`.
  GAMEPAD/WHEEL: `input.begin_capture(group)` on entry; each tick `poll_capture()`; on a qualifying
  event that the row accepts → `config.set_binding({group, target, type, index, value, signature,
  vid, pid, name})`, `end_capture()`, `bind_msg = "BOUND " + device name + " " + cell text` for 2 s,
  then `WAIT_RELEASE` on the captured control. Prompts: STEERING `MOVE STEERING AXIS`; ACCEL/BRAKE
  `MOVE AXIS OR PRESS BUTTON`; others `PRESS BUTTON OR HAT`; keyboard `PRESS A KEY`.
  Keyboard `MENU` key aborts capture (keyboard stays live during capture; joystick groups do not).

**Escape semantics — decision:** SE's `master_break_key` (ESC, main.cpp:231) **quits the program**
and is not repurposed (DX rebound it to BACK). Backing out of the editor is the MENU action
(`m`/F5 or a bound MENU button) or the BACK row. The status line says so.

Every write goes through `config` and is persisted by the existing **SAVE AND RETURN** entry
(menu.cpp:544-547) — SE keeps its explicit save (DX's `config_save_pending` autosave is not
adopted). Leaving the editor calls `input.rebuild_binding_cache()` so bindings are live immediately.

### A.5 Staging plan — each stage compiles and ships alone

| Stage | Ships | Files | User-visible |
|---|---|---|---|
| **1 — Data model + config** | `device_binding_t`, `controls_settings_t` additions, load/save/migration helpers, `input_mode`; rumble master clamp + `RUMBLE_LABELS` clamp; `wheel.zone` default 75→0 | `config.hpp`, `config.cpp`, `menu.cpp:1468`, `res/config.xml` (comments for new keys) | None. Bindings are loaded/saved but not evaluated. Hand-editable config.xml. |
| **2 — Multi-device runtime** | `InputDevice` vector, hot-plug by instance id, `rebuild_binding_cache`, group gating, navigation defaults, default GC profile materialisation, DX steering curve (WHEEL-only zone/dead), derived `analog`; legacy path kept for `bindings_active=false` | `input.hpp`, `input.cpp`, `main.cpp:269-275` | Existing configs: identical behaviour. Users can bind any raw device by editing config.xml. INPUT MODE entry in menu (small, `populate_controls`/`tick_menu`/`refresh_menu`). |
| **3 — Capture API + matrix editor** | `begin/poll/end_capture`, `Captured`; `redefine_joystick()` rewritten; CONFIGURE INPUTS entry; `menu_controls_gp` retired; legacy `store_last_axis/get_axis_config/joy_button` wizard code deleted; first-open migration of legacy arrays | `input.hpp/.cpp`, `menu.hpp`, `menu.cpp`, `menulabels.hpp` | The feature. Hardware gate before merge: Pi + DB-known pad, non-DB raw joystick/GPIO encoder, wheel+pedals (two devices), keyboard only, hot-plug/reconnect order. |
| **4 — Rumble subsystem** (Deliverable B; independent of 3, needs 2 for per-device handles) | 4a SDL/haptic/FFB backends + mixer + game-side API + FEEDBACK submenu; 4b HID profiles + custom editor + VID/PID suggest; 4c optional richer effects | new `sdl2/rumble*.{hpp,cpp}`, `main.cpp:345-352,1134-1143,211-219`, `input.cpp` (remove hidraw + `set_rumble`), `menu.*`, `CMakeLists.txt` | Rumble backend/profile selectable and testable in-menu. |

---

## Deliverable B — Rumble subsystem

### B.1 What is wrong with `Input::set_rumble()` today (input.cpp:614-678)

1. **Hard-coded vendor bytes**: `{0x01,0xF0,0x00}` ON, `{0x00,0xA0,0xA0}` skid pulse,
   `{0x00,0x00,0x00}` OFF, written to whatever `/dev/hidrawN` udev found for the *opened
   controller's* VID/PID (input.cpp:109-181). They are Thrustmaster report semantics (byte 0 = 0x00
   both motors / 0x01 HF only; bytes 1-2 = intensity high nibble) but would be written to **any**
   device whose SDL rumble probe fails, since no VID/PID check exists beyond "same as the joystick".
2. **Unbounded writes**: one `write()` per tick (30/s) regardless of change; errors ignored.
3. **No user control**: no way to pick a backend, no way to try other bytes without recompiling
   (`main.cpp:1129-1131` literally says "Input::set_rumble() needs to be amended for the specific
   controller").
4. **Mixed concerns**: hidraw discovery, SDL haptic, DirectInput/evdev FFB and gamepad rumble all
   inside `Input`; `forcefeedback::set()` is shared between rumble (input.cpp:663-667) and the
   SmartyPi cabinet motor (`ooutputs.cpp:823-836`).
5. Windows has no raw-report path at all (`#ifndef WIN32`).

### B.2 Architecture

New module **`src/main/sdl2/rumble.hpp`**, **`rumble.cpp`** (mixer, backend selection, safety),
**`rumble_backend_sdl.cpp`** (GameController/Joystick/Haptic), **`rumble_backend_hid.cpp`**
(profiles table, hidraw + `SDL_hid_*`), **`rumble_backend_ffb.cpp`** (adapter over the untouched
`forcefeedback::` namespace). Added to `CMakeLists.txt` next to `sdl2/input.cpp` (:145-153); udev
link stays as is (:387-394); Windows keeps `dinput8 dxguid winmm` (:416-418).

```
game state / main loop ──► rumble::set_level()/event() ──► rumble::tick() ──► RumbleFrame{low,high,ms}
                                                              │  (priority mixer + master + safety)
                                                              ▼
                                       RumbleBackend* active ─┬─ SdlControllerBackend   (SDL_GameControllerRumble / SDL_JoystickRumble)
                                                              ├─ SdlHapticBackend       (SDL_HapticRumblePlay)
                                                              ├─ HidReportBackend       (profile → bytes → hidraw | SDL_hid_write)
                                                              ├─ FfbBackend             (forcefeedback::set — evdev / DirectInput)
                                                              └─ NullBackend
```

#### B.2.1 Game-side API (platform-agnostic; the only thing engine/main code sees)

```cpp
namespace rumble
{
    enum Effect { MOTOR, SKID, CRASH, GEAR, START_REV, TEST, EFFECT_COUNT };

    void init();                                   // main.cpp after input.init (:1134)
    void shutdown();                               // quit_func (main.cpp:211-219): OFF + close
    void on_devices_changed();                     // from Input add/remove_joystick; re-selects target + backend
    void set_master(float strength01);             // config.controls.rumble
    void set_level(Effect, float intensity01);     // continuous; caller refreshes every tick_frame
    void event(Effect, float intensity01, uint16_t duration_ms);   // one-shot
    void tick();                                   // once per tick_frame: mix → safety → backend
    void stop_all();                               // menu entry, mode switch, editor open, device loss

    // Menu/config surface
    enum Backend { AUTO, SDL_CONTROLLER, SDL_HAPTIC, HID_REPORT, FFB, OFF };
    Backend    configured_backend();  void set_backend(Backend);   // persists controls.rumble_backend
    const char* active_backend_name();                              // for the menu ("SDL", "HID: THRUSTMASTER", "NONE")
    bool        test(Effect, uint16_t ms);                          // menu TEST; same limiter
}
```

**Call-site change** (`main.cpp:345-352`), preserving today's exact semantics:
```cpp
if (tick_frame) {
    rumble::set_level(rumble::MOTOR, outrun.outputs->is_set(OOutputs::D_MOTOR) ? 1.f : 0.f);
    rumble::set_level(rumble::SKID,  (outrun.SkiddingOnRoad() && outrun.game_state == GS_INGAME) ? 1.f : 0.f);
    rumble::tick();
}
```
MOTOR follows `D_MOTOR` in every state (as now), so the SmartyPi MOTOR TEST (`cabdiag.cpp:323-343`)
still drives rumble. `D_MOTOR` already carries the arcade's speed/off-road/crash/skid vibration
pattern (`ooutputs.cpp:852-985`), which is why SE's baseline effect set is authentically small.

#### B.2.2 Mixer (`rumble.cpp`)

Internal canonical frame: `struct RumbleFrame { uint16_t low, high, duration_ms; }` (SDL
`SDL_GameControllerRumble` semantics: `low` = strong/low-frequency motor, `high` = weak/HF motor).

| Effect | Source | low % | high % | duration | Notes |
|---|---|---|---|---|---|
| MOTOR | `set_level` from `D_MOTOR` | 100 | 60 | 45 ms, refreshed each tick | continuous while set |
| SKID | `set_level` from `SkiddingOnRoad` | 0 | 100 | 45 ms, refreshed | DX `gamepad_rumble_state.hpp:289-296` |
| CRASH (4c) | `ocrash.crash_counter` rising edge, polled in `tick()` | 100 | 100 | 160 ms | DX `:201-226` |
| GEAR (4c) | `oinputs.gear` change, `GS_INGAME` | 60→20 | 25→40 | 95 ms | DX `:234-248` |
| START_REV (4c) | `GS_START1..3`, `oinputs.acc_adjust` ramp | ½ level | level | continuous | DX `:151-177,257-261` |
| TEST | menu | as requested | | ≤ 500 ms | |

Priority when several are non-zero: CRASH > GEAR > START_REV > MOTOR > SKID (highest wins; no
summing, so a stuck level can never exceed master). Output = `percent × master × 65535`. Duration
45 ms > 33 ms tick, so continuous levels are seamless and self-expire if `tick()` stops.
Stage 4c derives its extra events by **polling engine globals inside `rumble.cpp`** (as DX does)
so no engine file is touched; it is optional and off by default (`controls.rumble_extras`).

#### B.2.3 Backends (`class RumbleBackend { open(const InputDevice&); play(RumbleFrame); stop(); close(); name(); }`)

| Backend | API | Platform | Effect mapping |
|---|---|---|---|
| `SDL_CONTROLLER` | `SDL_GameControllerRumble` (GC devices) / `SDL_JoystickRumble` (raw) — SDL ≥ 2.0.9. Probe = call with `(0,0,0) == 0` (works on 2.0.9-2.0.17 where `SDL_GameControllerHasRumble` (2.0.18) is absent; DX `input.cpp:305-306`) | Linux, Windows | `(low, high, ms)` direct |
| `SDL_HAPTIC` | `SDL_HapticOpenFromJoystick` + `SDL_HapticRumbleInit/Play/Stop` (existing input.cpp:184-192, 671-676) | Linux, Windows | `strength = max(low,high)/65535`, `Play(strength, ms)` |
| `HID_REPORT` | profile bytes → Linux `/dev/hidrawN` (existing udev VID/PID discovery, input.cpp:123-180, moved verbatim) **or** `SDL_hid_open(vid,pid)`/`SDL_hid_write` when `SDL_VERSION_ATLEAST(2,0,18)` (Windows and newer Linux) | Linux (hidraw always; SDL_hid if ≥2.0.18), Windows (SDL_hid only) | B.3 |
| `FFB` | existing `forcefeedback::init/set/close` (ffeedback.cpp; evdev on Linux, DirectInput on Windows) | Linux, Windows | `level = 1 + int((1 - max(low,high)/65535) * 4)` (input.cpp:665), `set(effect==SKID ? 1 : 0, level)` each tick while non-zero — unchanged semantics |
| `OFF` | — | — | — |

**AUTO selection order** (evaluated in `on_devices_changed()` for the target device):
`SDL_CONTROLLER` → `SDL_HAPTIC` → `HID_REPORT` *only if a built-in or custom profile matches the
device's VID/PID* → `FFB` (only if `controls.analog.haptic/@enabled` and not a SmartyPi moving
cabinet, whose motor owns `forcefeedback::`) → `OFF`. This reproduces today's outcome for the
Thrustmaster (SDL probe fails → profile matches → HID) while never writing raw bytes to an
unknown device.

**Target device**: the device holding the active group's STEER cell; else the first device with any
cell in the active group; else the legacy device; else none. Overridable by
`controls.rumble_device` (a signature) for cabinets with a separate rumble pad.

### B.3 Raw-HID "rumble device profile"

#### B.3.1 Profile model (`rumble_backend_hid.cpp`)

```cpp
struct HidRumbleProfile
{
    const char* id;            // "thrustmaster3", "custom"
    const char* name;          // menu text
    uint16_t vid, pid;         // pid 0 = any product of this vendor
    uint8_t  report_len;       // 1..64 bytes, excluding report id
    int16_t  report_id;        // -1 = device uses unnumbered reports (raw bytes written as-is);
                               // 0..255 = prefix byte. hidraw needs the prefix only for numbered
                               // reports; SDL_hid_write ALWAYS needs a leading id byte (0 if none) —
                               // the backend adds/strips it, the profile states the device's truth.
    enum Mode { PRESETS, INTENSITY } mode;
    uint8_t  on[64], off[64], pulse[64];   // PRESETS: exact byte images (today's behaviour)
    uint8_t  tmpl[64];                     // INTENSITY: template
    int8_t   pos_low, pos_high;            // INTENSITY: byte index carrying each motor, -1 = none
    enum Scale { NIBBLE_HI /*0xN0*/, BYTE /*0x00..0xFF*/ } scale;
    uint16_t keepalive_ms;                 // 0 = only write on change; else re-send last non-OFF report
};
```

Built-in table (Stage 4b), byte-for-byte the current code so behaviour is unchanged for that user:

| id | name | vid/pid | len | mode | ON | OFF | PULSE |
|---|---|---|---|---|---|---|---|
| `thrustmaster3` | THRUSTMASTER 3-BYTE | 0x044F / any | 3 | PRESETS | `01 F0 00` | `00 00 00` | `00 A0 A0` |
| `thrustmaster3s` | THRUSTMASTER 3-BYTE SCALED | 0x044F / any | 3 | INTENSITY, tmpl `00 00 00`, pos_low=1, pos_high=2, NIBBLE_HI | | | |

The exact PID of the wheel the original code was written against is not recorded anywhere in SE
(the udev match used "whatever SDL opened"); the profile therefore matches on vendor only. Adding a
second well-known vendor later is one table row.

**Effect → profile action**: PRESETS mode: MOTOR level>0 → `on`, SKID → `pulse`, any one-shot →
`on` for its duration, all-zero → `off` (identical to input.cpp:629-655). INTENSITY mode: copy
`tmpl`, write `scale(low)` at `pos_low` and `scale(high)` at `pos_high`.

#### B.3.2 Custom / advanced profile

One custom profile (id `custom`) stored in config.xml (B.4), edited in a dedicated screen
(B.6.2) with live TEST. It is only **armed** for in-game use when its `vid/pid` equals the target
device's — the editor offers `USE CURRENT DEVICE` to copy them. Without a match it can still be
TESTed from the editor, but each test requires the device row to show the same VID/PID (guards
against writing arbitrary bytes into a different HID device).

#### B.3.3 VID/PID detection and auto-suggest

`Input::add_joystick` records `vid/pid` (`SDL_JoystickGetVendor/Product`, SDL ≥2.0.6). The
FEEDBACK menu shows `DEVICE: <name> 044F:B66D` and, when a built-in profile matches and the SDL
probe failed, `HID PROFILE: THRUSTMASTER 3-BYTE (SUGGESTED)` pre-selected under AUTO. When the
user explicitly selects `RUMBLE BACKEND: HID REPORT`, the profile entry cycles through matching
built-ins + CUSTOM.

### B.4 Config schema (config.xml)

```xml
<controls>
    <rumble>1.0</rumble>                       <!-- existing master strength; clamped 0..1 on load -->
    <rumble_backend>auto</rumble_backend>      <!-- auto | sdl | haptic | hid | ffb | off -->
    <rumble_device></rumble_device>            <!-- optional signature override for the rumble target -->
    <rumble_extras>0</rumble_extras>           <!-- Stage 4c richer effects (crash/gear/start) -->
    <rumble_hid>
        <profile>thrustmaster3</profile>       <!-- built-in id, or "custom" -->
        <custom vid="044f" pid="b66d" len="3" report_id="-1" mode="presets"
                on="01 F0 00" off="00 00 00" pulse="00 A0 A0"
                template="00 00 00" low_byte="1" high_byte="2" scale="nibble" keepalive_ms="0"/>
    </rumble_hid>
</controls>
```
All via the existing `ptree` path/attribute API (`controls.rumble_hid.custom.<xmlattr>.on` etc.).
Byte strings are space-separated hex, validated on load (length == `len`, each token `00..FF`);
an invalid custom block is ignored with one log line and the profile falls back to `auto`.

### B.5 Effect mapping across platforms — one call site, same effect set

| Effect source (game) | Linux | Windows |
|---|---|---|
| `D_MOTOR` / skid / one-shots → `RumbleFrame` | `SDL_GameControllerRumble` (XInput-style pads, Switch/PS via SDL HIDAPI driver) → `SDL_Haptic` → **hidraw** (profile; `O_RDWR|O_NONBLOCK|O_CLOEXEC`) or `SDL_hid_write` (≥2.0.18) → **evdev FF_RUMBLE/FF_PERIODIC** bank (ffeedback.cpp:41-130) | `SDL_GameControllerRumble` (XInput) → `SDL_Haptic` → **`SDL_hid_write`** (profile, ≥2.0.18; hidapi requires the leading report-id byte, added by the backend) → **DirectInput** constant-force (ffeedback.cpp:278-339) |

The mixer, profiles, config and menu are shared; only `RumbleBackend::open/play` differ per OS.
`Input` no longer contains any `#ifndef WIN32` rumble code.

### B.6 Menu UX

#### B.6.1 `RUMBLE AND FEEDBACK...` submenu (`menu_feedback`, label-based like every SE submenu)

```
RUMBLE STRENGTH <OFF|LOW|MED|HIGH|FULL>     existing ENTRY_RUMBLE, index clamped 0..4
RUMBLE BACKEND <AUTO|SDL|SDL HAPTIC|HID REPORT|FORCE FEEDBACK|OFF>   ENTRY_RUMBLE_BACKEND; refresh_menu shows "(ACTIVE: SDL)" via rumble::active_backend_name()
HID PROFILE <THRUSTMASTER 3-BYTE|...|CUSTOM>   ENTRY_RUMBLE_PROFILE; shown only when backend is HID or AUTO-suggested
TEST RUMBLE                                 ENTRY_RUMBLE_TEST → rumble::test(TEST, 400) ; display_message(active backend or "NO RUMBLE DEVICE")
ADVANCED HID SETUP...                       ENTRY_RUMBLE_ADVANCED → state = STATE_RUMBLE_EDITOR
BACK
```
The old `controls.analog.haptic/@enabled` (FFB on/off, config.cpp:329) stays a config.xml key; it
gates the FFB backend in AUTO.

#### B.6.2 `ADVANCED HID SETUP` — `Menu::rumble_editor()` (new `STATE_RUMBLE_EDITOR`, drawn like the matrix)

```
y=2   HID RUMBLE PROFILE EDITOR
y=4   DEVICE   T150 RACING WHEEL  044F:B66D        (rumble target; GREY if none)
y=6   PROFILE VID/PID  044F:B66D   [USE CURRENT DEVICE]
y=7   REPORT LENGTH    3          (1..16 in the UI; 64 in config)
y=8   REPORT ID        NONE|0..255
y=9   MODE             PRESETS|INTENSITY
y=11  ON     01 F0 00              \
y=12  OFF    00 00 00               |  PRESETS: three byte rows
y=13  PULSE  00 A0 A0              /
      (INTENSITY: TEMPLATE row + LOW BYTE n + HIGH BYTE n + SCALE)
y=15  KEEPALIVE        0 MS
y=17  TEST ON   TEST OFF   TEST PULSE       (each runs through rumble::test with the edited bytes; TEST ON auto-OFF after 500 ms)
y=19  SAVE AS CUSTOM    RESET TO SUGGESTED    BACK
y=22  status: UP/DOWN ROW  LEFT/RIGHT FIELD  ENTER +1  ACCEL/GEAR -1 / +16 ...
```
Byte editing: cursor over a hex byte; `select_pressed()` (ENTER/START) increments by 1,
`LEFT/RIGHT` move between bytes, `has_pressed(ACCEL)` increments by 0x10, `has_pressed(BRAKE)`
decrements by 1 (all already-bound actions — nothing new to learn). Edits live in a scratch
`HidRumbleProfile`; only `SAVE AS CUSTOM` writes `config.controls.rumble_hid.custom` and selects
`profile=custom`; SAVE AND RETURN persists. MENU exits without saving (asks nothing — scratch is
discarded). While this screen is open, `rumble::set_level` inputs are ignored (`stop_all()` on
entry) so only TEST writes happen.

### B.7 Safety — unknown or wrong codes must neither hang nor spam the device

1. **Edge-triggered writes.** `HidReportBackend::play` compares the rendered report with the last
   written bytes; identical → no write (unless `keepalive_ms` elapsed). MOTOR at 30 Hz therefore
   costs one write on rise and one on fall, not 30/s.
2. **Hard rate cap:** at most **one** HID/FFB write per `tick()` (30/s) and never from another
   thread; `SDL_*Rumble` calls likewise once per tick.
3. **Non-blocking I/O, bounded failure:** hidraw opened `O_NONBLOCK`; `write()` returning
   `-1`/short → failure counter; **3 consecutive failures → backend marked FAILED for the session**,
   one final OFF attempted, one log line, AUTO falls through to the next backend. `SDL_hid_write`
   and `SDL_GameControllerRumble != 0` handled the same way (DX `input.cpp:1536-1547` pattern).
4. **Validation before any write:** `1 ≤ len ≤ 64`, `pos_* < len`, hex tokens well-formed,
   profile `vid/pid` == target device's `vid/pid` (built-in: vendor match; custom: exact) — else
   the HID backend refuses to open. No wildcard device.
5. **Always-OFF guarantees:** `stop_all()` on `STATE_INIT_MENU`, on editor entry, on backend/profile
   change, on device removal, on input-mode change (DX `input.cpp:1455-1467` intent) and in
   `shutdown()` from `quit_func`. SDL backends use bounded durations (≤ 500 ms) so a stalled loop
   self-silences; HID/FFB (no duration) get an explicit OFF when the mixed frame becomes zero, and a
   **stale guard**: if `tick()` sees > 100 ms since its last call it emits OFF before mixing.
6. **Test path shares the limiter** and auto-stops; TEST cannot be re-triggered while a test is
   running.
7. **Permissions:** `/dev/hidraw*` is root-only on Raspberry Pi OS by default. `install.sh` should
   drop `/etc/udev/rules.d/99-cannonball-hidraw.rules`
   (`KERNEL=="hidraw*", ATTRS{idVendor}=="044f", MODE="0660", GROUP="input"`); the FEEDBACK menu
   reports `HID: PERMISSION DENIED` (from `errno == EACCES`) instead of silently falling back.

---

## Pi 2 CPU impact — immaterial, stated plainly

Both paths are event-driven and tiny relative to the 33 ms tick (16.7 ms at 60 fps):

- **(a) Input polling + binding evaluation.** `SDL_PollEvent` already calls `SDL_JoystickUpdate`
  once per frame; each extra open device adds one non-blocking evdev read (a few µs on an A7). A
  wheel at 250 Hz produces ~8 axis events per 30 Hz tick; each event is one `find_device` (linear
  over ≤ 4 devices, instance-id compare) plus ≤ 13 `Resolved` compares — no string building or
  allocation (DX's `get_device_signature()` builds a `std::string` per event, `DX input.cpp:434-438,
  807`; SE's design caches `resolved[]` per device). Worst case well under **10 µs per frame**,
  < 0.05 % of budget. Zero GPU/bandwidth impact.
- **(b) Rumble writes.** At most one 3–64-byte `write()`/`ioctl` per tick and only on change:
  ≈ 10–50 µs when it happens, typically **0–2 per second** in play. `SDL_GameControllerRumble`
  on Linux is an `EVIOCSFF` + `write`, same order.

No benchmark is warranted; the only thing worth checking with `-perftest` after Stage 2 is that the
frame time is unchanged with two devices attached (it will be).

---

## Risks and open questions

1. **SDL version floor on target images.** `SDL_GameControllerRumble`/`SDL_JoystickRumble` need
   ≥ 2.0.9; `SDL_hid_*` ≥ 2.0.18; `SDL_GameControllerHasRumble` ≥ 2.0.18. Raspberry Pi OS Bullseye
   ships 2.0.14 (GC rumble OK, **no** `SDL_hid` → hidraw only), Bookworm 2.26. CMake pins no SDL2
   version (CMakeLists.txt:298-321); guard with `SDL_VERSION_ATLEAST` and document 2.0.9 as the
   floor.
2. **GUID stability** across SDL versions/backends (CRC bytes, hidapi vs evdev, XInput GUID is
   identical for every XInput pad). Mitigated by the VID/PID+A/B/H fallback (A.1.2); residual risk:
   an SDL upgrade orphans bindings for a device with no VID/PID (some Bluetooth pads) — the editor
   shows them grey and re-binding is one action.
3. **Steering-curve change** for users with `wheel.zone > 0` (A.3.4, §0.4). Shipped default is 0;
   the config.cpp default changes 75 → 0. Release note needed.
4. **Thrustmaster profile fidelity.** Bytes are transcribed from the only spec (input.cpp:620-655);
   the exact wheel/PID is unknown. Needs the owner's wheel on a Pi to confirm before Stage 4b is
   declared done.
5. **hidraw permissions** on Pi (B.7.7). Without the udev rule the HID backend cannot open the
   device; the menu must say so rather than fall back silently.
6. **Derived `analog`** (A.3.4) replaces a user-visible setting when bindings are active. Legacy
   configs keep the manual entry. Hardware test: pad with trigger pedals, pad with shoulder-button
   pedals, wheel with pedals on a second device.
7. **`forcefeedback::` contention** between cabinet motor (`ooutputs.cpp:835`) and the FFB rumble
   backend — pre-existing; AUTO never picks FFB on a SmartyPi moving cabinet, and the explicit
   selection is the user's choice.
8. **Two identical devices** share bindings (A.1.2); a second identical pad cannot be told apart.
   Accepted.
9. **Menu select on ACCEL/GEAR1** (menu.cpp:1222) — the WAIT_RELEASE phase (A.4.2) is mandatory or
   the opening button self-binds. Test with a pad whose A button is GEAR1.
10. **Hot-plug during the game** with the rumble target removed → `on_devices_changed()` must
    `stop_all()` before re-selecting (B.7.5); an SDL handle used after removal returns errors that
    the failure counter absorbs.
11. **Hardware gate before merging Stage 3**: Pi + DB-known pad,
    unknown raw joystick/GPIO encoder, wheel + pedals (two devices), keyboard-only, hot-plug and
    reconnect-order permutations, both `input_mode`s.

---

## Recommended build order

1. **Stage 1** (config + data model + the two §0.4 clamps) — small, zero behaviour change, unblocks
   everything; can ship in the next point release.
2. **Stage 2** (multi-device runtime, legacy path intact) — ship behind `bindings_active`; existing
   users see nothing; wheel users can hand-edit config.xml on a headless Pi immediately.
3. **Stage 4a** (rumble core: mixer + SDL/haptic/FFB backends + FEEDBACK submenu; remove the hidraw
   hack from `Input`) — independent of the editor, needs Stage 2's device vector; retires the
   hard-coded bytes early. The Thrustmaster owner is served by the built-in profile in 4b, so 4a
   and 4b should land in the same release to avoid a regression window.
4. **Stage 4b** (HID profiles + custom editor + VID/PID suggest).
5. **Stage 3** (capture API + matrix editor + legacy wizard removal) — the largest UI piece; gate on
   the hardware matrix above.
6. **Stage 4c** (optional richer effects, off by default).

---

## Appendix — file / edit map

| File | Stage | Edits |
|---|---|---|
| `src/main/frontend/config.hpp` | 1 | add `device_binding_t`; `controls_settings_t` += `input_mode`, `bindings_active`, `device_bindings`; `Config::find_binding/set_binding/clear_binding`; rumble fields `rumble_backend`, `rumble_device`, `rumble_extras`, `rumble_hid_profile`, `rumble_hid_custom` (4) |
| `src/main/frontend/config.cpp` | 1, 4 | controls load block (after :332) and save block (after :483): `input_mode`, `<bindings>` cells, rumble keys; clamp `controls.rumble` 0..1 (:290); `wheel.zone` default 75→0 (:326); do **not** touch :254-258 / :437 (f82ac81) |
| `res/config.xml` | 1, 4 | commented examples for `input_mode`, `<bindings>`, `rumble_backend`, `rumble_hid` |
| `src/main/sdl2/input.hpp` | 2, 3 | `InputDevice`; replace `stick/controller/haptic/hidraw_device` (:99-102) with `devices`; `add_joystick/remove_joystick/find_device/rebuild_binding_cache/reset_keys`; `Captured` + `begin/poll/end_capture/control_released` (3); remove `set_rumble` (4) |
| `src/main/sdl2/input.cpp` | 2, 3, 4 | `open_joy` (:70-224) → `scan`; new `add_joystick` (opens joystick + GC, records vid/pid/signature); `handle_joy_*`/`handle_controller_*` (:433-612) route to `resolved[]` with legacy fallback for `legacy_device`; `apply_axis` (DX curve, WHEEL-only zone/dead); `handle_key()` **unchanged** (:300-431, 29258db); delete hidraw discovery (:109-181) and `set_rumble` (:614-678) in Stage 4; delete `store_last_axis/get_axis_config` (:520-556) in Stage 3 |
| `src/main/main.cpp` | 2, 4 | `SDL_JOYDEVICEADDED/REMOVED` (:269-275) pass `event.jdevice.which`; rumble call site (:345-352); `rumble::init()` after `input.init` (:1134); `rumble::shutdown()` in `quit_func` (:211-219); update the stale comment (:1129-1132) |
| `src/main/frontend/menu.hpp` | 2, 3, 4 | `STATE_RUMBLE_EDITOR`; editor members (`bind_row/bind_col/bind_phase/bind_wait/bind_msg*`); `menu_controls_gp` → `menu_feedback`; `rumble_editor()`, `leave_editor()` |
| `src/main/frontend/menu.cpp` | 2, 3, 4 | `populate_controls` (:248-267); `tick_menu` controls handlers (:1066-1118); `refresh_menu` (:1459-1469, clamp :1468); `redefine_joystick` rewritten (:1577-1616); `tick_ui` dispatch for the rumble editor (:418-431); keep header (:8, 688f83e) |
| `src/main/frontend/menulabels.hpp` | 2, 3, 4 | `ENTRY_INPUT_MODE`, `ENTRY_CONFIGINPUTS`, `ENTRY_FEEDBACK`, `ENTRY_RUMBLE_BACKEND`, `ENTRY_RUMBLE_PROFILE`, `ENTRY_RUMBLE_TEST`, `ENTRY_RUMBLE_ADVANCED`, `INPUT_MODE_LABELS`, `RUMBLE_BACKEND_LABELS`; retire `ENTRY_CONFIGUREGP/ENTRY_REDEFJOY/ENTRY_ANALOG` when `menu_controls_gp` goes |
| `src/main/sdl2/rumble.hpp`, `rumble.cpp` | 4a | API, mixer, backend selection, safety (B.2, B.7) |
| `src/main/sdl2/rumble_backend_sdl.cpp` | 4a | GC/Joystick/Haptic backends |
| `src/main/sdl2/rumble_backend_ffb.cpp` | 4a | adapter over `forcefeedback::` (ffeedback.cpp untouched) |
| `src/main/sdl2/rumble_backend_hid.cpp` | 4b | profile table, hidraw (udev code moved from input.cpp:123-180) + `SDL_hid_*` path, validation |
| `CMakeLists.txt` | 4 | add the four `rumble*` sources beside `sdl2/input.cpp` (:145-153) |
| `install.sh` | 4b | optional udev rule for hidraw access |
| `src/main/directx/ffeedback.cpp/.hpp`, `src/main/engine/*` | — | **not modified** |
