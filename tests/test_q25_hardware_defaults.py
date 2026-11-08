#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check hardware-boot configuration without installing it on the host.

Requires udevadm (with verify) and visudo. Runtime DHCP/MD1 acceptance remains
separate: these tests check the repository defaults and their syntax only.
"""
import configparser
from pathlib import Path
import subprocess
import unittest


ROOTFS = Path(__file__).resolve().parents[1] / "rootfs"


class Q25HardwareDefaults(unittest.TestCase):
    def test_mdinit_does_not_run_host_sleep_in_android_chroot(self):
        units = ROOTFS / "usr/lib/systemd/system"
        # systemd permits repeated directives (e.g. BindPaths/Environment).
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read([
            units / "q25-ccci@.service",
            units / "q25-ccci@ccci_mdinit.service.d/order.conf",
        ])
        self.assertEqual(config["Service"]["RootDirectory"], "/var/lib/q25/android")
        self.assertIn("q25-ccci@ccci_rpcd.service", config["Unit"]["After"].split())
        self.assertIn("q25-ccci@ccci_rpcd.service", config["Unit"]["Requires"].split())
        prestart = config["Service"].get("ExecStartPre", "")
        self.assertNotIn("/usr/bin/sleep", prestart,
                         "Host sleep cannot execute inside the Android root")

    def test_usb_gadget_override_has_valid_syntax(self):
        rule = ROOTFS / "etc/udev/rules.d/86-q25-usb-gadget.rules"
        subprocess.run(["udevadm", "verify", str(rule)], check=True, capture_output=True)
        self.assertIn('ENV{NM_UNMANAGED}="0"', rule.read_text())
        self.assertIn('KERNEL=="usb0"', rule.read_text())
        self.assertIn('ENV{DEVTYPE}=="gadget"', rule.read_text())

    def test_wheel_sudo_requires_password(self):
        sudoers = ROOTFS / "etc/sudoers.d/10-wheel"
        subprocess.run(["visudo", "-cf", str(sudoers)], check=True, capture_output=True)
        self.assertEqual(sudoers.read_text().strip(), "%wheel ALL=(ALL:ALL) ALL")


if __name__ == "__main__":
    unittest.main()
