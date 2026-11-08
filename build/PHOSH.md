# Q25 stock Phosh phone image

**Phosh is the only configured desktop.** This is a complete phone application
set, not just phosh/phoc. All GUI programs, session launchers and services come
from upstream packages; there is no Q25 shell, control center, keyboard wrapper,
or UI resource overlay. Hardware/modem helpers remain separate and unchanged.

## Applications and repositories

The ARM repository databases were queried with `pacman --config
build/pacman-build.conf --dbpath /tmp/q25-phosh-db -Si ...` and `-Ss` before
choosing the package list. Use ARM sync databases (e.g. copy the staged root's
`var/lib/pacman/sync/` into a temporary DB path): changing `Architecture` alone
does **not** make a host's existing x86_64 sync databases into ARM databases.

Checked versions: phosh/phoc **0.57.0-2**, phosh-mobile-settings **0.57.0-1**,
Stevia **0.58.0-1**, xdg-desktop-portal-phosh **0.58.0-1**. ARM packages provide:

| Purpose | Packages / visible applications |
|---|---|
| Phone session | phosh, phoc, phosh-mobile-settings, stevia |
| Settings and integration | gnome-control-center, gnome-settings-daemon, gnome-keyring, feedbackd, iio-sensor-proxy, xdg-desktop-portal-phosh, xdg-desktop-portal-gtk |
| Calls, messaging, address book | gnome-calls, chatty, gnome-contacts |
| Everyday utilities | gnome-clocks, gnome-weather, gnome-calculator, gnome-text-editor |
| Files and pictures | nautilus, loupe; Evince is also a Phosh dependency |
| Terminal and browser | **kitty**, gnome-console, **Firefox** |
| Maps and camera | gnome-maps, snapshot |
| Appearance | adwaita-icon-theme, adwaita-cursors, gnome-backgrounds; Noto Sans, CJK, emoji and JetBrainsMono Nerd Font retained |

Names **not available** in these repos: `phosh-osk-stevia` (the package is called
`stevia`, its binary is `phosh-osk-stevia`), `portfolio-file-manager`, `megapixels`,
`phosh-wallpapers`. Squeekboard is available, but not installed alongside Stevia
(they conflict). Stevia supports the same Phosh OSK interface and explicitly
suppresses automatic activation when a hardware keyboard is present via
`mobi.phosh.osk ignore-hw-keyboards=false`. The OSK remains available manually.
This detection, including the keyd virtual keyboard, still needs a device test.

GNOME Software/Flatpak are not selected. The GNOME portal backend is pulled in
transitively, but Phosh's packaged portal selection uses its own backend plus
GTK/wlr fallbacks and the keyring Secret portal.
`gnome-shell`/Mutter are unavoidable upstream Phosh/Settings dependencies in this
repository snapshot, **not an enabled alternative desktop**; no GDM is enabled.
Snapshot is an ordinary camera app, not camera enablement: expect a no-camera
screen until a supported capture device exists. Calls/Chatty do not establish
that voice/SMS or modem audio routing work on this port.

Removed: SDDM, plasma-desktop/workspace/mobile/settings/nm/pa/keyboard/systemmonitor,
KWin, PowerDevil, KScreen/KScreenLocker, Bluedevil, polkit-kde-agent, kde-gtk-config,
breeze-gtk, xdg-desktop-portal-kde, Dolphin, KWrite (provided by Kate), Okular,
Gwenview, Ark, Spectacle and KCalc, plus all their Q25-specific session/theme
configuration. Btrfs Assistant remains as a desktop-neutral storage tool.

## Login, password and keyring

Arch's phosh package ships `phosh-session`, the GNOME-session user units and
`/etc/pam.d/phosh`, but **does not ship a system phosh.service**. The image installs
the unmodified [upstream v0.57.0 system unit](https://gitlab.gnome.org/World/Phosh/phosh/-/blob/v0.57.0/data/phosh.service)
in `/etc/systemd/system/phosh.service` (source URL in its header). The small
`phosh.service.d/q25.conf` drop-in supplies the default user, `PAMName=phosh`,
Wayland user-session class and device environment. It also excludes getty on
the unit's tty7; upstream already excludes tty1. No custom launcher is used.

`configure` enables this unit and selects `graphical.target`. The unit starts
stock **phosh-session as UID 1000**, then phoc and GNOME-session start the packaged
Phosh shell **without `--unlocked`/`-U`**. Autologin is into Phosh's lock screen,
not a passwordless unlock. PAM uses the Arch `system-local-login` authentication
stack (including `pam_unix` and faillock). Standard optional GNOME keyring PAM
auth/session entries start the keyring and unlock it with the entered password.
Check first-login keyring behavior on hardware; an existing keyring with a
different password can still prompt. This is not a custom PAM authenticator.

`Q25_USER` defaults to `alarm`; optional **`Q25_PASSWORD` defaults to the user
name** for compatibility. Both are passed through `build-all.sh`'s sudo boundary.
Never put a real password in tracked files or a build command's literal text:

```sh
read -rsp 'Initial account password / PIN: ' Q25_PASSWORD; echo
export Q25_PASSWORD
BUILDROOT="$HOME/q25-work/mm-buildroot" ./build/build-all.sh > "$HOME/q25-work/build-all.log" 2>&1
unset Q25_PASSWORD
```

The lock screen presents a **numeric keypad**, so a numeric account password is
recommended; the hardware keyboard still accepts alphanumeric passwords. The
same password authenticates sudo/SSH. A short PIN weakens those too: choose a
sufficiently long password and change the default before network use. Faillock
stays at `deny=10`, `unlock_time=120`; root remains locked and wheel sudo requires
a password. Secure first-frame locking is a required device acceptance check,
not something a cross-build can prove.

## Display, input and Settings

The service environment sets `WLR_DRM_DEVICES=/dev/dri/q25-display` using the
existing mediatek-drm udev symlink and `WLR_NO_HARDWARE_CURSORS=1`. Do not replace
this with a numbered card or force the Panfrost render GPU as the display device.
No KWin variables or global Qt/GTK scale overrides remain.

Stock `phosh-session` reads `/etc/phosh/phoc.ini`. Its `DSI-1` default is **1.25×**:
720×720 physical, **576×576 logical** pixels. Use **Settings > Displays** to change
scale; saved per-user display settings can supersede the seed. Phosh's usual
maximizing compositor behavior is retained. The physical keyd layout is unchanged.

Use **Settings** (gnome-control-center) for **Wi-Fi, Mobile Network, Bluetooth,
Sound, Displays, Power and Users**; networking uses NetworkManager/ModemManager,
not scripts. **nmtui** is supplied by NetworkManager. **Mobile Settings** provides
Phosh-specific preferences, OSK controls and per-app Scale-to-Fit.

Libadwaita apps generally adapt, but no app has been visually validated on this
image's 576×576 viewport. GNOME Settings' sidebar breakpoint is 550sp and Mobile
Settings' is 500sp: 576-wide can keep sidebars visible, leaving narrow content.
Firefox is stock desktop Firefox (no custom mobile CSS) and its dialogs, advanced
Calculator modes, Nautilus dialogs and Btrfs Assistant may need more space.
Try per-app Scale-to-Fit in Mobile Settings, or choose 1.5× for narrow layouts /
1× for more logical room. Scale-to-Fit shrinks controls; it is not true reflow.
An OSK takes away more height. Do not promise every settings dialog fits.

## Defaults and power

`90-q25-phosh.gschema.override` supplies **defaults**, not locked dconf policy.
Desktop-qualified `:Phosh` sections override the package's own Phosh defaults.
The build explicitly runs `glib-compile-schemas --strict` after installing the
overlay, so missing keys/types fail the build.

Favorites are kitty, Firefox, Settings, Files, Calls, Chats, Contacts, Clocks and
Mobile Settings. App filtering is off so non-mobile-tagged installed apps do not
disappear. MIME defaults select Firefox, Files, Text Editor, Loupe, Calls and
Chatty. Adwaita/dark, Noto fonts and the existing kitty configuration are used;
stock GNOME backgrounds satisfy Phosh's packaged wallpaper defaults.

Idle blank/lock is two minutes, with password required and no lock grace. Phosh
owns power-key blank/lock and its long-press menu; GNOME Settings Daemon must not
also suspend on the key. Automatic suspend is disabled on AC and battery, and
the existing logind/systemd sleep prohibitions are retained. **Suspend/resume is
not claimed to work.** Mirrors, paru, package holds, btrfs/snapper, modem, USB,
firmware, audio and all hardware services are unchanged.

## Clean build and validation

This conversion must use a **fresh staging tree**: pacstrap is additive and does
not uninstall Plasma or delete obsolete overlays from a reused root. Stop any
build/chroot using it, check `findmnt` for mounts beneath it, then remove **only**
`~/q25-work/rootfs` before building. Keep the host pacman cache, kernel tree and
`mm-buildroot`. The generated image contains no old desktop packages or user
configuration. This is an image replacement, not an in-place migration script.

```sh
bash -n build/build-rootfs.sh build/build-all.sh
for test in build/test-*.py tests/test_*.py; do python3 "$test" -v; done
Q25_TEST_ROOT="$HOME/q25-work/rootfs" python3 build/test-phosh-config.py -v
sudo systemd-analyze --root="$HOME/q25-work/rootfs" --man=no verify phosh.service
sudo systemctl --root="$HOME/q25-work/rootfs" is-enabled phosh.service
sudo systemctl --root="$HOME/q25-work/rootfs" get-default
sudo build/test-btrfs-boot.sh "$HOME/q25-work/rootfs"
sudo build/test-btrfs-checkpoint.sh
```

The Phosh test strictly compiles a temporary copy of installed schemas (or
`Q25_SCHEMA_DIR`), queries effective desktop-specific values, verifies app/MIME
desktop IDs, UID 1000, PAM, the unit, target links, OSK and absence of Plasma.
Repository-only runs skip schema validation if target schemas are not available.
The configured-root run must pass without those skips. For a different account,
set `Q25_TEST_USER` to the build's `Q25_USER`.

Cross-architecture pacman hooks may need offline repair when qemu-user reports
`Function not implemented`: run `systemd-tmpfiles --create` inside `arch-chroot`,
and `systemd-hwdb --root=... update` with the host tool. These are staging repairs,
not device hooks. Regenerate the image after any such repair.

### Offline build result (2026-11-09)

The full `BUILDROOT=$HOME/q25-work/mm-buildroot ./build/build-all.sh` run succeeded
from a fresh staging directory. The image has **754 installed packages**, no
Plasma/KWin/SDDM/KDE apps, enabled phosh.service and graphical.target, and all
listed applications and Settings panel entries. `pacman -T` against the requested
package set and `systemd-analyze --root=... --man=no verify phosh.service` passed.

Outputs: `out/boot.img` **67,108,864 bytes**, `out/initramfs.cpio.gz` **8,388,608
bytes**, `out/q25-rootfs.img` **10,776,215,552 bytes**, and **49** userdata sparse
chunks. The image's btrfs read-only check passed. The log is
`~/q25-work/build-all.log`. Nonfatal messages were arch-chroot's non-mountpoint
warning, a package hook running before locale generation, and existing
archlinuxcn key import warnings; configure/keyring and every build stage finished.

The **21-test Python suite** passed with the staged root (20 passed, one
image-layout skip covered separately on read-only mounts). All **9 Phosh tests
and 4 btrfs layout tests** passed against the generated image. Native and target ARM BusyBox
boot-policy tests and the native Snapper checkpoint/rollback test passed.
Strict schema compilation passed both inside the build and in a host temporary
copy with effective `Phosh:GNOME` defaults queried.

A temporary PAM harness inside the ARM chroot accepted the default user's
password and rejected a wrong password using the actual `phosh` PAM stack.
It had to run as root: this host's qemu-aarch64 binfmt has `PF`, not `C`, flags,
so a UID-1000 emulated `unix_chkpwd` cannot gain its required setuid credentials
and reports `AUTHINFO_UNAVAIL`. The image's helper permissions and password are
correct; host binfmt was not changed. This does **not** substitute for checking
real non-root lock/unlock, logind and keyring behavior on the phone.

### Device acceptance (supervisor; no flashing in this change)

1. Cold boot to the PIN screen, verify no unlocked first frame; wrong/correct PIN,
   password changes, restart, keyring and sudo. `loginctl show-session` must show
   an active local seat0 user session on tty7, `Class=user`, `Type=wayland`.
2. Check 1.25× output, Panfrost rendering, software cursor, touch alignment,
   scale changes in Displays, all favorite/apps and every requested Settings panel.
3. Type with physical/keyd keyboard without an unwanted OSK; explicitly show/hide
   Stevia, including after switching apps and waking. Check CJK/emoji/kitty fonts.
4. Check short/long power press and idle blank/lock/wake, without system suspend.
5. Recheck Wi-Fi, APN/LTE data, SIM unlock, calls/SMS limitations, Bluetooth, audio,
   USB networking, notifications, sensor rotation and reboot persistence. Camera
   availability, telephony and haptics are hardware capabilities, not app-install
   guarantees. Keep the existing hardware acceptance gates in `docs/validation-hw.md`.
