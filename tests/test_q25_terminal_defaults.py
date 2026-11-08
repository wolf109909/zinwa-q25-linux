#!/usr/bin/env python3
"""Offline regression checks for the touch-capable Kitty/Fish defaults."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TerminalDefaults(unittest.TestCase):
    def test_fish_and_no_foot(self):
        pkgs = {s.strip() for s in (ROOT / 'build/packages.txt').read_text().splitlines()
                if s.strip() and not s.lstrip().startswith('#')}
        self.assertIn('fish', pkgs)
        self.assertNotIn('foot', pkgs)

    def test_kitty_uses_fish_on_wayland(self):
        conf = (ROOT / 'rootfs/etc/xdg/kitty/kitty.conf').read_text().splitlines()
        self.assertIn('shell /usr/bin/fish', conf)
        self.assertIn('linux_display_server wayland', conf)

    def test_standard_terminal_desktop_id(self):
        conf = (ROOT / 'rootfs/etc/xdg/xdg-terminals.list').read_text().splitlines()
        self.assertEqual(conf[0], 'kitty.desktop')
        overrides = (ROOT / 'rootfs/usr/share/glib-2.0/schemas/90-q25-phosh.gschema.override').read_text()
        self.assertIn("[org.gnome.desktop.default-applications.terminal:Phosh]\nexec='kitty'", overrides)

    def test_package_is_pinned_and_replaces_split_packages(self):
        pkg = ROOT / 'packages/kitty-upstream-bin/PKGBUILD'
        subprocess.run(['bash', '-n', str(pkg)], check=True)
        data = pkg.read_text()
        self.assertIn('pkgver=0.50.0', data)
        self.assertIn('3eb3257081ece4650dd112b2cf1bdbc0d1952d1a3f3934f22949e666e18451ee', data)
        self.assertIn("conflicts=('kitty' 'kitty-shell-integration' 'kitty-terminfo')", data)
        self.assertIn('ln -s /opt/kitty/bin/kitty', data)
        self.assertNotIn('SKIP', data)


if __name__ == '__main__':
    unittest.main()
