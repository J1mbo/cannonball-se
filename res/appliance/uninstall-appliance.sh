#!/usr/bin/env bash
#
# uninstall-appliance.sh - revert CannonBall-SE appliance mode.
#
# Run with sudo (also invoked by `install.sh --remove-appliance`). Reads
# /etc/cannonball-se/appliance.state, undoes each recorded change, and is
# safe to run more than once: every step tolerates "already gone".

set -uo pipefail

STATE_DIR=/etc/cannonball-se
STATE_FILE=$STATE_DIR/appliance.state

if [[ $EUID -ne 0 ]]; then
  echo "This script must be run as root (try: sudo $0)." >&2
  exit 1
fi

if [[ ! -f "$STATE_FILE" ]]; then
  echo "No appliance.state file found ($STATE_FILE); appliance mode does not appear to be installed."
  echo "Nothing to do."
  exit 0
fi

# Read a key's value out of the state file, empty if absent.
state_get() {
  grep "^$1=" "$STATE_FILE" 2>/dev/null | tail -n1 | cut -d= -f2-
}

echo "Reverting CannonBall-SE appliance mode..."

# 1. Stop and disable the service, then remove the unit and helper script.
systemctl disable --now cannonball-se.service >/dev/null 2>&1 || true
rm -f /etc/systemd/system/cannonball-se.service
rm -rf /usr/local/lib/cannonball-se

getty_disabled=$(state_get getty_disabled)
if [[ "$getty_disabled" == "1" ]]; then
  systemctl enable getty@tty1.service >/dev/null 2>&1 || true
fi

# 2. Restore the plymouth theme/config.
if [[ -f "$STATE_DIR/plymouthd.conf.orig" ]]; then
  cp "$STATE_DIR/plymouthd.conf.orig" /etc/plymouth/plymouthd.conf 2>/dev/null || true
elif command -v plymouth-set-default-theme >/dev/null 2>&1; then
  plymouth-set-default-theme -R -r >/dev/null 2>&1 || true
fi
rm -rf /usr/share/plymouth/themes/cannonball-se
# The initramfs carries a copy of plymouthd.conf and the theme; rebuild it so a
# still-installed plymouth does not keep booting with the removed theme.
if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u >/dev/null 2>&1 || true
fi

# 3. Strip the cmdline.txt options we added, word-boundary safe (as
#    raspi-config does), and remove disable_splash=1 from config.txt.
cmdline_file=/boot/firmware/cmdline.txt
if [[ -f "$cmdline_file" ]]; then
  for opt in $(state_get cmdline_added); do
    esc_opt=$(printf '%s\n' "$opt" | sed 's/[.[\*^$/]/\\&/g')
    sed -i -E "s/(^|[[:space:]])${esc_opt}([[:space:]]|\$)/\1\2/g" "$cmdline_file"
  done
  # Collapse any doubled-up spaces left behind by the removals.
  sed -i -E 's/[[:space:]]+/ /g; s/^ //; s/ $//' "$cmdline_file"
fi

config_file=/boot/firmware/config.txt
if [[ -f "$config_file" ]] && [[ "$(state_get config_added)" == *disable_splash=1* ]]; then
  sed -i '/^disable_splash=1$/d' "$config_file"
fi

# 4. Remove only the packages we installed.
packages_installed=$(state_get packages_installed)
if [[ -n "$packages_installed" ]]; then
  # shellcheck disable=SC2086
  apt remove -y $packages_installed || true
fi

# 5. Remove the state file, the plymouth backup, and the (now empty) directory last.
rm -f "$STATE_FILE" "$STATE_DIR/plymouthd.conf.orig"
rmdir "$STATE_DIR" 2>/dev/null || true

systemctl daemon-reload || true

echo "Appliance mode reverted. Reboot to return to a normal text console."
