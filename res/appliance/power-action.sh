#!/bin/sh
# power-action.sh - ExecStopPost helper for the cannonball-se appliance service.
#
# systemd sets $SERVICE_RESULT, $EXIT_CODE and $EXIT_STATUS for ExecStopPost
# commands (see systemd.exec(5)). We only want to power the machine off when
# the game itself exited cleanly (player pressed Escape, used the menu EXIT,
# or closed the window), which the game reports as exit status 0.
#
# The $EXIT_CODE = "exited" test matters: if the service is stopped by
# systemd (systemctl stop cannonball-se, or a shutdown/reboot already in
# progress), the game is killed by SIGTERM and dies by signal. systemd then
# reports SERVICE_RESULT=success but EXIT_CODE=killed - not "exited" - so
# that case falls through to the no-op exit below instead of also powering
# off (which would fight a maintenance stop or double up on a real
# shutdown). Only a genuine "exited with status 0" reaches poweroff.
#
# Exit status 1 (configuration problem, e.g. missing ROMs) is handled here
# too: RestartPreventExitStatus=1 in the unit stops systemd restarting the
# game in that case, so we restore the tty1 login instead of leaving the
# console idle.

if [ "${SERVICE_RESULT:-}" = "success" ] && [ "${EXIT_CODE:-}" = "exited" ] && [ "${EXIT_STATUS:-}" = "0" ]; then
    exec /usr/sbin/poweroff
fi

if [ "${EXIT_CODE:-}" = "exited" ] && [ "${EXIT_STATUS:-}" = "1" ]; then
    logger -t cannonball-se "game exited with status 1 (configuration problem); restoring the console login"
    systemctl start --no-block getty@tty1.service
fi

exit 0
