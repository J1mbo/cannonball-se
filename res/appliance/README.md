# res/appliance/

Files used by `install.sh` to set up "appliance mode" on Raspberry Pi OS
(boot straight into CannonBall-SE, power off on quit). Everything here is
installed to system locations by `install.sh`; nothing runs from this
directory directly except at install/uninstall time.

| File | Installed to | Purpose |
|---|---|---|
| `splash.png` | `/usr/share/plymouth/themes/cannonball-se/splash.png` | Boot/shutdown splash image (or supply your own at the install prompt). |
| `cannonball-se.plymouth` | `/usr/share/plymouth/themes/cannonball-se/cannonball-se.plymouth` | Plymouth theme descriptor. |
| `cannonball-se.script` | `/usr/share/plymouth/themes/cannonball-se/cannonball-se.script` | Plymouth script: scales and centres the splash, drawn in every mode (boot and shutdown). |
| `cannonball-se.service.in` | `/etc/systemd/system/cannonball-se.service` | Service unit template, rendered with the invoking user/group/checkout path by `install.sh`. |
| `power-action.sh` | `/usr/local/lib/cannonball-se/power-action.sh` | `ExecStopPost` helper: powers off on a clean quit, restores the tty1 login on a configuration error, otherwise lets `Restart=on-failure` handle it. |
| `uninstall-appliance.sh` | run in place (also via `install.sh --remove-appliance`) | Reverts every change below using `/etc/cannonball-se/appliance.state`. |

`install.sh` records what it changed in `/etc/cannonball-se/appliance.state`
so uninstall only removes what it added.

## Manual equivalents

If you would rather not use the installer, the same result can be put
together by hand:

1. `sudo apt install -y plymouth plymouth-themes initramfs-tools`
2. `sudo install -d /usr/share/plymouth/themes/cannonball-se` and copy in
   `splash.png`, `cannonball-se.plymouth`, `cannonball-se.script`, then
   `sudo plymouth-set-default-theme -R cannonball-se`.
3. Append `quiet splash plymouth.ignore-serial-consoles loglevel=3
   logo.nologo vt.global_cursor_default=0 consoleblank=0` to the single line
   in `/boot/firmware/cmdline.txt`, and add `disable_splash=1` under an
   `[all]` section in `/boot/firmware/config.txt`.
4. Render `cannonball-se.service.in` (substitute `@USER@`, `@GROUP@`,
   `@CHECKOUT@` for your user, its primary group, and the absolute path of
   your CannonBall-SE checkout) to `/etc/systemd/system/cannonball-se.service`,
   and install `power-action.sh` to
   `/usr/local/lib/cannonball-se/power-action.sh` (root:root, mode 0755).
5. `sudo systemctl disable getty@tty1.service`,
   `sudo systemctl daemon-reload`, `sudo systemctl enable cannonball-se.service`,
   then reboot.

To revert, run `sudo ./uninstall-appliance.sh` (or `./install.sh --remove-appliance`
from the repository root).
