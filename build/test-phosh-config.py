#!/usr/bin/env python3
"""Check Phosh defaults; Q25_TEST_ROOT additionally checks a configured image.

Q25_SCHEMA_DIR can point at extracted target package schemas for offline checks.
Otherwise schema validation uses Q25_TEST_ROOT or the host's installed schemas.
"""
import ast
import configparser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get("Q25_TEST_ROOT", REPO / "rootfs"))
STAGED = bool(os.environ.get("Q25_TEST_ROOT"))
SCHEMAS = Path(os.environ.get("Q25_SCHEMA_DIR", str(
    ROOT / "usr/share/glib-2.0/schemas" if STAGED else Path("/usr/share/glib-2.0/schemas"))))
OVERRIDE = "usr/share/glib-2.0/schemas/90-q25-phosh.gschema.override"
APPS = {
    "kitty": "kitty.desktop", "firefox": "firefox.desktop",
    "gnome-control-center": "org.gnome.Settings.desktop",
    "phosh-mobile-settings": "mobi.phosh.MobileSettings.desktop",
    "gnome-calls": "org.gnome.Calls.desktop", "chatty": "sm.puri.Chatty.desktop",
    "gnome-contacts": "org.gnome.Contacts.desktop", "gnome-clocks": "org.gnome.clocks.desktop",
    "gnome-weather": "org.gnome.Weather.desktop", "gnome-calculator": "org.gnome.Calculator.desktop",
    "gnome-text-editor": "org.gnome.TextEditor.desktop", "nautilus": "org.gnome.Nautilus.desktop",
    "loupe": "org.gnome.Loupe.desktop", "gnome-console": "org.gnome.Console.desktop",
    "gnome-maps": "org.gnome.Maps.desktop", "snapshot": "org.gnome.Snapshot.desktop",
}
REMOVED = {
    "sddm", "plasma-desktop", "plasma-workspace", "plasma-mobile", "plasma-settings",
    "plasma-nm", "plasma-pa", "bluedevil", "kwin", "powerdevil", "kscreen",
    "plasma-keyboard", "kscreenlocker", "polkit-kde-agent", "kde-gtk-config",
    "breeze-gtk", "xdg-desktop-portal-kde", "dolphin", "kwrite", "kate", "okular",
    "gwenview", "ark", "spectacle", "plasma-systemmonitor", "kcalc",
}


def config(path):
    result = configparser.ConfigParser(interpolation=None, strict=False)
    result.optionxform = str
    with (ROOT / path).open() as source:
        result.read_file(source)
    return result


def packages():
    return {line.strip() for line in (REPO / "build/packages.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")}


class PhoshDefaults(unittest.TestCase):
    def test_complete_package_set(self):
        required = set(APPS) | {
            "phosh", "phoc", "stevia", "xdg-desktop-portal-phosh", "xdg-desktop-portal-gtk",
            "gnome-settings-daemon", "gnome-keyring", "feedbackd", "iio-sensor-proxy",
            "adwaita-icon-theme", "adwaita-cursors", "gnome-backgrounds",
            "networkmanager", "modemmanager", "kitty", "noto-fonts", "noto-fonts-cjk",
            "noto-fonts-emoji", "ttf-jetbrains-mono-nerd",
        }
        self.assertFalse(required - packages())
        self.assertFalse(REMOVED & packages())
        self.assertNotIn("squeekboard", packages(), "Install only one OSK provider")

    def test_no_plasma_or_custom_session_overlay(self):
        overlay = REPO / "rootfs"
        for path in ("etc/sddm.conf.d", "usr/share/sddm", "usr/share/plasma",
                     "etc/xdg/kwinrc", "etc/xdg/powerdevilrc", "etc/xdg/plasmamobilerc",
                     "etc/skel/.config/kdeglobals", "etc/environment.d/60-q25-display.conf",
                     "usr/lib/systemd/system/phosh.service", "etc/phosh/home.ui"):
            self.assertFalse((overlay / path).exists(), path)
        for path in overlay.rglob("*"):
            if path.is_file() and not path.is_symlink() and "__pycache__" not in path.parts:
                self.assertNotIn("KWIN_", path.read_text(), str(path))

    def test_stock_login_and_display(self):
        service = config("etc/systemd/system/phosh.service")
        self.assertEqual(service["Service"]["ExecStart"],
                         'capsh --noamb -- -c "exec /usr/bin/phosh-session"')
        self.assertEqual(service["Service"]["TTYPath"], "/dev/tty7")
        self.assertEqual(service["Service"]["UtmpIdentifier"], "tty7")
        self.assertEqual(service["Service"]["StandardInput"], "tty-fail")
        self.assertEqual(service["Install"]["WantedBy"], "graphical.target")
        drop = config("etc/systemd/system/phosh.service.d/q25.conf")
        self.assertEqual(drop["Service"]["User"], os.environ.get("Q25_TEST_USER", "alarm"))
        self.assertEqual(drop["Service"]["PAMName"], "phosh")
        self.assertIn("getty@tty7.service", drop["Unit"]["Conflicts"])
        text = (ROOT / "etc/systemd/system/phosh.service").read_text()
        for value in ("XDG_SESSION_TYPE=wayland", "XDG_CURRENT_DESKTOP=Phosh:GNOME"):
            self.assertIn("Environment=" + value + "\n", text)
        text = (ROOT / "etc/systemd/system/phosh.service.d/q25.conf").read_text()
        for value in ("WLR_DRM_DEVICES=/dev/dri/q25-display", "WLR_NO_HARDWARE_CURSORS=1",
                      "XDG_SESSION_CLASS=user"):
            self.assertIn("Environment=" + value + "\n", text)
        self.assertEqual(config("etc/phosh/phoc.ini")["output:DSI-1"].getfloat("scale"), 1.0)
        self.assertIn('DRIVERS=="mediatek-drm", SYMLINK+="dri/q25-display"',
                      (ROOT / "etc/udev/rules.d/60-q25.rules").read_text())

    def test_password_pam_and_configure(self):
        pam = (ROOT / "etc/pam.d/phosh").read_text()
        for kind in ("auth", "account", "session"):
            self.assertRegex(pam, kind + r"\s+include\s+system-local-login")
        self.assertRegex(pam, r"auth\s+optional\s+pam_gnome_keyring.so")
        self.assertRegex(pam, r"session\s+optional\s+pam_gnome_keyring.so auto_start")
        build = (REPO / "build/build-rootfs.sh").read_text()
        self.assertIn('Q25_PASSWORD=${Q25_PASSWORD:-$Q25_USER}', build)
        self.assertIn('printf \'%s:%s\\n\' "$Q25_USER" "$Q25_PASSWORD" | in_root chpasswd', build)
        self.assertIn('useradd -m -u 1000', build)
        self.assertIn('"$ROOT/etc/systemd/system/phosh.service.d/q25.conf"', build)
        self.assertIn("phosh.service q25-boot-ok.service", build)
        self.assertIn("systemctl set-default graphical.target", build)
        self.assertIn("in_root glib-compile-schemas --strict /usr/share/glib-2.0/schemas", build)
        self.assertNotIn("sddm", build)
        self.assertIn("Q25_USER,Q25_PASSWORD", (REPO / "build/build-all.sh").read_text())

    def test_favorites_keyboard_and_lock_defaults(self):
        defaults = config(OVERRIDE)
        favorites = ast.literal_eval(defaults["sm.puri.phosh:Phosh"]["favorites"])
        self.assertEqual(favorites, [APPS[name] for name in (
            "kitty", "firefox", "gnome-control-center", "nautilus", "gnome-calls",
            "chatty", "gnome-contacts", "gnome-clocks", "phosh-mobile-settings")])
        self.assertEqual(defaults["sm.puri.phosh:Phosh"]["app-filter-mode"], "[]")
        self.assertFalse(defaults["mobi.phosh.osk:Phosh"].getboolean("ignore-hw-keyboards"))
        self.assertTrue(defaults["org.gnome.desktop.a11y.applications:Phosh"].getboolean("screen-keyboard-enabled"))
        self.assertTrue(defaults["sm.puri.phosh.lockscreen:Phosh"].getboolean("require-unlock"))
        self.assertEqual(defaults["org.gnome.desktop.session:Phosh"].getint("idle-delay"), 120)
        self.assertTrue(defaults["org.gnome.desktop.screensaver:Phosh"].getboolean("lock-enabled"))
        self.assertEqual(defaults["org.gnome.desktop.screensaver:Phosh"].getint("lock-delay"), 0)
        power = defaults["org.gnome.settings-daemon.plugins.power:Phosh"]
        for supply in ("ac", "battery"):
            self.assertEqual(power[f"sleep-inactive-{supply}-type"], "'nothing'")
            self.assertEqual(power.getint(f"sleep-inactive-{supply}-timeout"), 0)
        self.assertFalse(defaults["sm.puri.phosh:Phosh"].getboolean("enable-suspend"))
        self.assertEqual(config("etc/systemd/logind.conf.d/q25.conf")["Login"]["HandlePowerKey"], "ignore")
        self.assertFalse(config("etc/systemd/sleep.conf.d/q25.conf")["Sleep"].getboolean("AllowSuspend"))

    def test_fonts_and_mime_defaults(self):
        fontconfig = ET.parse(ROOT / "etc/fonts/conf.d/64-q25-defaults.conf")
        families = {element.text for element in fontconfig.findall(".//family")}
        self.assertTrue({"Noto Sans", "Noto Sans CJK SC", "Noto Color Emoji", "JetBrainsMono Nerd Font Mono"} <= families)
        kitty = (ROOT / "etc/xdg/kitty/kitty.conf").read_text()
        self.assertIn("font_family JetBrainsMono Nerd Font Mono\n", kitty)
        self.assertIn("font_size 10.0\n", kitty)
        mime = config("etc/xdg/mimeapps.list")["Default Applications"]
        self.assertEqual(mime["x-scheme-handler/http"], APPS["firefox"])
        self.assertEqual(mime["inode/directory"], APPS["nautilus"])
        self.assertEqual(mime["x-scheme-handler/tel"], APPS["gnome-calls"])
        self.assertEqual(mime["x-scheme-handler/sms"], APPS["chatty"])
        if STAGED:
            for desktop in mime.values():
                self.assertTrue((ROOT / "usr/share/applications" / desktop).is_file(), desktop)

    def test_compile_schemas_strict_and_effective_defaults(self):
        if not (SCHEMAS / "mobi.phosh.osk.gschema.xml").exists():
            if STAGED:
                self.fail("Stevia schemas missing from staging tree")
            self.skipTest("Install target schemas or set Q25_SCHEMA_DIR for strict validation")
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            for path in SCHEMAS.iterdir():
                if path.suffix in (".xml", ".override"):
                    shutil.copy2(path, directory)
            shutil.copy2(ROOT / OVERRIDE, directory)
            subprocess.run(["glib-compile-schemas", "--strict", str(directory)], check=True)
            env = dict(os.environ, GSETTINGS_SCHEMA_DIR=str(directory), GSETTINGS_BACKEND="memory",
                       XDG_CURRENT_DESKTOP="Phosh:GNOME")
            expected = {
                ("mobi.phosh.osk", "ignore-hw-keyboards"): "false",
                ("org.gnome.desktop.session", "idle-delay"): "uint32 120",
                ("org.gnome.desktop.screensaver", "lock-enabled"): "true",
                ("sm.puri.phosh.lockscreen", "require-unlock"): "true",
                ("org.gnome.settings-daemon.plugins.power", "sleep-inactive-battery-type"): "'nothing'",
            }
            for (schema, key), value in expected.items():
                actual = subprocess.check_output(["gsettings", "get", schema, key], env=env, text=True).strip()
                self.assertEqual(actual, value)
            favorites = subprocess.check_output(["gsettings", "get", "sm.puri.phosh", "favorites"], env=env, text=True)
            self.assertEqual(ast.literal_eval(favorites), ast.literal_eval(config(OVERRIDE)["sm.puri.phosh:Phosh"]["favorites"]))

    @unittest.skipUnless(STAGED, "configured staging tree not requested")
    def test_settings_panels_and_phone_integration(self):
        for panel in ("wifi", "wwan", "bluetooth", "sound", "display", "power", "users"):
            entry = config(f"usr/share/applications/gnome-{panel}-panel.desktop")["Desktop Entry"]
            self.assertIn("gnome-control-center", entry["Exec"])
        self.assertTrue((ROOT / "usr/bin/nmtui").is_file())
        for desktop in ("org.gnome.Calls-daemon.desktop", "sm.puri.Chatty-daemon.desktop"):
            entry = config("etc/xdg/autostart/" + desktop)["Desktop Entry"]
            self.assertIn("--gapplication-service", entry["Exec"])
            self.assertIn("Phosh", entry.get("OnlyShowIn", "Phosh"))
        portal = config("usr/share/xdg-desktop-portal/phosh-portals.conf")["preferred"]
        self.assertIn("phosh;", portal["default"])
        self.assertIn("gtk;", portal["default"])
        self.assertEqual(portal["org.freedesktop.impl.portal.Secret"], "gnome-keyring;")
        for wallpaper in ("blobs-l.svg", "blobs-d.svg"):
            self.assertTrue((ROOT / "usr/share/backgrounds/gnome" / wallpaper).is_file())
        wlroots = (ROOT / "usr/lib/libwlroots-0.20.so").read_bytes()
        for variable in (b"WLR_DRM_DEVICES", b"WLR_NO_HARDWARE_CURSORS"):
            self.assertIn(variable, wlroots)

    @unittest.skipUnless(STAGED, "configured staging tree not requested")
    def test_configured_session_apps_and_services(self):
        system = ROOT / "etc/systemd/system"
        self.assertEqual((system / "graphical.target.wants/phosh.service").readlink().name, "phosh.service")
        self.assertEqual((system / "default.target").readlink().name, "graphical.target")
        self.assertFalse((system / "display-manager.service").is_symlink())
        user = os.environ.get("Q25_TEST_USER", "alarm")
        self.assertIn(user + ":x:1000:", (ROOT / "etc/passwd").read_text())
        self.assertIn("phosh-session", config("usr/share/wayland-sessions/phosh.desktop")["Desktop Entry"]["Exec"])
        self.assertTrue((ROOT / "usr/bin/phosh-session").is_file())
        session = (ROOT / "usr/lib/systemd/user/mobi.phosh.Shell.service").read_text()
        self.assertIn("ExecStart=/usr/lib/phosh/phosh\n", session)  # No --unlocked/-U
        osk = (ROOT / "usr/lib/systemd/user/mobi.phosh.OSK.service").read_text()
        self.assertIn("/usr/bin/phosh-osk-stevia", osk)
        self.assertTrue((ROOT / "usr/share/glib-2.0/schemas/gschemas.compiled").is_file())
        for desktop in APPS.values():
            entry = config("usr/share/applications/" + desktop)["Desktop Entry"]
            self.assertFalse(entry.getboolean("Hidden", False), desktop)
            self.assertFalse(entry.getboolean("NoDisplay", False), desktop)
            self.assertIn("Exec", entry, desktop)
        installed = subprocess.check_output(["pacman", "--root", str(ROOT), "-Qq"], text=True).splitlines()
        self.assertFalse(REMOVED & set(installed))
        self.assertFalse([name for name in installed if name.startswith(("plasma-", "kwin", "sddm"))])
        self.assertTrue(set(APPS) <= set(installed))
        auth = (ROOT / "etc/pam.d/system-auth").read_text()
        self.assertIn("pam_unix.so", auth)
        self.assertIn("pam_faillock.so", auth)


if __name__ == "__main__":
    unittest.main()
