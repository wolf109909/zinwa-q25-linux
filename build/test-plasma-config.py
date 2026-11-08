#!/usr/bin/env python3
"""Check Q25 Plasma defaults; Q25_TEST_ROOT also checks a configured staging tree."""
import configparser
import json
import os
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get("Q25_TEST_ROOT", REPO / "rootfs"))


def config(path):
    result = configparser.ConfigParser(interpolation=None)
    result.optionxform = str
    with (ROOT / path).open() as source:
        result.read_file(source)
    return result


class PlasmaDefaults(unittest.TestCase):
    def test_packages(self):
        packages = {
            line.strip() for line in (REPO / "build/packages.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        }
        self.assertTrue({
            "sddm", "plasma-desktop", "plasma-mobile", "plasma-settings", "kwin",
            "plasma-keyboard", "plasma-nm", "plasma-pa", "bluedevil", "powerdevil",
            "kitty", "firefox", "dolphin", "kwrite", "okular", "gwenview", "ark",
            "spectacle", "plasma-systemmonitor", "networkmanager", "modemmanager",
            "noto-fonts", "noto-fonts-cjk", "noto-fonts-emoji", "ttf-jetbrains-mono-nerd",
        } <= packages)

    def test_display_and_scale(self):
        env = (ROOT / "etc/environment.d/60-q25-display.conf").read_text()
        self.assertIn("KWIN_DRM_DEVICES=/dev/dri/q25-display\n", env)
        self.assertIn("KWIN_FORCE_SW_CURSOR=1\n", env)
        rules = (ROOT / "etc/udev/rules.d/60-q25.rules").read_text()
        self.assertIn('DRIVERS=="mediatek-drm", SYMLINK+="dri/q25-display"', rules)
        output = json.loads((ROOT / "etc/xdg/kwinoutputconfig.json").read_text())
        self.assertEqual(output[0]["name"], "outputs")
        self.assertEqual(output[0]["data"], [{
            "connectorName": "DSI-1", "scale": 1.25,
            "transform": "Normal", "autoRotation": "Never",
        }])
        self.assertEqual(output[1], {"name": "setups", "data": []})
        self.assertEqual(config("etc/xdg/kcmfonts")["General"]["forceFontDPI"], "0")

    def test_sddm_autologin_and_greeter(self):
        sddm = config("etc/sddm.conf.d/20-q25.conf")
        sddm.read(ROOT / "etc/sddm.conf.d/30-q25-mobile.conf")
        self.assertEqual(sddm["Autologin"]["User"], os.environ.get("Q25_TEST_USER", "alarm"))
        self.assertEqual(sddm["Autologin"]["Session"], "plasma-mobile.desktop")
        self.assertFalse(sddm["Autologin"].getboolean("Relogin"))
        self.assertEqual(sddm["General"]["DisplayServer"], "wayland")
        self.assertIn("--no-lockscreen", sddm["Wayland"]["CompositorCommand"])
        self.assertEqual(sddm["General"]["GreeterEnvironment"],
                         "KWIN_DRM_DEVICES=/dev/dri/q25-display,KWIN_FORCE_SW_CURSOR=1,QT_SCALE_FACTOR=0.8")
        theme = config("usr/share/sddm/themes/breeze/theme.conf.user")["General"]
        self.assertFalse(theme.getboolean("showClock"))
        self.assertEqual(theme["type"], "color")

    def test_mobile_gesture_navigation(self):
        mobile = config("etc/xdg/plasmamobilerc")["General"]
        self.assertFalse(mobile.getboolean("navigationPanelEnabled"))
        self.assertTrue(mobile.getboolean("gesturePanelEnabled"))

    def test_touch_keyboard_and_normal_window_rule(self):
        kwin = config("etc/xdg/kwinrc")
        self.assertEqual(kwin["Input"]["TabletMode"], "on")
        self.assertEqual(kwin["Wayland"]["VirtualKeyboardMode"], "1")
        self.assertEqual(kwin["Wayland"]["InputMethod"], "/usr/share/applications/org.kde.plasma.keyboard.desktop")
        self.assertTrue(kwin["Windows"].getboolean("BorderlessMaximizedWindows"))
        rules = config("etc/xdg/kwinrulesrc")
        rule = rules[rules["General"]["Order"]]
        self.assertEqual(rule["types"], "1")
        for axis in ("horiz", "vert"):
            self.assertTrue(rule.getboolean("maximize" + axis))
            self.assertEqual(rule["maximize" + axis + "rule"], "3")

    def test_lock_and_power_without_suspend(self):
        lock = config("etc/xdg/kscreenlockerrc")["Daemon"]
        for key in ("Autolock", "LockOnStart", "RequirePassword"):
            self.assertTrue(lock.getboolean(key))
        self.assertEqual(lock["LockGrace"], "0")
        power = config("etc/xdg/powerdevilrc")
        for profile in ("AC", "Battery", "LowBattery"):
            self.assertEqual(power[profile + "][SuspendAndShutdown"]["AutoSuspendAction"], "0")
            self.assertEqual(power[profile + "][SuspendAndShutdown"]["PowerButtonAction"], "64")
            self.assertTrue(power[profile + "][Display"].getboolean("LockBeforeTurnOffDisplay"))
        self.assertEqual(config("etc/systemd/logind.conf.d/q25.conf")["Login"]["HandlePowerKey"], "ignore")
        self.assertFalse(config("etc/systemd/sleep.conf.d/q25.conf")["Sleep"].getboolean("AllowSuspend"))

    def test_fonts_and_standard_apps(self):
        kde = config("etc/xdg/kdeglobals")
        self.assertEqual(kde["General"]["TerminalApplication"], "kitty")
        self.assertEqual(kde["General"]["TerminalService"], "kitty.desktop")
        self.assertEqual(config("etc/skel/.config/kdeglobals")["General"]["BrowserApplication"], "firefox.desktop")
        fontconfig = ET.parse(ROOT / "etc/fonts/conf.d/64-q25-defaults.conf")
        families = {element.text for element in fontconfig.findall(".//family")}
        self.assertTrue({"Noto Sans", "Noto Sans CJK SC", "Noto Color Emoji", "JetBrainsMono Nerd Font Mono"} <= families)
        kitty = (ROOT / "etc/xdg/kitty/kitty.conf").read_text()
        self.assertIn("font_family JetBrainsMono Nerd Font Mono\n", kitty)
        self.assertIn("font_size 10.0\n", kitty)

    def test_desktop_layout_package(self):
        theme = config("etc/xdg/kdeglobals")["KDE"]["LookAndFeelPackage"]
        base = ROOT / "usr/share/plasma/look-and-feel" / theme
        self.assertEqual(json.loads((base / "metadata.json").read_text())["KPlugin"]["Id"], theme)
        layout = (base / "contents/layouts/org.kde.plasma.desktop-layout.js").read_text()
        self.assertIn("panel.height = 48;", layout)
        for widget in ("kickoff", "icontasks", "systemtray", "digitalclock"):
            self.assertIn('panel.addWidget("org.kde.plasma.' + widget + '")', layout)

    @unittest.skipUnless(os.environ.get("Q25_TEST_ROOT"), "configured staging tree not requested")
    def test_configured_sessions_and_services(self):
        for session, command in (("plasma", "startplasma-wayland"), ("plasma-mobile", "startplasmamobile")):
            entry = config("usr/share/wayland-sessions/" + session + ".desktop")["Desktop Entry"]
            self.assertIn(command, entry["Exec"])
            self.assertTrue((ROOT / "usr/bin" / command).is_file())
        system = ROOT / "etc/systemd/system"
        self.assertEqual((system / "display-manager.service").readlink().name, "sddm.service")
        self.assertEqual((system / "default.target").readlink().name, "graphical.target")
        user = os.environ.get("Q25_TEST_USER", "alarm")
        self.assertIn(user + ":x:1000:", (ROOT / "etc/passwd").read_text())
        self.assertEqual(config("home/" + user + "/.config/kdeglobals")["General"]["BrowserApplication"], "firefox.desktop")


if __name__ == "__main__":
    unittest.main()
