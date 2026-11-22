# Desktop choice for the Zinwa Q25

*Researched 2026-11-09; documentation/source inspection only. No phone access or runtime changes. Public pages were retrieved over HTTP because dedicated web-search tools were unavailable; search-engine challenges limited broader anecdotal coverage.*

## Historical recommendation (superseded by the implementation decision)

**Current image: Phosh only, 1×, full apps and upstream phosh.service autologin
to the password-protected lock screen.** See [../phosh.md](../phosh.md).
The ranked alternatives, 1.5× starting suggestion and greetd trial proposal below
are research history, not current build instructions or additional desktops.

**Choose Phosh/phoc for the integrated phone; keep Sxmo/sway as the keyboard-first alternative.** Stop treating Plasma scaling as a sufficient fix. The Q25's limiting resource is usable screen geometry, not its 12 GB RAM.

| Output scale | Logical screen | Recommended use |
|---|---|---|
| 1.0 | 720×720 | Terminal-heavy work; small touch targets |
| 1.25 | 576×576 | Keyboard-first compromise |
| 1.5 | 480×480 | Phosh starting point: triggers narrow settings layouts |

Shell bars subtract further height. Rotating a square adds no space; an OSK consumes scarce height unnecessarily. [Mesa documents Mali-G57 OpenGL ES 3.1 support](https://docs.mesa3d.org/drivers/panfrost.html): these Wayland stacks are plausible EGL/GLES clients, not reasons to require Vulkan. This is **not Q25-specific compositor validation**.

## Actual size constraints

- **360×294 is guidance, not a universal minimum or breakpoint.** The [GNOME HIG](https://developer.gnome.org/hig/guidelines/adaptive.html) recommends that phone-appropriate apps fit that size; desktop guidance remains 1024×600. Current [AdwWindow](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.Window.html) defaults to a 360×200 minimum with breakpoints; applications can impose larger requirements.
- [GNOME Settings source](https://github.com/GNOME/gnome-control-center/blob/main/shell/cc-window.blp) specifies **980×640 default**, not minimum, dimensions and collapses its sidebar at **550sp**. [Phosh Mobile Settings 0.57](https://github.com/PhoshMobi/phosh-mobile-settings/blob/v0.57.0/src/ui/ms-window.ui) explicitly requests **360×200**, collapsing at **500sp**. At default text scaling, 480-wide activates both narrow layouts; 576-wide does not. Thus increasing scale can improve navigation despite reducing space. Individual panels/dialogs still need checking.
- **Kirigami is not a fit guarantee.** [PageRow](https://github.com/KDE/kirigami/blob/master/src/controls/PageRow.qml) defaults to 20 grid units per column and permits page overrides. [Plasma Settings](https://invent.kde.org/plasma-mobile/plasma-settings/-/blob/master/src/qml/Main.qml) switches to its wide sidebar at **720 logical pixels**: native 1× can actually select the less suitable layout. Desktop KCMs can retain larger minima or non-scrolling content.

## Ranked options

### 1. Phosh + phoc + Phosh Mobile Settings — best overall

Official ALARM aarch64 packages exist for [Phosh](https://archlinuxarm.org/packages/aarch64/phosh), [phoc](https://archlinuxarm.org/packages/aarch64/phoc), and [Mobile Settings](https://archlinuxarm.org/packages/aarch64/phosh-mobile-settings); the checked Phosh/settings versions are 0.57.0. Mature phone integration includes NetworkManager/ModemManager, SIM unlocking, password/PIN lock, sensor rotation and [power-key blanking](https://github.com/PhoshMobi/phosh/blob/main/src/screen-saver-manager.c). Calls/Chatty are separate applications, not modem-driver fixes.

[Mobile Settings now includes Wi-Fi/Bluetooth configuration](https://phosh.mobi/gettingstarted/), alongside mobile-specific controls; do not assume it is merely a cosmetic tweaks app. Use adaptive apps, Tab/Shift-Tab and app switching; it remains less keyboard-centric than sway. Per-app **Scale-to-Fit** is an escape hatch for oversized windows, not reflow: it can make text/touch targets tiny. Prefer it over shrinking everything.

### 2. Sxmo on sway — best keyboard-oriented workspace

[Sway is packaged](https://archlinuxarm.org/packages/aarch64/sway); Sxmo has [AUR recipes](https://aur.archlinux.org/packages/sxmo-utils-sway-git), not a verified official ALARM package. Expect dependency/service and Q25 device-profile work, not a turnkey install. Start at 1.25, use tabbed/fullscreen windows, zero gaps, a small/hidden bar and sticky/prefix bindings rather than difficult simultaneous chords.

The [Sxmo manual](https://sxmo.org/docs/user/sxmo.7.html) documents NM/MM-backed APN/call/SMS menus, optional manually toggled OSK, blanking and configurable power states. [Autorotation uses iio-sensor-proxy](https://git.sr.ht/~mil/sxmo-utils/blob/master/scripts/core/sxmo_autorotate.sh). Menus are keyboard-accessible; arbitrary GUI apps retain their size problems. **Its pocket-lock state is not automatically authentication:** the documented Wayland locker is optional. Explicitly configure a secure locker; prefer PAM/password-capable swaylock for this keyboard phone over relying on the default gesture sequence.

### 3. Plasma Mobile — retain only if its specific apps matter

[Packaged on ALARM](https://archlinuxarm.org/packages/aarch64/plasma-mobile), with integrated cellular, lockscreen, rotation and power management. Try 1.25 using `kscreen-doctor output.<id>.scale.1.25` after inspecting `kscreen-doctor -o`; use mobile **plasma-settings**, not desktop System Settings. Enable [Auto Hide Panels and Tiny status bar](https://invent.kde.org/plasma/plasma-mobile/-/blob/master/kcms/mobileshell/ui/main.qml), disable automatic OSK, and try [Docked Mode](https://github.com/KDE/plasma-mobile/blob/master/quicksettings/docked/contents/ui/main.qml) for movable/decorated windows. Convergence changes window management, **not application minimum sizes**. Do not promise these knobs cure the reported unreachable pages.

### 4–6. labwc, niri, Hyprland — capable compositors, unfinished phone environments

All have ALARM packages: [labwc](https://archlinuxarm.org/packages/aarch64/labwc), [niri](https://archlinuxarm.org/packages/aarch64/niri), [Hyprland](https://archlinuxarm.org/packages/aarch64/hyprland). Start at 1.0–1.25 with maximized/full-width windows.

**4: labwc** is the conservative floating-window choice, but requires more pointer/window manipulation; [touch is supported, shell gestures are not interpreted](https://labwc.github.io/labwc-config.5.html). **5: niri** offers excellent keyboard-driven scrolling columns; [touchscreens work but touchscreen gestures are not advertised as implemented](https://github.com/niri-wm/niri). **6: Hyprland** adds attractive tiling/effects but more configuration churn without solving phone settings.

Budget for a launcher, notifications, polkit agent, NM/MM frontends, optional OSK, authenticated locker, idle daemon, power-key binding and sensor-to-output/touch rotation glue. Native touch support is not a complete mobile shell. Niri's [default power key suspends](https://github.com/niri-wm/niri/blob/v26.04/docs/wiki/Configuration%3A-Input.md); explicitly change this for blank-only behavior.

### 7. GNOME Shell mobile branch — experimental, not the pragmatic switch

[Stock GNOME Shell is packaged](https://archlinuxarm.org/packages/aarch64/gnome-shell); that does **not** supply the mobile shell/Mutter patchsets. The [mobile port documentation](https://wiki.postmarketos.org/wiki/GNOME) labels them experimental. GNOME offers keyboard navigation, NM/MM and sensors, but branch synchronization, phone mode thresholds, lock/OSK behavior and fractional scaling need validation. Shared libadwaita apps do not make its shell better than Phosh at 480×480.

### 8. Lomiri — strong convergence, wrong integration cost here

[Debian packages](https://packages.debian.org/trixie/lomiri) demonstrate ARM feasibility, not easy ALARM packaging: no official ALARM shell package was verified. The Mir/QtMir Wayland stack has substantial dependencies. Ubuntu Touch's lock/OSK/rotation/power integration cannot simply be assumed on ALARM. Its [network indicator depends on oFono bindings](https://packages.debian.org/trixie/lomiri-indicator-network), so Ubuntu Touch cellular UI is not a drop-in ModemManager frontend. Touch/PIN/convergence strengths do not justify replacing the working modem stack for an unverified square-screen result.

## What keyboard-device users actually establish

- [Key2's port recommends Sxmo because of GPU failures](https://wiki.postmarketos.org/wiki/BlackBerry_KEY2_Generic_(blackberry-key2-generic)), not proof that Phosh cannot suit keyboards. [Titan Pocket](https://wiki.postmarketos.org/wiki/Unihertz_Titan_Pocket_(unihertz-titan-pocket)) matches 720×720 but needs special-character/sticky-key mapping; much phone hardware remains untested.
- A [Pro1 owner](https://community.fxtec.com/topic/3654-droidian-released-for-pro1-and-pro1-x/?do=findComment&comment=63007) reported disliking Phosh ergonomics and experimenting with sway; this is a **2022 anecdote**, not a current benchmark. [uConsole sway configurations](https://github.com/emdash/uConsole_sway_config) use sticky modifiers and remove titlebars/gaps—useful transferable patterns, unlike its wider screen.
- [Q20-keyboard Hackberry Pi](https://github.com/ZitaoTech/Hackberry-Pi_Zero) is a genuine square-screen comparator, **not native Linux on a stock Q20**. [Gemini users document sticky keys and conflicting lock/power handlers](https://github.com/gemian/gemini-keyboard-apps/wiki/DebianTP2). [Astro Slide has a Droidian adaptation repository](https://github.com/planet-community/adaptation-droidian-astroslide); neither establishes square-screen suitability or a winning shell.

## Concrete starting configuration and acceptance gate

For a future authorized trial, substitute the real connector name in `/etc/phosh/phoc.ini`:

```ini
[output:DSI-1]
scale = 1.5
```

Disable automatic OSK: `gsettings set org.gnome.desktop.a11y.applications screen-keyboard-enabled false`. Keep rotation locked while typing; unlock it when testing iio-sensor-proxy. Ensure the keyboard map exposes modifiers, Escape, Tab and navigation. Use Mobile Settings' per-app Scale-to-Fit; try 1.25 later for terminal density.

Use authenticated [greetd/agreety](https://archlinuxarm.org/packages/aarch64/greetd-agreety) launching `phosh-session`, avoiding SDDM's independently scaled/theme-sized greeter. **Do not substitute unguarded autologin.** Phosh's PIN is normally the account password; a short numeric password weakens account authentication.

Before replacing Plasma, verify every required settings page and dialog, keyboard symbols/navigation, login/unlock, SIM/APN, rotation/touch alignment and power-key blank/wake. Keep suspend separate until modem wake is proven. No research-only report can guarantee those device-level outcomes.
