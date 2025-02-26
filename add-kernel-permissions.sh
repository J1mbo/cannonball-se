sudo tee /etc/udev/rules.d/99-cannonball.rules <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", MODE="0666"
KERNEL=="watchdog*", SUBSYSTEM=="watchdog", MODE="0666"

EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
