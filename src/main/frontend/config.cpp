/***************************************************************************
    XML Configuration File Handling.

    Load Settings.
    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.

    Automatic custom music loader by James Pearce
***************************************************************************/

// see: http://www.boost.org/doc/libs/1_52_0/doc/html/boost_propertytree/tutorial.html
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
// Boost string prediction
#include <boost/algorithm/string/predicate.hpp>
#include <iostream>

#include "main.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "../utils.hpp"

#include "engine/ohiscore.hpp"
#include "engine/outils.hpp"
#include "engine/audio/osoundint.hpp"

// api change in boost 1.56
#include <boost/version.hpp>
#if (BOOST_VERSION >= 105600)
typedef boost::property_tree::xml_writer_settings<std::string> xml_writer_settings;
#else
typedef boost::property_tree::xml_writer_settings<char> xml_writer_settings;
#endif

// JJP - for automatic music file scanning from res folder:
#include <filesystem>
#include <regex>
#include <map>
#include <algorithm>


Config config;

Config::Config(void)
{
    data.cfg_file = "./config.xml";
    
    // Setup default sounds
    music_t magical, breeze, splash;
    magical.title = "MAGICAL SOUND SHOWER";
    breeze.title  = "PASSING BREEZE";
    splash.title  = "SPLASH WAVE";
    magical.type  = music_t::IS_YM_INT;
    breeze.type   = music_t::IS_YM_INT;
    splash.type   = music_t::IS_YM_INT;
    magical.cmd   = sound::MUSIC_MAGICAL;
    breeze.cmd    = sound::MUSIC_BREEZE;
    splash.cmd    = sound::MUSIC_SPLASH;
    sound.music.push_back(magical); // 1st slot
    sound.music.push_back(breeze);  // 2nd slot
    sound.music.push_back(splash);  // 3rd slot
    // Users can replace these with custom music via .wav, .mp3, or .ym files in the res/ folder,
    // and/or add additional tracks.
    sound.custom_tracks_loaded = 0;
}


Config::~Config(void)
{
}




void Config::get_custom_music(const std::string& respath)
{
    // The music list will default to "MAGICAL SOUND SHOWER", "PASSING BREEZE" and "SPLASH WAVE".
    // But, the user can over-ride or add to these by simply adding .WAV, .MP3, or YM binary format files in /res
    // with filenames crafted as follows:
    //
    // [index]_Track_Name.[extension]
    //
    // Where:
    //   [index] is 01 through 99
    //   Track_Name is what will be displayed in the UI (with _ replaced by space)
    //   [extension] is WAV, MP3, or YM
    //
    // Using index values 01, 02 or 03 will replace the associated built-in track (01 being Magical Sound Shower).
    // Where more than one file is provided with the same index value, precidence is WAV>MP3>YM and any other files are
    // ignored.
    //
    // e.g.:
    //    01_Magical_Sound_Shower_Remix.wav - would *replace* the built-in magical sound shower (the first track)
    //    04_AHA_Take_On_Me.mp3             - would *add* a fourth track shown as "AHA Take On Me"
    //

    namespace fs = std::filesystem;
#ifdef WITH_MP3
    static const std::map<std::string,int> ext_priority = {
        { "WAV", 0 },
        { "MP3", 1 },
        { "YM",  2 }
    };
#else
    static const std::map<std::string,int> ext_priority = {
        { "WAV", 0 },
        { "YM",  1 }
    };
#endif

    std::map<int, std::pair<std::string,fs::path>> chosen;
    std::regex pattern(R"((\d{2})[-_](.+))");

    for (auto& entry : fs::directory_iterator(respath)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        auto ext = path.extension().string();
        if (ext.size()<2) continue;
        ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
        auto prio_it = ext_priority.find(ext);
        if (prio_it == ext_priority.end()) continue;

        auto stem = path.stem().string();
        std::smatch m;
        if (!std::regex_match(stem, m, pattern)) continue;

        int idx = std::stoi(m[1]);      // track index 01–99
        int prio = prio_it->second;
        auto it = chosen.find(idx);
        if (it==chosen.end() || prio < ext_priority.at(it->second.first)) {
            chosen[idx] = { ext, path };
        }
    }

    // apply replacements/additions
    for (auto& [idx, ext_path] : chosen) {
        auto& [ext, filepath] = ext_path;

        // build display name in uppercase
        std::string raw = filepath.stem().string().substr(3); // drop "NN_"
        std::replace(raw.begin(), raw.end(), '_', ' ');
        std::replace(raw.begin(), raw.end(), '-', ' ');
        std::transform(raw.begin(), raw.end(), raw.begin(), ::toupper);

        // pick type based on extension
        int track_type = (ext == "YM")
            ? music_t::IS_YM_EXT
            : music_t::IS_WAV;   // WAV & MP3 both map to IS_WAV

        int cmd = sound::MUSIC_CUSTOM;

        std::cout << "Found music file " << raw;
        ++sound.custom_tracks_loaded;
        music_t entry {
            track_type,
            cmd,
            raw,                  // TITLE (upper‑case)
            filepath.filename().string()
        };

        if (idx >= 1 && idx <= 3) {
            std::cout << " (replacing built-in track " << idx << ")" << std::endl;
            // replace built‑in slot 0–2
            sound.music[idx-1] = entry;
        } else {
            std::cout << " (added as available track)" << std::endl;
            // append new track
            sound.music.push_back(entry);
        }
    }
}



// Set Path to load and save config to
void Config::set_config_file(const std::string& file)
{
    data.cfg_file = file;
}

using boost::property_tree::ptree;
ptree pt_config;

void Config::load()
{
    // Load XML file and put its contents in property tree. 
    // No namespace qualification is needed, because of Koenig 
    // lookup on the second argument. If reading fails, exception
    // is thrown.
    try
    {
        read_xml(data.cfg_file, pt_config, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        std::cout << "Error: " << e.what() << "\n";
    }

    // ------------------------------------------------------------------------
    // Data Settings
    // ------------------------------------------------------------------------
    data.rom_path         = pt_config.get("data.rompath", "roms/");  // Path to ROMs
    data.res_path         = pt_config.get("data.respath", "res/");   // Path to ROMs
    data.save_path        = pt_config.get("data.savepath", "./");    // Path to Save Data
    data.crc32            = pt_config.get("data.crc32", 1);

    data.file_scores      = data.save_path + "hiscores.xml";
    data.file_scores_jap  = data.save_path + "hiscores_jap.xml";
    data.file_ttrial      = data.save_path + "hiscores_timetrial.xml";
    data.file_ttrial_jap  = data.save_path + "hiscores_timetrial_jap.xml";
    data.file_cont        = data.save_path + "hiscores_continuous.xml";
    data.file_cont_jap    = data.save_path + "hiscores_continuous_jap.xml";
    data.file_stats       = data.save_path + "play_stats.xml";

    // ------------------------------------------------------------------------
    // Menu Settings
    // ------------------------------------------------------------------------

    menu.enabled           = pt_config.get("menu.enabled",   1);
    menu.road_scroll_speed = pt_config.get("menu.roadspeed", 50);

    // ------------------------------------------------------------------------
    // Video Settings
    // ------------------------------------------------------------------------

    video.mode          = pt_config.get("video.mode",            2); // Video Mode: Default is Full Screen 
    video.scale         = pt_config.get("video.window.scale",    1); // Video Scale: Default is 1x
    video.fps           = pt_config.get("video.fps",             0); // Open game at 30fps; will auto-switch to 60fps if possible
    video.fps_count     = pt_config.get("video.fps_counter",     0); // FPS Counter
    video.widescreen    = pt_config.get("video.widescreen",      0); // Enable Widescreen Mode
    video.hires         = pt_config.get("video.hires",           1); // Hi-Resolution Mode
    video.s16accuracy   = pt_config.get("video.s16accuracy",     1); // Reproduce S16 arcade hardware glowy edges (1=accurate, 0=fast)
    video.vsync         = pt_config.get("video.vsync",           1); // Use V-Sync where available (e.g. Open GL)
    video.x_offset      = pt_config.get("video.x_offset",        0); // Offset from calculated image X position
    video.y_offset      = pt_config.get("video.y_offset",        0); // Offset from calculated image Y position
    // JJP Additional configuration for CRT emulation
    video.shader_mode   = pt_config.get("video.shader_mode",     2); // shader type: 0 = off (actually pass-through), 1 = fast, 2 = full
    video.shadow_mask   = pt_config.get("video.shadow_mask",     2); // shadow mask type: 0 = off, 1 = overlay based (fast), 2 = shader based (looks better)
    video.mask_size     = pt_config.get("video.mask_size",       1); // shadow mask size (1=normal, 2 for high DPI displays)
    video.maskDim       = pt_config.get("video.maskDim",        75); // shadow mask type dim multiplier (75=75%)
    video.maskBoost     = pt_config.get("video.maskBoost",     135); // shadow mask type boost multiplier (135=135%)
    video.scanlines     = pt_config.get("video.scanlines",       0); // scanlines (0=off, 3=max)
    video.crt_shape     = pt_config.get("video.crt_shape",       0); // CRT shape overlay on or off
    video.vignette      = pt_config.get("video.vignette",        0); // amount to dim edges (1=1%)
    video.noise         = pt_config.get("video.noise",           0); // amount of random noise to add (1=1%)
    video.warpX         = pt_config.get("video.warpX",           0); // amount of warp to add along X axis (1=1%)
    video.warpY         = pt_config.get("video.warpY",           0); // amount of warp to add along Y axis (1=1%)
    video.desaturate    = pt_config.get("video.desaturate",      0); // amount to desaturate the entire image (raises black level) (1=1%)
    video.desaturate_edges = pt_config.get("video.desaturate_edges", 0); // amount further desaturate towards edges (1=1%)
    video.brightboost   = pt_config.get("video.brightboost",     0); // relative output brightness (1=1%)
    video.blargg        = pt_config.get("video.blargg",          0); // Blargg filtering mode, 0=off
    video.saturation    = pt_config.get("video.saturation",      0); // Blargg filter saturation, -1 to +1
    video.contrast      = pt_config.get("video.contrast",        0); // Blargg filter contrast, -1 to +1
    video.brightness    = pt_config.get("video.brightness",      0); // Blargg filter brightness, -1 to +1
    video.sharpness     = pt_config.get("video.sharpness",       0); // Blargg edge bluring
    video.resolution    = pt_config.get("video.resolution",      0); // Blargg resolution, -2 to 0
    video.gamma         = pt_config.get("video.gamma",           0); // Blargg gamma, -3 to +3
    video.hue           = pt_config.get("video.hue",             0); // Blargg hue, -10 to +10 => -0.1 to +0.1

    // ------------------------------------------------------------------------
    // Sound Settings
    // ------------------------------------------------------------------------
    sound.enabled     = pt_config.get("sound.enable",      1);
    sound.rate        = pt_config.get("sound.rate",        44100);
    sound.advertise   = pt_config.get("sound.advertise",   1);
    sound.preview     = pt_config.get("sound.preview",     1);
    sound.fix_samples = pt_config.get("sound.fix_samples", 1);
    sound.music_timer = pt_config.get("sound.music_timer", 0);
    // JJP - allow either standard 8ms audio callbacks, or a slower 16ms rate (required with WSL2)
    sound.callback_rate   = pt_config.get("sound.callback_rate",0);
    // Index of SDL playback device to request, -1 for default
    sound.playback_device = pt_config.get("sound.playback_device", -1);

    // Custom Music. Search for enabled custom tracks
    get_custom_music(data.res_path);

    if (!sound.music_timer)
        sound.music_timer = MUSIC_TIMER;
    else
    {
        if (sound.music_timer > 99)
            sound.music_timer = 99;
        sound.music_timer = outils::DEC_TO_HEX[sound.music_timer]; // convert to hexadecimal
    }

    // JJP - Wave file playback volume, 1-8 where 4 = no adjustment
    sound.wave_volume = pt_config.get("sound.wave_volume",4);

    // ------------------------------------------------------------------------
    // SMARTYPI Settings
    // ------------------------------------------------------------------------
    smartypi.enabled = pt_config.get("smartypi.<xmlattr>.enabled", 0);
    smartypi.ouputs  = pt_config.get("smartypi.outputs", 1);
    smartypi.cabinet = pt_config.get("smartypi.cabinet", 1);

    // ------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------
    controls.gear          = pt_config.get("controls.gear", 0);
    controls.steer_speed   = pt_config.get("controls.steerspeed", 3);
    controls.pedal_speed   = pt_config.get("controls.pedalspeed", 4);
    controls.rumble        = pt_config.get("controls.rumble", 1.0f);
    controls.keyconfig[0]  = pt_config.get("controls.keyconfig.up",      273);
    controls.keyconfig[1]  = pt_config.get("controls.keyconfig.down",    274);
    controls.keyconfig[2]  = pt_config.get("controls.keyconfig.left",    276);
    controls.keyconfig[3]  = pt_config.get("controls.keyconfig.right",   275);
    controls.keyconfig[4]  = pt_config.get("controls.keyconfig.acc",     122);
    controls.keyconfig[5]  = pt_config.get("controls.keyconfig.brake",   120);
    controls.keyconfig[6]  = pt_config.get("controls.keyconfig.gear1",   32);
    controls.keyconfig[7]  = pt_config.get("controls.keyconfig.gear2",   32);
    controls.keyconfig[8]  = pt_config.get("controls.keyconfig.start",   49);
    controls.keyconfig[9]  = pt_config.get("controls.keyconfig.coin",    53);
    controls.keyconfig[10] = pt_config.get("controls.keyconfig.menu",    286);
    controls.keyconfig[11] = pt_config.get("controls.keyconfig.view",    304);
    controls.padconfig[0]  = pt_config.get("controls.padconfig.acc",     -1);
    controls.padconfig[1]  = pt_config.get("controls.padconfig.brake",   -1);
    controls.padconfig[2]  = pt_config.get("controls.padconfig.gear1",   -1);
    controls.padconfig[3]  = pt_config.get("controls.padconfig.gear2",   -1);
    controls.padconfig[4]  = pt_config.get("controls.padconfig.start",   -1);
    controls.padconfig[5]  = pt_config.get("controls.padconfig.coin",    -1);
    controls.padconfig[6]  = pt_config.get("controls.padconfig.menu",    -1);
    controls.padconfig[7]  = pt_config.get("controls.padconfig.view",    -1);
    controls.padconfig[8]  = pt_config.get("controls.padconfig.up",      -1);
    controls.padconfig[9]  = pt_config.get("controls.padconfig.down",    -1);
    controls.padconfig[10] = pt_config.get("controls.padconfig.left",    -1);
    controls.padconfig[11] = pt_config.get("controls.padconfig.right",   -1);
    controls.padconfig[12] = pt_config.get("controls.padconfig.limit_l", -1);
    controls.padconfig[13] = pt_config.get("controls.padconfig.limit_c", -1);
    controls.padconfig[14] = pt_config.get("controls.padconfig.limit_r", -1);
    controls.analog        = pt_config.get("controls.analog.<xmlattr>.enabled", 1);
    controls.pad_id        = pt_config.get("controls.pad_id", 0);
    controls.axis[0]       = pt_config.get("controls.analog.axis.wheel", -1);
    controls.axis[1]       = pt_config.get("controls.analog.axis.accel", -1);
    controls.axis[2]       = pt_config.get("controls.analog.axis.brake", -1);
    controls.axis[3]       = pt_config.get("controls.analog.axis.motor", -1);
    controls.invert[1]     = pt_config.get("controls.analog.axis.accel.<xmlattr>.invert", 0);
    controls.invert[2]     = pt_config.get("controls.analog.axis.brake.<xmlattr>.invert", 0);
    controls.asettings[0]  = pt_config.get("controls.analog.wheel.zone", 75);
    controls.asettings[1]  = pt_config.get("controls.analog.wheel.dead", 0);
    
    controls.haptic        = pt_config.get("controls.analog.haptic.<xmlattr>.enabled", 0);
    controls.max_force     = pt_config.get("controls.analog.haptic.max_force", 9000);
    controls.min_force     = pt_config.get("controls.analog.haptic.min_force", 8500);
    controls.force_duration= pt_config.get("controls.analog.haptic.force_duration", 20);

    // ------------------------------------------------------------------------
    // Engine Settings
    // ------------------------------------------------------------------------

    engine.dip_time      = pt_config.get("engine.time",    0);
    engine.dip_traffic   = pt_config.get("engine.traffic", 1);
    
    engine.freeze_timer    = engine.dip_time == 4;
    engine.disable_traffic = engine.dip_traffic == 4;
    engine.dip_time    &= 3;
    engine.dip_traffic &= 3;

    engine.freeplay      = pt_config.get("engine.freeplay",        0) != 0;
    engine.jap           = pt_config.get("engine.japanese_tracks", 0);
    engine.prototype     = pt_config.get("engine.prototype",       0);
    
    // Additional Level Objects
    engine.level_objects   = pt_config.get("engine.levelobjects", 1);
    engine.randomgen       = pt_config.get("engine.randomgen",    1);
    engine.fix_bugs_backup = 
    engine.fix_bugs        = pt_config.get("engine.fix_bugs",     1) != 0;
    engine.fix_timer       = pt_config.get("engine.fix_timer",    0) != 0;
    engine.layout_debug    = pt_config.get("engine.layout_debug", 0) != 0;
    engine.hiscore_delete  = pt_config.get("scores.delete_last_entry", 1);
    engine.hiscore_timer   = pt_config.get("scores.hiscore_timer", 0);
    engine.new_attract     = pt_config.get("engine.new_attract", 1) != 0;
    engine.offroad         = pt_config.get("engine.offroad", 0);
    engine.grippy_tyres    = pt_config.get("engine.grippy_tyres", 0);
    engine.bumper          = pt_config.get("engine.bumper", 0);
    engine.turbo           = pt_config.get("engine.turbo", 0);
    engine.car_pal         = pt_config.get("engine.car_color", 0);

    if (!engine.hiscore_timer)
        engine.hiscore_timer = HIGHSCORE_TIMER;
    else
    {
        if (engine.hiscore_timer > 99)
            engine.hiscore_timer = 99;
        engine.hiscore_timer = outils::DEC_TO_HEX[engine.hiscore_timer]; // convert to hexadecimal
    }

    // ------------------------------------------------------------------------
    // Time Trial Mode
    // ------------------------------------------------------------------------

    ttrial.laps    = pt_config.get("time_trial.laps",    5);
    ttrial.traffic = pt_config.get("time_trial.traffic", 3);

    cont_traffic   = pt_config.get("continuous.traffic", 3);
}

bool Config::save()
{
    // Save stuff
    // JJP - CRT emulation settings
    pt_config.put("video.mode",               video.mode);          // Video Mode: Full Screen (2)
    pt_config.put("video.window.scale",       video.scale);         // Video Scale: 1x (1)
    pt_config.put("video.fps_counter",        video.fps_count);     // FPS Counter (0)
    pt_config.put("video.widescreen",         video.widescreen);    // Widescreen Mode (1)
    pt_config.put("video.vsync",              video.vsync);         // V-Sync (1)
    pt_config.put("video.s16accuracy",        video.s16accuracy);   // Reproduce S16 arcade hardware glowy edges (1=accurate, 0=fast)
    pt_config.put("video.x_offset",           video.x_offset);      // X offset
    pt_config.put("video.y_offset",           video.y_offset);      // Y offset
    // JJP Additional configuration for CRT emulation
    pt_config.put("video.shader_mode",        video.shader_mode);   // Shader type (Off/Fast/Full) (2)
    pt_config.put("video.shadow_mask",        video.shadow_mask);   // Shadow mask type (Off/Overlay/Shader) (2)
    pt_config.put("video.maskDim",            video.maskDim);       // Shadow mask Dim value (0)
    pt_config.put("video.maskBoost",          video.maskBoost);     // Shadow mask Boost value (0)
    pt_config.put("video.scanlines",          video.scanlines);     // Scanlines (0)
    pt_config.put("video.crt_shape",          video.crt_shape);     // CRT shape overlay (0)
    pt_config.put("video.vignette",           video.vignette);      // Vignette amount (0)
    pt_config.put("video.noise",              video.noise);         // Noise amount (0)
    pt_config.put("video.warpX",              video.warpX);         // Warp on X axis (0)
    pt_config.put("video.warpY",              video.warpY);         // Warp on Y axis (0)
    pt_config.put("video.desaturate",         video.desaturate);    // Desaturation level (0)
    pt_config.put("video.desaturate_edges",   video.desaturate_edges); // Edge desaturation (0)
    pt_config.put("video.brightboost",        video.brightboost);   // Bright boost element 1 (0)
    pt_config.put("video.blargg",             video.blargg);        // Blargg filtering mode (0=off)
    pt_config.put("video.saturation",         video.saturation);    // Filter saturation (-1 to +1)
    pt_config.put("video.contrast",           video.contrast);      // Filter contrast (-1 to +1)
    pt_config.put("video.brightness",         video.brightness);    // Filter brightness (-1 to +1)
    pt_config.put("video.sharpness",          video.sharpness);     // Edge blurring
    pt_config.put("video.resolution",         video.resolution);    // Resolution (-2 to 0)
    pt_config.put("video.gamma",              video.gamma);         // Gamma (-3 to +3)
    pt_config.put("video.hue",                video.hue);           // Hue (-10 to +10)

    pt_config.put("sound.enable",             sound.enabled);
    pt_config.put("sound.advertise",          sound.advertise);
    pt_config.put("sound.preview",            sound.preview);
    pt_config.put("sound.fix_samples",        sound.fix_samples);
    pt_config.put("sound.rate",               sound.rate);             // audio sampling rate e.g. 44100 (Hz)
    pt_config.put("sound.callback_rate",      sound.callback_rate);    // JJP - 0=8ms callbacks, 1=16ms
    pt_config.put("sound.playback_device",    sound.playback_device);  // JJP - Index of SDL playback device to request, -1 for default  
    pt_config.put("sound.wave_volume",        sound.wave_volume);      // JJP - volume adjustment to .wav files

    if (config.smartypi.enabled)
        pt_config.put("smartypi.cabinet",     config.smartypi.cabinet);

    pt_config.put("controls.gear",            controls.gear);
    pt_config.put("controls.rumble",          controls.rumble);
    pt_config.put("controls.steerspeed",      controls.steer_speed);
    pt_config.put("controls.pedalspeed",      controls.pedal_speed);
    pt_config.put("controls.keyconfig.up",    controls.keyconfig[0]);
    pt_config.put("controls.keyconfig.down",  controls.keyconfig[1]);
    pt_config.put("controls.keyconfig.left",  controls.keyconfig[2]);
    pt_config.put("controls.keyconfig.right", controls.keyconfig[3]);
    pt_config.put("controls.keyconfig.acc",   controls.keyconfig[4]);
    pt_config.put("controls.keyconfig.brake", controls.keyconfig[5]);
    pt_config.put("controls.keyconfig.gear1", controls.keyconfig[6]);
    pt_config.put("controls.keyconfig.gear2", controls.keyconfig[7]);
    pt_config.put("controls.keyconfig.start", controls.keyconfig[8]);
    pt_config.put("controls.keyconfig.coin",  controls.keyconfig[9]);
    pt_config.put("controls.keyconfig.menu",  controls.keyconfig[10]);
    pt_config.put("controls.keyconfig.view",  controls.keyconfig[11]);
    pt_config.put("controls.padconfig.acc",   controls.padconfig[0]);
    pt_config.put("controls.padconfig.brake", controls.padconfig[1]);
    pt_config.put("controls.padconfig.gear1", controls.padconfig[2]);
    pt_config.put("controls.padconfig.gear2", controls.padconfig[3]);
    pt_config.put("controls.padconfig.start", controls.padconfig[4]);
    pt_config.put("controls.padconfig.coin",  controls.padconfig[5]);
    pt_config.put("controls.padconfig.menu",  controls.padconfig[6]);
    pt_config.put("controls.padconfig.view",  controls.padconfig[7]);
    pt_config.put("controls.padconfig.up",    controls.padconfig[8]);
    pt_config.put("controls.padconfig.down",  controls.padconfig[9]);
    pt_config.put("controls.padconfig.left",  controls.padconfig[10]);
    pt_config.put("controls.padconfig.right", controls.padconfig[11]);
    pt_config.put("controls.analog.<xmlattr>.enabled", controls.analog);
    pt_config.put("controls.analog.axis.wheel", controls.axis[0]);
    pt_config.put("controls.analog.axis.accel", controls.axis[1]);
    pt_config.put("controls.analog.axis.brake", controls.axis[2]);

    pt_config.put("engine.freeplay",        (int) engine.freeplay);
    pt_config.put("engine.time",            engine.freeze_timer ? 4 : engine.dip_time);
    pt_config.put("engine.traffic",         engine.disable_traffic ? 4 : engine.dip_traffic);
    pt_config.put("engine.japanese_tracks", engine.jap);
    pt_config.put("engine.prototype",       engine.prototype);
    pt_config.put("engine.levelobjects",    engine.level_objects);
    pt_config.put("engine.fix_bugs",        (int) engine.fix_bugs);
    pt_config.put("engine.fix_timer",       (int) engine.fix_timer);
    pt_config.put("engine.new_attract",     engine.new_attract);
    pt_config.put("engine.offroad",         (int) engine.offroad);
    pt_config.put("engine.grippy_tyres",    (int) engine.grippy_tyres);
    pt_config.put("engine.bumper",          (int) engine.bumper);
    pt_config.put("engine.turbo",           (int) engine.turbo);
    pt_config.put("engine.car_color",       engine.car_pal);

    pt_config.put("time_trial.laps",    ttrial.laps);
    pt_config.put("time_trial.traffic", ttrial.traffic);
    pt_config.put("continuous.traffic", cont_traffic), 

    ttrial.laps    = pt_config.get("time_trial.laps",    5);
    ttrial.traffic = pt_config.get("time_trial.traffic", 3);
    cont_traffic   = pt_config.get("continuous.traffic", 3);

    try
    {
        write_xml(data.cfg_file, pt_config, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
        return false;
    }
    return true;
}

void Config::load_scores(bool original_mode)
{
    std::string filename;

    if (original_mode)
        filename = engine.jap ? data.file_scores_jap : data.file_scores;
    else
        filename = engine.jap ? data.file_cont_jap : data.file_cont;

    // Create empty property tree object
    ptree pt;

    try
    {
        read_xml(filename , pt, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
        return;
    }
    
    // Game Scores
    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];
        
        std::string xmltag = "score";
        xmltag += Utils::to_string(i);  
    
        e->score    = Utils::from_hex_string(pt.get<std::string>(xmltag + ".score",    "0"));
        e->initial1 = pt.get(xmltag + ".initial1", ".")[0];
        e->initial2 = pt.get(xmltag + ".initial2", ".")[0];
        e->initial3 = pt.get(xmltag + ".initial3", ".")[0];
        e->maptiles = Utils::from_hex_string(pt.get<std::string>(xmltag + ".maptiles", "20202020"));
        e->time     = Utils::from_hex_string(pt.get<std::string>(xmltag + ".time"    , "0")); 

        if (e->initial1 == '.') e->initial1 = 0x20;
        if (e->initial2 == '.') e->initial2 = 0x20;
        if (e->initial3 == '.') e->initial3 = 0x20;
    }
}

void Config::save_scores(bool original_mode)
{
    std::string filename;

    if (original_mode)
        filename = engine.jap ? data.file_scores_jap : data.file_scores;
    else
        filename = engine.jap ? data.file_cont_jap : data.file_cont;

    // Create empty property tree object
    ptree pt;

    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];

        std::string xmltag = "score";
        xmltag += Utils::to_string(i);

        pt.put(xmltag + ".score",    Utils::to_hex_string(e->score));
        pt.put(xmltag + ".initial1", e->initial1 == 0x20 ? "." : Utils::to_string((char) e->initial1)); // use . to represent space
        pt.put(xmltag + ".initial2", e->initial2 == 0x20 ? "." : Utils::to_string((char) e->initial2));
        pt.put(xmltag + ".initial3", e->initial3 == 0x20 ? "." : Utils::to_string((char) e->initial3));
        pt.put(xmltag + ".maptiles", Utils::to_hex_string(e->maptiles));
        pt.put(xmltag + ".time",     Utils::to_hex_string(e->time));
    }
    
    try
    {
        write_xml(filename, pt, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        std::cout << "Error saving hiscores: " << e.what() << "\n";
    }
}

void Config::load_stats()
{
    std::string filename = data.file_stats;

    // Create empty property tree object
    ptree pt;

    try
    {
        read_xml(filename , pt, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
        return;
    }
    
    // Load machine stats from file
    stats.playcount = pt.get("stats.playcount", 0);
    stats.runtime   = pt.get("stats.runtime",   0);
}

void Config::save_stats()
{
    std::string filename = data.file_stats;

    // Create empty property tree object
    ptree pt;

    pt.put("stats.playcount", stats.playcount);
    pt.put("stats.runtime",   stats.runtime);

    try
    {
        write_xml(filename, pt, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        std::cout << "Error saving machine stats: " << e.what() << "\n";
    }
}

void Config::load_tiletrial_scores()
{
    // Counter value that represents 1m 15s 0ms
    static const uint16_t COUNTER_1M_15 = 0x11D0;

    // Create empty property tree object
    ptree pt;

    try
    {
        read_xml(engine.jap ? config.data.file_ttrial_jap : config.data.file_ttrial, pt, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        for (int i = 0; i < 15; i++)
            ttrial.best_times[i] = COUNTER_1M_15;

        std::cout << e.what();
        return;
    }

    // Time Trial Scores
    for (int i = 0; i < 15; i++)
    {
        ttrial.best_times[i] = pt.get("time_trial.score" + Utils::to_string(i), COUNTER_1M_15);
    }
}

void Config::save_tiletrial_scores()
{
    // Create empty property tree object
    ptree pt;

    // Time Trial Scores
    for (int i = 0; i < 15; i++)
    {
        pt.put("time_trial.score" + Utils::to_string(i), ttrial.best_times[i]);
    }

    try
    {
        write_xml(engine.jap ? config.data.file_ttrial_jap : config.data.file_ttrial, pt, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        std::cout << "Error saving hiscores: " << e.what() << "\n";
    }
}

bool Config::clear_scores()
{
    // Init Default Hiscores
    ohiscore.init_def_scores();

    int clear = 0;

    // Remove XML files if they exist
    clear += remove(data.file_scores.c_str());
    clear += remove(data.file_scores_jap.c_str());
    clear += remove(data.file_ttrial.c_str());
    clear += remove(data.file_ttrial_jap.c_str());
    clear += remove(data.file_cont.c_str());
    clear += remove(data.file_cont_jap.c_str());

    // remove returns 0 on success
    return clear == 6;
}

void Config::set_fps(int fps)
{
    video.fps = fps;
    // Set core FPS to 30fps or 60fps
    this->fps = video.fps == 0 ? 30 : 60;
    
    // Original game ticks sprites at 30fps but background scroll at 60fps
    tick_fps  = video.fps < 2 ? 30 : 60;

    cannonball::frame_ms = 1000.0 / this->fps;

    /* JJP - Sound initialised in seperate thread so not required here */
 }

// Inc time setting from menu
void Config::inc_time()
{
    if (engine.dip_time == 3)
    {
        if (!engine.freeze_timer)
            engine.freeze_timer = 1;
        else
        {
            engine.dip_time = 0;
            engine.freeze_timer = 0;
        }
    }
    else
        engine.dip_time++;
}

// Inc traffic setting from menu
void Config::inc_traffic()
{
    if (engine.dip_traffic == 3)
    {
        if (!engine.disable_traffic)
            engine.disable_traffic = 1;
        else
        {
            engine.dip_traffic = 0;
            engine.disable_traffic = 0;
        }
    }
    else
        engine.dip_traffic++;
}
