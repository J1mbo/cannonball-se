Cannonball - OutRun Engine
==========================

Credits
-------

* Chris White - Project creator. See [Reassembler Blog](http://reassembler.blogspot.co.uk/) and [GitHub Repo](https://github.com/djyt/cannonball)


What Is This Version?
---------------------

This build is tuned for use in home-made cabinets powered by Raspberry Pi Zero 2W and focuses on providing a gaming experience closer to the arcade when using an LCD screen. There are a number of enhancements over Reassembler's (incredible!) original:

* CRT effects, implemented with a combination of a SIMD tuned version of Blargg's NTSC filter, overlay mask, and GLSL shader
* Automatic 30/60 FPS selection
* Audio output device can be specified in config.xml (available devices are listed when the game is run)
* Audio optimised for 31,250Hz operation to match the original S16 hardware
* Enhanced rumble (provides some rumble effect with tyre smoke and code uses /dev/hidraw to activate rumble if SDL doesn't support the device)
* Uses Linux watchdog to automatically restart device on hang (for example due to too aggressive overclocking; the game has nevertheless been tested with over 2 days run-time)

This build will run at 60FPS on just a Pi Zero 2W at 1280x1024 (requires 450MHz GPU configuration and will need a cooling solution) when using a lite command-line only installation of Raspbian. Should also run at 60FPS on Pi3 since that has the same SoC.

For cooling, an aluminium case like this one available from Pimoroni (with who I have no affiliation) works fine for me with SoC temperature settling at about 70°C:
https://shop.pimoroni.com/products/aluminium-heatsink-case-for-raspberry-pi-zero

The SIMD implementation of the Blarrg library also supports compiling on x86. If you are interested in the operation of Blarrg's NTSC filter, I have included some notes from a few sources in Blargg-NTSC-Filter-Concepts-and-Implementation.txt in the docs folder. The implementation for this project is in the SDL2 folder.


What You Need
-------------

* Raspberry Pi Zero 2W or Pi 3/4/5.
* Screen such as Dell 1708FP or similar. These are perfect as they have a removable VESA mount that can be screwed into an arcade case and include a USB hub (providing ports for wheel/sound/cooling fan/whatever else). Often available on auction sites for next to nothing (sometimes new in box).
* Cabinet revision B ROMs, copied to roms directory and renamed if necessary.
* Ideally, a wheel. Included config.xml is configured for the ancient Thrustmaster Ferrari GT Experience rumble-capable PC/PS3 USB wheel, which looks about right and is available for almost nothing on your favourite auction site.


Getting Started
---------------

The following assume starting with minimal 64-bit command-line install and were last updated for bookworm release. A Micro-SD card for this can be created with the 'Raspberry Pi Imager' by selecting the platform, then in 'Raspberry Pi OS (other)' chose 'Raspberry Pi OS Lite (64-bit)'. it will need at least an 8GB micro-SD card. If you are making something you want to last, use an industrial card like Sandisk SDSDQAF3-008G-I (available from mouser).

It is recommended to enable ssh, which can be done in the Rasberry Pi Imager along with setting up WiFi connection parameters, and then ssh to the device (it will show it's IP address at the login prompt) so that commands can be copy/pasted to the terminal.

Install required libraries then fetch and build SDL_gpu, and register it with the linker:

###
  sudo apt update
  sudo apt install git cmake libboost-all-dev libsdl2-dev libglu1-mesa-dev
  git clone https://github.com/grimfang4/sdl-gpu.git
  cd sdl-gpu
  mkdir build
  cd build
  cmake ..
  make && sudo make install
  sudo sh -c 'echo "/usr/local/lib" >> /etc/ld.so.conf.d/local.conf' && sudo ldconfig
###

Next fetch cannonball source and compile:

###
  cd ~
  git clone https://github.com/J1mbo/cannonball.git && cd cannonball
  mkdir -p build roms
  cd build
  cmake ../cmake -DTARGET=../cmake/linux.cmake
  make
  cd ~/cannonball
  chmod +x add-kernel-permissions.sh && sudo ./add-kernel-permissions.sh
###

The build process will take a few minutes. The "add-kernel-permissions.sh" script enables the user-mode application to use the system watchdog and control rumble via /dev/hidraw.

Next, copy the S16 ROMs to ~/cannonball/roms/ and edit ~/cannonball/config.xml as needed.


Extra configuration for Pi Zero 2W or Pi 3:
-------------------------------------------

If using a USB audio device (such as external amp), add ###dwc_otg.speed=1### to cmdline.txt (usually /boot/firmware/cmdline.txt), for example:

###
  sudo sed -i 's/$/ dwc_otg.speed=1/' /boot/firmware/cmdline.txt
###

This sets the USB to 1.1 mode. Otherwise, audio will break-up and eventually stop on this platform. Add the setting to the end of the line (not on a new line). Source: http://www.raspyfi.com/anatomy-of-a-pi-usb-audio-quality-and-related-issues-on-pi/

Next, set the GPU clock to 450MHz by adding the relevant settings to /boot/firmware/config.txt, and reboot:

###
  sudo sh -c 'printf "\nover_voltage=6\ncore_freq=450\ngpu_freq=450\n" >> /boot/firmware/config.txt'
###

This adds to the end of the file:

###
  over_voltage=6
  core_freq=450
  gpu_freq=450
###

Note that CPU overclock is not required (the default 1GHz is sufficient).

Next, reboot the machine:

###
  sudo reboot
###


To Run The Game
---------------

To run the game for the first time, connect via ssh and run the below:

###
  cd ~/cannonball
  build/cannonball
###

This will start the game on the connected monitor and will generate some potentially useful console output such as the list of SDL audio devices and their associated numeric references, for example:

###
$ build/cannonball
./play_stats.xml: cannot open file
Cannonball requires wayland video driver for 60fps operation under desktop environment. Start cannonball like:
$ SDL_VIDEODRIVER=wayland build/cannonball
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
###

Set the required sound device in config.xml, for example to use the 'Poly BT700' listed above:

###
<playback_device>1</playback_device>
###


Autostarting Cannonball at Power On
----------------------------------- 

To run the program automatically on boot, enable auto-login then start cannonball via .bashrc:

###
  sudo raspi-config nonint do_boot_behaviour B2
  echo 'cd ~/cannonball && build/cannonball' >> ~/.bashrc
###

