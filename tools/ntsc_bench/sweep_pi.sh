#!/usr/bin/env bash
# Working-set sweep, runs ON the Pi: times one prebuilt bench binary for a list
# of --colours values under the performance governor, pinned to core 3.
# Usage (on the Pi):  sweep_pi.sh <bench binary> <seconds> <colours...>
# Ship it with:       rsync tools/ntsc_bench/sweep_pi.sh sl-d303:~/ntsc_bench/
set -u
BIN=$1; SECS=$2; shift 2
setgov() { for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$1" | sudo tee "$g" >/dev/null; done; }
setgov performance
echo "throttled(before)=$(vcgencmd get_throttled | cut -d= -f2)"
for c in "$@"; do
  taskset -c 3 "$BIN" --seconds "$SECS" --colours "$c" | grep "^RESULT" | sed -E "s/.*fn=([a-z_0-9]+).*colours=([0-9]+).*ms=([0-9.]+).*/\1 colours=\2 ms=\3/"
done
echo "throttled(after)=$(vcgencmd get_throttled | cut -d= -f2)"
setgov ondemand
