# CannonBall-SE Change Log

## 1.50

### Performance
- Major speed-ups across the renderer: the sprite, road and background drawing were rebuilt, making 60fps reachable on Raspberry Pi 2 and Pi 3-class boards.
- Faster CRT/NTSC video filter.
- Improved GPU shader performance (CRT shadow mask).

### Audio
- Choose the audio output device from the in-game menu; the choice is remembered by name, so it survives the device order changing between boots.
- Selectable audio callback interval (8ms / 16ms / Auto), auto-tuned to the board to reduce dropouts.
- Smoother, more robust audio playback, with optional real-time scheduling for use under heavy load (off by default).

### Controls
- New Configure Inputs screen: bind any wheel, pedal or gamepad control, with automatic detection of axis direction so a reversed wheel or pedal needs no manual setting.
- Expanded rumble and force-feedback support, including raw-HID wheels and effects for crashes, gear changes and race start.

### ROMs
- Load ROMs directly from a MAME `outrun.zip` without extracting it.

### Raspberry Pi
- Appliance mode: the Pi boots straight into the game with a custom splash screen and powers off when you quit, turning it into a dedicated OutRun machine.
- One-line install command for Raspberry Pi.
- Installer offers a recommended overclock on Pi 2/3 and warns if the power supply looks inadequate.

### Interface
- The game screen now dims behind the menu, making the menu easier to read.

### Fixes
- F7 no longer wrongly enables hi-res sprites while the engine is in low-res mode.
- Fixed a CRT-filter seam across the middle of the screen when using two render threads.
- Restored correct default sprite sizes and the traffic passing sound effects.
- Fixed a hi-res sprite black line and horizontal offset, and a black screen after a display-mode change.

## 1.41
- Added a RISC-V RVV 1.0 SIMD path, with an SSE2 fallback for x86.
- Improved formatting in the Windows compiling notes.

## 1.4
- Hi-res sprite rendering mode (beta): sharper distant sprites, though some objects may be slightly misaligned.
- Alternative wheel-slip generation (with `fix_bugs` enabled) for consistent slip throughout the game.
- Soft-fail if the fixed-samples ROM is missing: log a warning and use the Rev B ROM instead.
- Added the `-perftest` command-line option.
- F8 toggles the video effects on or off in game.
- Fast shader now includes the CRT mask; 60fps achievable on Pi 2.
- Faster overlay generation, and the CRT bezel now adjusts with warpX/warpY.
- Fewer video restarts needed when changing settings.
- Better use of CPU cores in the threaded work order.
- Fixes: OpenMP work-sharing race in the NTSC init, and the vsync setting at initialisation.

## 1.3
- System 16 glowing sprite edges are now always enabled.
- Improved MP3 loader to reduce the frame-rate impact on Pi Zero and Pi 1.
- Single-core mode adds more video noise.
- The `vsync` config setting is now respected by auto 30/60fps mode.
- Fixes: numeric characters in custom music track names on 32-bit builds; consistent skidding across level transitions; a Windows video-initialisation failure.

## 1.2
- Added Windows support (Visual Studio).
- 16-bit intermediate pixel buffer (dropped unused S16 highlights).
- Single-core mode uses a 22kHz audio sample rate.
- Faster sprite rendering on Pi 1 and Pi Zero.
- Fixes: crash when `config.xml` is missing; missing settings written to a fresh config; menu info messages no longer cropped by the CRT overlay.

## 1.1
- Removed the Boost dependency, reducing the disk space needed to build.
- Single-core Pi support (Pi 1, Pi Zero): original 320x224 mode, scalar filter fallbacks, and automatic optimised settings at launch.
- Performance improvements in the sprite, tile and road hardware paths.
- Option to use F10 instead of Escape as the master break key.
- Fixes: ghost cars near checkpoints; several out-of-bounds and overflow issues; segfaults on menu transitions and video restarts; settings saved when choosing Enhanced or Original.

## 1.0
- Added an OpenGL interface module for shader support without SDLgpu.
- Heavily revised the sound module for MP3 playback and to eliminate periodic sync pops.

## Older
### New functionality
- CRT effect using a customised version of Blargg's NTSC filter, optimised for ARM NEON.
- CRT effect settings in the menu.
- GLSL shader support (SDLgpu).
- Multi-threading to improve performance on Pi 2/3/4.
- Additional rumble when the tyre-smoke effect is shown.

### Fixes
- Countdown clock speed.
- Hang on the high-score screen with the timing fixes in place.
- Ferrari engine stopping during the crash sequence.
- Sound running at the correct arcade sample rate and playback speed.
