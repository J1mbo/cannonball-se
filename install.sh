#!/usr/bin/env bash

# install.sh
# Install script for CannonBall-SE, a CannonBall fork by James Pearce
# Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce
# Supports Ubuntu 24.04 and RaspberryPi OS (CLI only)

# Exit on unset vars and errors
set -euo pipefail

# Arrays to record steps and statuses
declare -A STATUS
declare -a STEP_ORDER=()

# Print summary of all steps
function print_summary() {
  echo -e "\n====== Installation Summary ======"
  for step in "${STEP_ORDER[@]}"; do
    printf "%-40s : %s\n" "$step" "${STATUS[$step]:-Skipped}"
  done
}

# Wrapper to run a step, record its status, and abort on failure
function run_step() {
  local name="$1"; shift
  STEP_ORDER+=("$name")
  echo -e "\n--> $name..."
  if "$@"; then
    STATUS["$name"]="Success"
    echo "    [OK] $name completed."
  else
    STATUS["$name"]="Failed"
    echo "    [ERROR] $name failed."
    print_summary
    exit 1
  fi
}

# Detect Raspberry Pi OS: appliance mode is only offered there.
function is_raspberry_pi() {
  [[ -f /proc/device-tree/model ]] || return 1
  [[ -f /boot/firmware/cmdline.txt ]] || return 1
  grep -q "^Raspberry Pi" /proc/device-tree/model
}

# ARM CPU part id (e.g. 0xc07 for Cortex-A7, 0xd03 for Cortex-A53), used to
# decide whether a recommended overclock is offered.
function cpu_part() { grep -m1 -i "^CPU part" /proc/cpuinfo | awk '{print tolower($NF)}'; }

# --remove-appliance: revert appliance mode and exit, before any other prompt.
if [[ "${1:-}" == "--remove-appliance" ]]; then
  if is_raspberry_pi; then
    exec sudo bash "$(dirname "$0")/res/appliance/uninstall-appliance.sh"
  else
    echo "Appliance mode is only available on Raspberry Pi OS."
    exit 0
  fi
fi

# 1. Confirm with user
echo "Install script for CannonBall-SE, a CannonBall fork by James Pearce"
echo "Copyright (c) 2025, James Pearce"
echo "Supports Ubuntu 24.04 and RaspberryPi OS (CLI only)"
echo ""
read -rp "Do you wish to continue (you will be prompted for sudo)? [y/N] " confirm
STEP_ORDER+=("User confirmation")
if [[ "$confirm" =~ ^[Yy] ]]; then
  STATUS["User confirmation"]="Accepted"
else
  STATUS["User confirmation"]="Aborted by user"
  echo "Aborted. No changes made."
  print_summary
  exit 0
fi

# 2. Offer appliance mode (Raspberry Pi OS only)
APPLIANCE_MODE=0
SPLASH_PNG=""
if is_raspberry_pi; then
  read -rp "Enable appliance mode (boot straight into the game, power off on quit)? [y/N] " appliance_confirm
  STEP_ORDER+=("Appliance mode prompt")
  if [[ "$appliance_confirm" =~ ^[Yy] ]]; then
    STATUS["Appliance mode prompt"]="Accepted"
    while true; do
      read -rp "Splash image PNG [res/appliance/splash.png]: " splash_input
      splash_input=${splash_input:-res/appliance/splash.png}
      if [[ ! -f "$splash_input" ]]; then
        echo "    [ERROR] File not found: $splash_input"
        continue
      fi
      if [[ "$(head -c 8 "$splash_input" | od -An -tx1 | tr -d ' 
')" != "89504e470d0a1a0a" ]]; then
        echo "    [ERROR] Not a PNG file: $splash_input"
        continue
      fi
      SPLASH_PNG=$(cd "$(dirname "$splash_input")" && pwd)/$(basename "$splash_input")
      break
    done
    APPLIANCE_MODE=1
    STEP_ORDER+=("Splash image selection")
    STATUS["Splash image selection"]="$SPLASH_PNG"
  else
    STATUS["Appliance mode prompt"]="Declined"
  fi
else
  echo "Appliance mode is only offered on Raspberry Pi OS; skipping."
fi

# 2a. Offer a recommended overclock (Raspberry Pi 2/3-class boards only,
#     independent of appliance mode).
OVERCLOCK=0
GPU_FREQ=""
if is_raspberry_pi; then
  part="$(cpu_part)"
  if [[ "$part" == "0xc07" || "$part" == "0xd03" ]]; then
    model="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
    read -rp "Apply recommended overclocks for $model? Note: this requires a 2.5A PSU and a heatsink on the CPU. [y/N] " oc_confirm
    STEP_ORDER+=("Overclock prompt")
    if [[ "$oc_confirm" =~ ^[Yy] ]]; then
      OVERCLOCK=1
      if [[ "$part" == "0xc07" ]]; then GPU_FREQ=450; else GPU_FREQ=350; fi
      STATUS["Overclock prompt"]="Accepted (gpu_freq=$GPU_FREQ)"
    else
      STATUS["Overclock prompt"]="Declined"
    fi
  fi
fi

# 3. Determine build threads based on RAM
STEP_ORDER+=("Determine build threads")
{
  mem_kb=$(grep MemTotal /proc/meminfo | awk '{print $2}')
  mem_mb=$((mem_kb/1024))
  if (( mem_mb <= 460 )); then threads=1
  elif (( mem_mb <= 920 )); then threads=2
  elif (( mem_mb <= 1380 )); then threads=3
  else threads=4; fi
  export NUMTHREADS=$threads
  STATUS["Determine build threads"]="Using $NUMTHREADS threads ($mem_mb MB RAM)"
  echo "    [INFO] Using $NUMTHREADS build thread(s) based on ${mem_mb}MB RAM."
} || {
  STATUS["Determine build threads"]="Failed"
  echo "    [ERROR] Could not determine RAM size. Defaulting NUMTHREADS=1."
  export NUMTHREADS=1
}

# 4. Update APT
run_step "APT update" sudo apt update -y

# 5. Grant input, video and render group access
run_step "Grant input group access" sudo usermod -aG input "$USER"
run_step "Grant video group access" sudo usermod -aG video "$USER"
run_step "Grant render group access" bash -c 'if getent group render >/dev/null; then sudo usermod -aG render "$USER"; fi'

# 6. Configure udev rules
run_step "Configure udev rules" bash -c '
  sudo tee /etc/udev/rules.d/99-cannonball.rules > /dev/null <<EOF
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", MODE="0666"
KERNEL=="watchdog*", SUBSYSTEM=="watchdog", MODE="0666"
EOF
  sudo udevadm control --reload-rules
  sudo udevadm trigger
'

# 6a. OPTIONAL - tighter hidraw permissions for the rumble subsystem's raw-HID profiles
#     (tools/INPUT_BINDING_DESIGN.md B.7.7). Step 6 above already grants world read/write on every
#     hidraw node (0666), which is sufficient for HID_REPORT rumble to work, so this step is
#     NOT run automatically. If you would rather restrict hidraw access to members of the
#     `input` group and scope it to a specific vendor (e.g. Thrustmaster, 0x044f) instead of
#     every hidraw device on the system, uncomment the block below (and adjust idVendor) before
#     running this script, or apply it by hand afterwards:
#
# sudo tee /etc/udev/rules.d/99-cannonball-hidraw.rules > /dev/null <<EOF
# KERNEL=="hidraw*", ATTRS{idVendor}=="044f", MODE="0660", GROUP="input"
# EOF
# sudo udevadm control --reload-rules
# sudo udevadm trigger
#
#     Note this rule only takes effect for a matching device if it is more specific than (or
#     applied instead of) the blanket 99-cannonball.rules rule above.

# 7. Grant real-time audio scheduling (rtprio), used by the audio mix/SDL
#    threads to avoid ALSA underruns under CPU load (see
#    tools/AUDIO_PERF_PLAN.md). Falls back gracefully at runtime if this
#    step is skipped or fails - the game just runs without RT priority.
run_step "Grant audio group rtprio access" bash -c '
  sudo usermod -aG audio "$USER"
  sudo cp res/50-cannonball-audio.conf /etc/security/limits.d/50-cannonball-audio.conf
'

# 8. Install dependencies
run_step "Install dependencies" sudo apt install -y \
  build-essential git cmake libsdl2-dev libglu1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev libmpg123-dev pkg-config alsa-utils libtinyxml2-dev

# 9. Prepare and build CannonBall
run_step "Prepare build directories" mkdir -p build roms
run_step "Configure Project with CMake" cmake -S . -B build
run_step "Compile" cmake --build build --parallel $NUMTHREADS
#cd build && cmake ../cmake && make -j"$NUMTHREADS"'

# 10. Appliance mode setup (only when chosen above, on Raspberry Pi OS)
if [[ "$APPLIANCE_MODE" -eq 1 ]]; then
  STATE_DIR=/etc/cannonball-se
  STATE_FILE=$STATE_DIR/appliance.state

  # 10a. Install appliance packages, recording only the ones we install so
  #      uninstall-appliance.sh removes only what we added.
  run_step "Install appliance packages" bash -c '
    set -e
    sudo install -d '"$STATE_DIR"'
    sudo touch '"$STATE_FILE"'
    to_install=""
    newly_installed=""
    for p in plymouth plymouth-themes initramfs-tools; do
      # Check for the fully-installed state specifically: a package left in
      # "config-files" state by a plain apt remove still satisfies "dpkg -s",
      # but its programs (e.g. plymouth-set-default-theme) are gone, so it
      # must be reinstalled.
      if ! dpkg-query -W -f="\${Status}" "$p" 2>/dev/null | grep -q "^install ok installed$"; then
        to_install="$to_install $p"
        newly_installed="$newly_installed $p"
      fi
    done
    if [[ -n "$to_install" ]]; then
      # shellcheck disable=SC2086
      sudo apt install -y $to_install
    fi
    old=$(grep "^packages_installed=" '"$STATE_FILE"' 2>/dev/null | cut -d= -f2-)
    combined=$(printf "%s\n" $old $newly_installed | sed "/^$/d" | sort -u | tr "\n" " ")
    combined=${combined% }
    sudo sed -i "/^packages_installed=/d" '"$STATE_FILE"'
    echo "packages_installed=$combined" | sudo tee -a '"$STATE_FILE"' > /dev/null
  '

  # 10b. Install the splash theme.
  run_step "Install splash theme" bash -c '
    set -e
    sudo install -d /usr/share/plymouth/themes/cannonball-se
    sudo install -m 0644 "$1" /usr/share/plymouth/themes/cannonball-se/splash.png
    sudo install -m 0644 res/appliance/cannonball-se.plymouth /usr/share/plymouth/themes/cannonball-se/cannonball-se.plymouth
    sudo install -m 0644 res/appliance/cannonball-se.script /usr/share/plymouth/themes/cannonball-se/cannonball-se.script
    sudo install -d '"$STATE_DIR"'
    if [[ -f /etc/plymouth/plymouthd.conf ]] && [[ ! -f '"$STATE_DIR"'/plymouthd.conf.orig ]]; then
      sudo cp /etc/plymouth/plymouthd.conf '"$STATE_DIR"'/plymouthd.conf.orig
    fi
    sudo touch '"$STATE_FILE"'
    sudo sed -i "/^theme=/d" '"$STATE_FILE"'
    echo "theme=cannonball-se" | sudo tee -a '"$STATE_FILE"' > /dev/null
    # Make sure the initramfs carries the I2C DDC adapter before the vc4 KMS
    # driver (vc4 defers its HDMI component on i2c_bcm2835), so the splash
    # theme actually shows during boot instead of a grey text screen. Order
    # matters; only record modules we did not already find present.
    modules_file=/etc/initramfs-tools/modules
    sudo touch "$modules_file"
    modules_added=""
    for m in i2c_bcm2835 vc4; do
      grep -qx "$m" "$modules_file" 2>/dev/null || modules_added="$modules_added $m"
      sudo sed -i "/^${m}\$/d" "$modules_file"
    done
    for m in i2c_bcm2835 vc4; do
      echo "$m" | sudo tee -a "$modules_file" > /dev/null
    done
    sudo sed -i "/^modules_added=/d" '"$STATE_FILE"'
    echo "modules_added=${modules_added# }" | sudo tee -a '"$STATE_FILE"' > /dev/null
    # Give Plymouth longer to wait for the display device to appear before
    # falling back to a text splash.
    sudo sed -i "/^DeviceTimeout=/d" /etc/plymouth/plymouthd.conf
    if grep -q "^\[Daemon\]" /etc/plymouth/plymouthd.conf 2>/dev/null; then
      sudo sed -i "/^\[Daemon\]/a DeviceTimeout=20" /etc/plymouth/plymouthd.conf
    else
      printf "\n[Daemon]\nDeviceTimeout=20\n" | sudo tee -a /etc/plymouth/plymouthd.conf > /dev/null
    fi
    sudo plymouth-set-default-theme -R cannonball-se
  ' _ "$SPLASH_PNG"

  # 10c. Configure boot options: cmdline.txt (single line, word-match append)
  #      and config.txt (disable_splash=1 under a trailing [all] section).
  run_step "Configure boot options" bash -c '
    set -e
    cmdline_file=/boot/firmware/cmdline.txt
    config_file=/boot/firmware/config.txt
    added=""
    for opt in quiet splash plymouth.ignore-serial-consoles loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0 systemd.show_status=0 rd.systemd.show_status=0; do
      if ! grep -qw -- "$opt" "$cmdline_file"; then
        sudo sed -i "1 s/\$/ $opt/" "$cmdline_file"
        added="$added $opt"
      fi
    done
    config_added=""
    if ! grep -q "^disable_splash=1" "$config_file" 2>/dev/null; then
      printf "\n[all]\ndisable_splash=1\n" | sudo tee -a "$config_file" > /dev/null
      config_added="disable_splash=1"
    fi
    sudo install -d '"$STATE_DIR"'
    sudo touch '"$STATE_FILE"'
    old_cmdline=$(grep "^cmdline_added=" '"$STATE_FILE"' 2>/dev/null | cut -d= -f2-)
    combined_cmdline=$(printf "%s\n" $old_cmdline $added | sed "/^$/d" | sort -u | tr "\n" " ")
    sudo sed -i "/^cmdline_added=/d" '"$STATE_FILE"'
    echo "cmdline_added=${combined_cmdline% }" | sudo tee -a '"$STATE_FILE"' > /dev/null
    old_config=$(grep "^config_added=" '"$STATE_FILE"' 2>/dev/null | cut -d= -f2-)
    combined_config=$(printf "%s\n" $old_config $config_added | sed "/^$/d" | sort -u | tr "\n" " ")
    sudo sed -i "/^config_added=/d" '"$STATE_FILE"'
    echo "config_added=${combined_config% }" | sudo tee -a '"$STATE_FILE"' > /dev/null
  '

  # 10d. Install the appliance service unit and the power-action helper.
  run_step "Install appliance service" bash -c '
    set -e
    svc_user="${USER:-$(id -un)}"
    svc_group="$(id -gn "$svc_user")"
    checkout="$(pwd)"
    sed -e "s|@USER@|$svc_user|g" -e "s|@GROUP@|$svc_group|g" -e "s|@CHECKOUT@|$checkout|g" \
      res/appliance/cannonball-se.service.in | sudo tee /etc/systemd/system/cannonball-se.service > /dev/null
    sudo install -d /usr/local/lib/cannonball-se
    sudo install -o root -g root -m 0755 res/appliance/power-action.sh /usr/local/lib/cannonball-se/power-action.sh
    sudo install -d '"$STATE_DIR"'
    sudo touch '"$STATE_FILE"'
    sudo sed -i "/^user=/d;/^checkout=/d" '"$STATE_FILE"'
    { echo "user=$svc_user"; echo "checkout=$checkout"; } | sudo tee -a '"$STATE_FILE"' > /dev/null
  '

  # 10e. Enable the service (do not start it now; it starts on next boot).
  run_step "Enable appliance service" bash -c '
    set -e
    sudo systemctl disable getty@tty1.service
    # Leave the splash logo on screen after Plymouth quits, until the game
    # itself draws, instead of dropping back to a blank/text console.
    sudo install -d /etc/systemd/system/plymouth-quit.service.d
    printf "[Service]\nExecStart=\nExecStart=-/usr/bin/plymouth quit --retain-splash\n" | sudo tee /etc/systemd/system/plymouth-quit.service.d/retain-splash.conf > /dev/null
    sudo systemctl daemon-reload
    sudo systemctl enable cannonball-se.service
    sudo install -d '"$STATE_DIR"'
    sudo touch '"$STATE_FILE"'
    sudo sed -i "/^getty_disabled=/d;/^retain_dropin=/d" '"$STATE_FILE"'
    { echo "getty_disabled=1"; echo "retain_dropin=1"; } | sudo tee -a '"$STATE_FILE"' > /dev/null
  '
fi

# 10f. Apply the recommended overclock, if accepted above. Independent of
#      appliance mode, and not reverted by --remove-appliance; the marker
#      comment lets a user remove it by hand.
if [[ "$OVERCLOCK" -eq 1 ]]; then
  run_step "Apply recommended overclock" bash -c '
    set -e
    config_file=/boot/firmware/config.txt
    if ! grep -q "^# CannonBall-SE recommended overclock" "$config_file" 2>/dev/null; then
      printf "\n# CannonBall-SE recommended overclock\n[all]\ngpu_freq='"$GPU_FREQ"'\n" | sudo tee -a "$config_file" > /dev/null
    fi
  '
fi

# 11. Final summary
print_summary

# 12. Prompt to view the man page
read -rp "Would you like to view the man page now? [y/N] " view_man
STEP_ORDER+=("View man page prompt")
if [[ "$view_man" =~ ^[Yy] ]]; then
  STATUS["View man page prompt"]="Displayed"
  man -l docs/cannonball-se.6
else
  STATUS["View man page prompt"]="Skipped"
  echo "You can view it later with: man -l docs/cannonball-se.6"
fi
echo "****************************************************************************************"
if [[ "$APPLIANCE_MODE" -eq 1 ]]; then
  echo "IMPORTANT: You must reboot before running CannonBall-SE, and remember to copy in the"
  echo "           ROMS. After rebooting, the Pi will boot straight into the game."
  echo ""
  echo "           Press Escape in-game to quit and power off. A login is still available"
  echo "           on tty2 (Ctrl+Alt+F2) and over ssh."
  echo ""
  echo "           Sound output, wheels and gamepads are configured from the in-game menu."
  echo ""
  echo "           To revert appliance mode, run: ./install.sh --remove-appliance"
else
  echo "IMPORTANT: You must reboot before running CannonBall-SE, and remember to copy in the"
  echo "           ROMS. After rebooting, start CannonBall-SE from this directory using:"
  echo ""
  echo "           build/cannonball-se"
  echo ""
  echo "           Sound output, wheels and gamepads are configured from the in-game menu."
fi
echo "****************************************************************************************"
echo ""

# 13. Warn if under-voltage was seen during install (bit 0 = currently
#     throttled, bit 16 = has occurred since boot).
if is_raspberry_pi && command -v vcgencmd >/dev/null 2>&1; then
  thr="$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)"
  if [[ -n "$thr" ]] && (( thr & 0x10001 )); then
    printf '\033[1;33m%s\033[0m\n' "Undervoltage was recorded during install. PSU may not be adequate to run Cannonball-SE at 60fps."
  fi
fi

# End of install.sh
