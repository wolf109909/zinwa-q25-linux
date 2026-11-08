#!/usr/bin/env python3
"""Regression checks for image package repositories and security defaults."""
from pathlib import Path
import unittest

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / "rootfs"


class PackageDefaults(unittest.TestCase):
    def test_arm_mirrors(self):
        servers = [line for line in (ROOT / "etc/pacman.d/mirrorlist").read_text().splitlines()
                   if line.startswith("Server")]
        self.assertEqual(servers[0], "Server = https://mirrors.cernet.edu.cn/archlinuxarm/$arch/$repo")
        self.assertEqual(len(servers), 3)
        build = (REPO / "build/pacman-build.conf").read_text()
        for repo in ("core", "extra", "alarm"):
            section = build.split(f"[{repo}]\n", 1)[1].split("[", 1)[0]
            self.assertEqual(section.strip().splitlines(), servers)
        configure = (REPO / "build/build-rootfs.sh").read_text()
        self.assertIn('"$REPO/rootfs/etc/pacman.d/mirrorlist" "$ROOT/etc/pacman.d/mirrorlist"', configure)

    def test_archlinuxcn_and_final_signature_policy(self):
        target = (ROOT / "etc/pacman.conf").read_text()
        for path in (ROOT / "etc/pacman.conf", REPO / "build/pacman-build.conf"):
            section = path.read_text().split("[archlinuxcn]\n", 1)[1].split("[", 1)[0]
            self.assertEqual(section.strip(), "Server = https://mirrors.cernet.edu.cn/archlinuxcn/$arch")
        self.assertIn("SigLevel    = Required DatabaseOptional", target)
        self.assertNotIn("SigLevel = Never", target)
        self.assertIn("IgnorePkg = linux-aarch64 linux-aarch64-headers linux-firmware linux-firmware-*", target)
        self.assertIn("IgnorePkg = modemmanager libmm-glib", target)
        configure = (REPO / "build/build-rootfs.sh").read_text()
        self.assertIn('trap \'rm -f "$conf"\' EXIT', configure)
        self.assertIn("--noconfirm --needed -Sy archlinuxcn-keyring", configure)
        self.assertIn("in_root pacman-key --populate archlinuxcn", configure)


if __name__ == "__main__":
    unittest.main()
