# Q25 Plasma defaults

The image installs **Plasma Desktop** and **Plasma Mobile** from the ARM package
repositories. Both use Wayland/KWin and the normal KDE session launchers. There
are no replacement settings applications, terminal wrappers, session daemons, or
patched Plasma components. The defaults were checked against Plasma **6.8.2** and
SDDM **0.21.0**; recheck their configuration schemas when updating packages.

## Login and session selection

* SDDM starts the packaged `plasma.desktop` session automatically as `Q25_USER`
  (`alarm` by default). `configure` substitutes that same user into the SDDM
  configuration. `graphical.target` and `display-manager.service` select SDDM.
* KScreenLocker has `LockOnStart=true`, no grace period, and password protection.
  Autologin starts the session, **not an unlocked desktop**. The build's existing
  initial password is `alarm`; change it before using the image on a network.
* Log out to reach the standard Breeze SDDM greeter. Choose **Plasma Mobile** in
  its session menu. `Relogin=false` prevents an autologin loop after logout.
  On reboot the configured default is still Plasma Desktop.
* Mobile keeps its upstream shell and its own `~/.config/plasma-mobile` defaults
  layer. The Q25 Desktop look-and-feel only supplies Breeze defaults and an
  initial layout of stock applets; it does not replace Mobile's home screen.

## Display and input

`/etc/environment.d/60-q25-display.conf` selects `/dev/dri/q25-display`, the
existing udev symlink matched by the `mediatek-drm` driver. Do not hard-code a
`cardN`: the Panfrost render GPU is not the display controller. Mesa's MediaTek
kmsro driver pairs display scanout with Panfrost rendering. Software cursors are
selected for the current display driver. SDDM filters inherited environment
variables, so its **GreeterEnvironment** also explicitly supplies both settings.

The built-in, EDID-less `DSI-1` has a 720×720 native mode. KWin's system-wide
`kwinoutputconfig.json` seeds **1.5×** scaling, giving a **480×480 logical** desktop.
A user's saved display configuration takes precedence. Fonts use logical 96 DPI
(no extra `QT_SCALE_FACTOR` or forced font DPI). Noto Sans 10 pt, 32 px toolbar
icons, permanent tablet mode and a 48-logical-pixel panel provide larger touch
controls without reducing the workspace to 360×360.

The stock Kickoff popup is seeded to fill most of the available area, with an
icon task manager, system tray and clock alongside it. Normal application
windows initially maximize, with maximized decorations hidden. The rule uses
**Apply**, not Force; dialogs, menus and the keyboard are excluded, and users can
restore windows. Desktop applications can still have minimum sizes larger than
this screen: check the actual touch layout on hardware.

Plasma Keyboard is KWin's input method. `VirtualKeyboardMode=1` is the native
**NonMouseInput** policy: show it on touch/stylus focus, not merely on hardware
keyboard or mouse focus. The Q25 keyd mapping and native input helpers are
unchanged. Kitty is the default terminal and uses JetBrainsMono Nerd Font Mono
10 pt; fontconfig supplies Noto CJK and color emoji fallback. Firefox, Dolphin,
KWrite, Okular, Gwenview, Ark, Spectacle and Plasma System Monitor are normal
packaged applications. The user-level browser seed prevents Mobile's optional
Angelfish default from pointing at an uninstalled browser.

## Locking and power

KScreenLocker locks after two minutes idle and immediately at session start.
PowerDevil dims, locks before DPMS-off, and switches the display off without
suspending the machine. Its tablet-mode short power-key action is lock + screen
off; the long-press action is the normal power menu. Logind ignores power keys
outside the desktop, and the existing systemd sleep prohibitions remain intact.
All AC/battery profiles disable automatic suspend. There is no claim that system
suspend/resume is working.

Networking, the 48 kHz PipeWire setup, keyd, USB gadget, firstboot, Bluetooth and
boot-success services remain unchanged. ModemManager is included for the modem
integration; its service ownership is separate from the desktop configuration.

## Offline checks

```sh
bash -n build/build-rootfs.sh
python3 build/test-plasma-config.py -v
node --check rootfs/usr/share/plasma/look-and-feel/org.zinwa.q25.desktop/contents/layouts/org.kde.plasma.desktop-layout.js
# After installing the package list and applying overlay/configure:
Q25_TEST_ROOT=/home/maxsun/q25-work/rootfs python3 build/test-plasma-config.py -v
sudo systemd-analyze --root=/home/maxsun/q25-work/rootfs --man=no verify sddm.service
```

The staging test additionally checks both packaged session entries, UID 1000,
the user's browser seed, SDDM's display-manager link and graphical target. For a
non-default user, also set `Q25_TEST_USER` to the build's `Q25_USER`.

Cross-architecture package installation with host `pacman --root` can leave
post-transaction hooks incomplete: run `systemd-tmpfiles --create` in
`arch-chroot`, and regenerate `systemd-hwdb --root=... update` with the host tool
if qemu-user reports `Function not implemented`. Verify dependencies with
`pacman -T` against the complete package list (KWrite is provided by Kate in this
repository snapshot). These are offline staging workarounds, not device hooks.

## Required device checks (not performed by this change)

1. Boot into the standard lock screen, unlock, confirm 720×720 at 1.5× and
   Panfrost rendering. Log out, select Mobile, then return to Desktop.
2. Check Kickoff and tray popup reachability, app maximization, dialogs and
   keyboard focus. Type using the physical keyboard without an unwanted OSK;
   touch a text field to show it. Check kitty Nerd Font symbols, CJK and emoji.
3. Check short power press locks/blanks and another press wakes to the lock
   screen. Check idle dim/blank and the long-press menu. Confirm no suspend.
4. Recheck Wi-Fi, LTE UI, Bluetooth, audio, volume keys, touchpad, USB networking
   and persistence after reboot. Package/configuration checks do not prove
   hardware interaction or uninterrupted first-frame locking.
