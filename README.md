# Cannonball - OutRun Engine

## Credits

* **Chris White** - Project creator. See [Reassembler Blog](http://reassembler.blogspot.co.uk/) and [GitHub Repo](https://github.com/djyt/cannonball)

## What Is This Version?

This build is tuned for use in home-made cabinets powered by Raspberry Pi Zero 2W and focuses on providing a gaming experience closer to the arcade when using an LCD screen. There are a number of enhancements over Reassembler's (incredible!) original:

- **CRT effects**: Implemented with a combination of a SIMD-tuned version of Blargg's NTSC filter, overlay mask, and GLSL shader.
- **Automatic 30/60 FPS selection** and automatic frame-drops to keep the game running smoothly.
- **Audio output device specification**: The audio device can be set in `config.xml` (available devices are listed when the game is run).
- **Audio optimized for 31,250Hz operation** to match the original S16 hardware.
- **Enhanced rumble**: Provides some rumble effect with tyre smoke and uses `/dev/hidraw` to activate rumble if SDL doesn't support the device.
- **Linux watchdog support**: Automatically restarts the device on hang (for example, due to too aggressive overclocking; the game has been tested with over 2 days run-time).

This build will run at 60FPS on just a Pi Zero 2W at 1280x1024 (requires 450MHz GPU configuration and a cooling solution) when using a lite command-line only installation of Raspbian. It should also run at 60FPS on a Pi3 since that has the same SoC.

For cooling, an aluminium case like [this one from Pimoroni](https://shop.pimoroni.com/products/aluminium-heatsink-case-for-raspberry-pi-zero) (no affiliation) works fine, with the SoC temperature settling at about 70°C.

The SIMD implementation of the Blargg library also supports compiling on x86. For those interested in the operation of Blargg's NTSC filter, some notes from various sources are included in the file `docs/Blargg-NTSC-Filter-Concepts-and-Implementation.txt`. The implementation for this project is located in the `SDL2` folder.

## What You Need

- **Hardware**:
  - Raspberry Pi Zero 2W or Pi 3/4/5.
  - A screen such as a Dell 1708FP or similar. These are ideal because they have a removable VESA mount (great for arcade cases) and include a USB hub (for wheel, sound, cooling fan, etc.). They are often available very cheaply (sometimes even new).
- Cabinet revision B ROMs, copied to the `roms` directory and renamed if necessary.
- Ideally, a wheel. The included `config.xml` is set up for the ancient Thrustmaster Ferrari GT Experience, a rumble-capable PC/PS3 USB wheel, which is often available at low cost on auction sites.

## Getting Started

The following instructions assume you are starting with a minimal 64-bit command-line install (last updated for the Bookworm release). A Micro-SD card for this install can be created with the **Raspberry Pi Imager** by selecting the platform, then in "Raspberry Pi OS (other)" choosing **Raspberry Pi OS Lite (64-bit)**. You will need at least an 8GB micro-SD card. For a more durable solution, use an industrial card (for example, Sandisk SDSDQAF3-008G-I available from Mouser).

It is recommended to enable SSH. This can be done in the Raspberry Pi Imager along with WiFi setup. Then, SSH into the device (the IP address will be shown at the login prompt) so you can copy and paste commands into the terminal.

### Install Required Libraries, Build SDL_gpu, and Register It With the Linker

```bash
sudo apt update
sudo apt install git cmake libboost-all-dev libsdl2-dev libglu1-mesa-dev
git clone https://github.com/grimfang4/sdl-gpu.git
cd sdl-gpu
mkdir build
cd build
cmake ..
make && sudo make install
sudo sh -c 'echo "/usr/local/lib" >> /etc/ld.so.conf.d/local.conf' && sudo ldconfig
```

### Fetch Cannonball Source and Compile

```bash
cd ~
git clone https://github.com/J1mbo/cannonball.git && cd cannonball
mkdir -p build roms
cd build
cmake ../cmake -DTARGET=../cmake/linux.cmake
make
cd ~/cannonball
chmod +x add-kernel-permissions.sh && sudo ./add-kernel-permissions.sh
```

The build process will take a few minutes. The `add-kernel-permissions.sh` script enables the user-mode application to use the system watchdog and control rumble via `/dev/hidraw`.

Next, copy the S16 ROMs to `~/cannonball/roms/` and edit `~/cannonball/config.xml` as needed.

## Extra Configuration for Pi Zero 2W or Pi 3

If using a USB audio device (such as an external amplifier), add the following to **cmdline.txt** (usually `/boot/firmware/cmdline.txt`) to set USB to 1.1 mode. This prevents audio break-up and eventual stoppage:

```bash
sudo sed -i 's/$/ dwc_otg.speed=1/' /boot/firmware/cmdline.txt
```

*Note:* This appends the setting to the end of the existing line. Thanks to [RaspyFi](http://www.raspyfi.com/anatomy-of-a-pi-usb-audio-quality-and-related-issues-on-pi/) for documenting this fix.

Next, set the GPU clock to 450MHz by appending the following settings to `/boot/firmware/config.txt`, then reboot:

```bash
sudo sh -c 'printf "\nover_voltage=6\ncore_freq=450\ngpu_freq=450\n" >> /boot/firmware/config.txt'
```

This adds the following to the end of the file:

```text
over_voltage=6
core_freq=450
gpu_freq=450
```

*Note:* CPU overclock is not required (the default 1GHz is sufficient).

Finally, reboot the machine:

```bash
sudo reboot
```

## To Run The Game

For the first run, connect via SSH and execute:

```bash
cd ~/cannonball
build/cannonball
```

This will start the game on the connected monitor and output some potentially useful console information, for example:

```console
$ build/cannonball
./play_stats.xml: cannot open file
Cannonball requires wayland video driver for 60fps operation under desktop environment.
Start cannonball like:
  SDL_VIDEODRIVER=wayland build/cannonball
Available SDL video drivers:
   x11
   wayland
   KMSDRM
   offscreen
   dummy
   evdev
Window Pixel Format: SDL_PIXELFORMAT_ARGB8888 (0x16362004)
No kernel supported rumbler wheel detected via /dev/input/event
Watchdog timeout set to 15 seconds.
Game controller detected without SDL rumble support.
Successfully opened rumble device at /dev/hidraw0
./hiscores.xml: cannot open file
SoundChip::init sample rate 31250
SoundChip::init sample rate 31250
Available SDL audio devices:
   0: vc4-hdmi, MAI PCM i2s-hifi-0
   1: Poly BT700, USB Audio
```

Set the required sound device in `config.xml` accordingly, for example:

```xml
<playback_device>1</playback_device>
```

## Autostarting Cannonball at Power On

To run the program automatically at boot, the following will enable auto-login start cannonball using `~/.bashrc`:

```bash
sudo raspi-config nonint do_boot_behaviour B2
echo 'cd ~/cannonball && build/cannonball' >> ~/.bashrc
```
