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

Checked versions: phosh/phoc **0.59.0-1**, phosh-mobile-settings **0.59.0-1**,
Stevia **0.58.0-1**, xdg-desktop-portal-phosh **0.58.0-1**. ARM packages provide:

| Purpose | Packages / visible applications |
|---|---|
| Phone session | phosh, phoc, phosh-mobile-settings, stevia |
| Settings and integration | gnome-control-center, gnome-settings-daemon, gnome-keyring, feedbackd, iio-sensor-proxy, xdg-desktop-portal-phosh, xdg-desktop-portal-gtk |
| Calls, messaging, address book | gnome-calls, chatty, gnome-contacts |
| Everyday utilities | gnome-clocks, gnome-weather, gnome-calculator, gnome-text-editor |
| Files and pictures | nautilus, loupe; Evince is also a Phosh dependency |
| Terminal and browser | **kitty 0.50+** (Fish), gnome-console, **Firefox** |
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
screen until a supported capture device exists. Chatty handles SMS; Calls has
no voice support yet (see [telephony.md](telephony.md)).

Btrfs Assistant is included as a storage tool.

## Login, password and keyring

Arch's phosh package ships `phosh-session`, the GNOME-session user units and
`/etc/pam.d/phosh`, but **does not ship a system phosh.service**. The image installs
the unmodified [upstream v0.59.0 system unit](https://gitlab.gnome.org/World/Phosh/phosh/-/blob/v0.59.0/data/phosh.service)
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

Stock `phosh-session` reads `/etc/phosh/phoc.ini`. Its `DSI-1` default is **1×**
(720×720 logical; 1.25× looked oversized under Phosh). Use **Settings > Displays** to change
scale; saved per-user display settings can supersede the seed. Phosh's usual
maximizing compositor behavior is retained. The physical keyd layout is unchanged.

Use **Settings** (gnome-control-center) for **Wi-Fi, Mobile Network, Bluetooth,
Sound, Displays, Power and Users**; networking uses NetworkManager/ModemManager,
not scripts. **nmtui** is supplied by NetworkManager. **Mobile Settings** provides
Phosh-specific preferences, OSK controls and per-app Scale-to-Fit.

Libadwaita apps generally adapt, but no app has been visually validated on this
image's 720×720 viewport. GNOME Settings (550sp) and Mobile Settings (500sp)
keep their sidebars visible at this width.
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
Chatty. Kitty uses Fish and the pinned official ARM64 `kitty-upstream-bin` package
for working Wayland touch input (see `packages/kitty-upstream-bin/README.md`).
Foot is not part of the image. Adwaita/dark, Noto fonts and the existing kitty configuration are used;
stock GNOME backgrounds satisfy Phosh's packaged wallpaper defaults.

Idle blank/lock is two minutes, with password required and no lock grace. Phosh
owns power-key blank/lock and its long-press menu; GNOME Settings Daemon must not
also suspend on the key. Automatic suspend is disabled on AC and battery, and
the existing logind/systemd sleep prohibitions are retained. **Suspend is not
supported yet.** Mirrors, paru, package holds, btrfs/snapper, modem, USB,
firmware, audio and all hardware services are unchanged.

## Clean build and validation

Build into a **fresh staging tree**: pacstrap is additive, so a reused root keeps
packages and overlay files that were since removed. Make sure nothing is mounted
beneath `~/q25-work/rootfs` (`findmnt`), remove it, then build. The host pacman
cache and kernel tree can stay.

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
desktop IDs, UID 1000, PAM, the unit, target links and the OSK.
Repository-only runs skip schema validation if target schemas are not available.
The configured-root run must pass without those skips. For a different account,
set `Q25_TEST_USER` to the build's `Q25_USER`.

Cross-architecture pacman hooks may need offline repair when qemu-user reports
`Function not implemented`: run `systemd-tmpfiles --create` inside `arch-chroot`,
and `systemd-hwdb --root=... update` with the host tool. These are staging repairs,
not device hooks. Regenerate the image after any such repair.
