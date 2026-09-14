/***************************************************************************
    XML Configuration File Handling.

    Load Settings.
    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
    - Input binding matrix data model (device_binding_t, controls.device_bindings)
***************************************************************************/

#pragma once

#include <SDL.h>
#include <set>
#include <string>
#include <vector>
#include "stdint.hpp"
#include "xml_parser.h" // replaces Boost for XML handling

struct data_settings_t
{
    std::string rom_path;
    std::string res_path;
    std::string save_path;
    std::string cfg_file;
    int crc32;

    std::string file_scores;            // Arcade Hi-Scores (World & Japanese)
    std::string file_scores_jap;
    std::string file_ttrial;            // Time Trial Hi-Scores
    std::string file_ttrial_jap;
    std::string file_cont;              // Continous Mode Hi-Scores
    std::string file_cont_jap;
    std::string file_stats;             // Machine stats (currently number of games played and minutes run time)
};

struct stats_t
{
    // arcade machine stats
    // these do what mechanical counters would have done on real arcade hardware
    // saved every minute
    int playcount;	  // Games played
    int runtime;          // Total run time in minutes
};

struct music_t
{
    // Audio Format
    const static int IS_YM_INT = 0; // Intenal YM Track (from OutRun ROMs)
    const static int IS_YM_EXT = 1; // External YM Track (from Binary)
    const static int IS_WAV = 2;    // External WAV Track
    int type;

    int cmd;                        // Z80 Command
    std::string title;
    std::string filename;
};

struct ttrial_settings_t
{
    int laps;
    int traffic;
    uint16_t best_times[15];
};

struct menu_settings_t
{
    int enabled;
    int road_scroll_speed;
};

struct video_settings_t
{
    const static int MODE_WINDOW  = 0;
    const static int MODE_FULL    = 1;
    const static int MODE_STRETCH = 2;

    int mode;
    int scale;
    int scanlines;
    int widescreen;
    int fps;
    int fps_count;
    int hires;
    int hires_next;
    int filtering;
    int vsync;
    int shadow;

    // JJP - User configurable X and Y position
    // range is -100 to +100, being pixel offsets from calculated image position
    int x_offset;
    int y_offset;

    // JJP - Blargg CRT filtering constants
    const static int BLARGG_DISABLE   = 0;
    const static int BLARGG_COMPOSITE = 1;
    const static int BLARGG_SVIDEO    = 2;
    const static int BLARGG_RGB       = 3;

    // JJP - Blargg filtering settings
    int blargg;            // Blargg mode - per above constants
    int saturation;
    int contrast;
    int brightness;
    int sharpness;
    int gamma;
    int hue;
    int resolution;

    // JJP - Shader constants
    const static int SHADOW_MASK_OFF     = 0;
    const static int SHADOW_MASK_OVERLAY = 1;
    const static int SHADOW_MASK_SHADER  = 2;

    const static int SHADER_OFF          = 0;
    const static int SHADER_FAST         = 1;
    const static int SHADER_FULL         = 2;

    // JJP - CRT Shader settings
    int shader_mode;        // 0 = off (actually pass-through), 1 = fast, 2 = full
    int shadow_mask;        // 0 = off, 1 = overlay based (fast), 2 = shader based (looks better)
    int mask_size;          // 1 = normal, 2 for high DPI screens eg retina
    int crt_shape;          // actually implemented as an SDL2 texture overlay
    int vignette;           // implemented in overlay or shader
    int noise;
    int warpX;
    int warpY;
    int maskDim;            // 0-95, will be used as [value]/100
    int maskBoost;          // 100-195, will be used [value]/100
    int desaturate;
    int desaturate_edges;
    int brightboost;
    int hiresprites;        // 0 = original; 1 = hires mode
};

struct sound_settings_t
{
    int enabled;
    int rate;
    int advertise;
    int preview;
    int fix_samples;
    int music_timer;
    std::vector <music_t> music;
    int callback_rate;   // 0 = 8ms, 1 = 16ms (needed for WSL2)
    int rt_priority;     // 0 = off (default), 1 = request SCHED_FIFO for the audio threads (needs rtprio grant; can cost fps via render-thread preemption - see AUDIO_PERF_PLAN.md)
    int playback_device; // omit from config file or set to -1 to use system default
    std::string playback_device_name; // SDL device name; takes priority over playback_device index when set (matched exactly, then as a substring). Empty = use the index.
    int wave_volume;     // when using .wav files, the playback volume (1-8 where 5 = no adjustment)
    int custom_tracks_loaded = 0; // used to mask help text at startup if tracks are loaded
};

// Rumble subsystem (Stage 4b, tools/INPUT_BINDING_DESIGN.md B.3.2/B.4): the one user-editable custom raw-HID
// profile, persisted under controls.rumble_hid.custom only after "SAVE AS CUSTOM" in the ADVANCED
// HID SETUP editor. `valid` is false until a well-formed <custom> block has been loaded or saved -
// config.cpp rejects a malformed block wholesale (byte strings, indices) rather than partially
// trusting it, so nothing downstream needs to re-validate.
struct rumble_hid_custom_t
{
    bool     valid = false;
    uint16_t vid = 0, pid = 0;
    int      len = 3;               // 1..64
    int      report_id = -1;        // -1 = unnumbered report; 0..255 = numbered report prefix
    int      mode = 0;              // 0 = PRESETS, 1 = INTENSITY (mirrors rumble::HidMode)
    uint8_t  on[64]    = { 0 };
    uint8_t  off[64]   = { 0 };
    uint8_t  pulse[64] = { 0 };
    uint8_t  tmpl[64]  = { 0 };
    int      low_byte = -1, high_byte = -1;
    int      scale = 0;             // 0 = NIBBLE_HI, 1 = BYTE (mirrors rumble::HidScale)
    int      keepalive_ms = 0;
};

// Input binding matrix: one persisted (group, target) -> physical control assignment.
// Adapted from the DX binding model, with `group` added (see tools/INPUT_BINDING_DESIGN.md A.1.1).
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
    bool        invert = false;           // axis only: negate the SDL value before mapping (set automatically at capture)
};

struct controls_settings_t
{
    const static int GEAR_BUTTON   = 0;
    const static int GEAR_PRESS    = 1; // For cabinets
    const static int GEAR_SEPARATE = 2; // Separate button presses
    const static int GEAR_AUTO     = 3;

    int gear;
    int steer_speed;   // Steering Digital Speed
    int pedal_speed;   // Pedal Digital Speed
    int padconfig[15]; // Joypad Button Config
    int keyconfig[12]; // Keyboard Button Config
    int pad_id;        // Use the N'th joystick on the system.
    int analog;        // Use analog controls
    int axis[4];       // Analog Axis
    int asettings[2];  // Analog Settings
    bool invert[3];    // Invert Analog Axis

    float rumble;      // Simple Controller Rumble Support
    int haptic;        // Force Feedback Enabled
    int max_force;
    int min_force;
    int force_duration;

    // Input binding matrix (see tools/INPUT_BINDING_DESIGN.md, Stage 1: data model + config only;
    // not yet evaluated at runtime).
    int input_mode;                          // device_binding_t::GROUP_GAMEPAD / GROUP_WHEEL
    bool bindings_active;                    // true iff <controls><bindings> existed in config.xml
    std::vector<device_binding_t> device_bindings;

    // Rumble subsystem (Stage 4a, see tools/INPUT_BINDING_DESIGN.md Deliverable B, B.4). `rumble_backend`
    // mirrors rumble::Backend (AUTO=0, SDL_CONTROLLER=1, SDL_HAPTIC=2, HID_REPORT=3, FFB=4,
    // OFF=5) as a plain int so this header stays independent of sdl2/rumble.hpp; config.cpp maps
    // the on-disk auto|sdl|haptic|hid|ffb|off string to/from it. HID_REPORT has no backend
    // implementation yet (Stage 4b) and behaves like OFF if selected.
    int rumble_backend;                      // rumble::Backend, as int
    std::string rumble_device;               // optional device-signature override for the rumble target
    int rumble_extras;                       // Stage 4c richer effects (crash/gear/start); unused in 4a

    // Raw-HID rumble profiles (Stage 4b, see tools/INPUT_BINDING_DESIGN.md B.3/B.4). `rumble_hid_profile`
    // is a built-in id (e.g. "thrustmaster3") or "custom"; `rumble_hid_custom` is the one editable
    // custom profile (rumble_hid_custom_t::valid is false until a well-formed block exists).
    std::string rumble_hid_profile;
    rumble_hid_custom_t rumble_hid_custom;
};

struct smartypi_settings_t
{
    int enabled;      // CannonBall used in conjunction with SMARTYPI in arcade cabinet
    int ouputs;       // Write Digital Outputs to console
    int cabinet;      // Cabinet Type
};

struct engine_settings_t
{
    int dip_time;
    int dip_traffic;
    bool freeplay;
    bool freeze_timer;
    bool disable_traffic;
    int jap;
    int prototype;
    int randomgen;
    int level_objects;
    bool fix_bugs;
    bool fix_bugs_backup;
    bool fix_timer;
    bool layout_debug;
    bool hiscore_delete;  // Allow deletion of last entry in score table
    int hiscore_timer;    // Override default timer on high-score entry screen
    int new_attract;      // New Attract Mode
    bool grippy_tyres;    // Handling: Stick to track
    bool offroad;         // Handling: Drive off-road
    bool bumper;          // Handling: Smash into other cars without spinning
    bool turbo;           // Handling: Faster Car
    int car_pal;          // Car Palette
};

class Config
{
public:
    data_settings_t        data;
    stats_t                stats;
    menu_settings_t        menu;
    video_settings_t       video;
    sound_settings_t       sound;
    controls_settings_t    controls;
    engine_settings_t      engine;
    ttrial_settings_t      ttrial;
    smartypi_settings_t    smartypi;

    int master_break_key = SDLK_ESCAPE;

    const static int CABINET_MOVING  = 0;
    const static int CABINET_UPRIGHT = 1;
    const static int CABINET_MINI    = 2;

    // Internal screen width and height
    uint16_t s16_width, s16_height;

    // Internal screen x offset
    uint16_t s16_x_off;

    // 30 or 60 fps
    int fps;

    // Original game ticks sprites at 30fps but background scroll at 60fps
    int tick_fps;

    // Continuous Mode: Traffic Setting
    int cont_traffic;

    Config(void);
    ~Config(void);

    void get_custom_music(const std::string& respath);
    void set_config_file(const std::string& filename);
    void load();
    bool save();
    void load_scores(bool original_mode);
    void save_scores(bool original_mode);
    void load_stats();
    void save_stats();
    void load_timetrial_scores();
    void save_timetrial_scores();
    bool clear_scores();
    void set_fps(int fps);
    void inc_time();
    void inc_traffic();

    // Input binding matrix helpers (Stage 1: one binding per (group, target) cell).
    const device_binding_t* find_binding(int group, int target) const;
    void set_binding(const device_binding_t& binding);
    void clear_binding(int group, int target);

    // To support multi-threaded SDL module:
    bool videoRestartRequired = false;
private:
    xml_parser::ptree cfg;
};

extern Config config;
