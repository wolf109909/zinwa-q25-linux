#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Hardware-free bridge bootstrap/migration and address-log privacy checks."""
import configparser
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
UNIT = REPO / "rootfs/usr/lib/systemd/system/q25-bt-vhci.service"


class Q25BtVhciUnit(unittest.TestCase):
    def test_bootstrap_does_not_require_an_existing_controller(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(UNIT)
        self.assertEqual(config["Install"]["WantedBy"].split(), ["multi-user.target"])
        self.assertIn("q25-connsys.service", config["Unit"]["Requires"].split())
        self.assertIn("q25-connsys.service", config["Unit"]["After"].split())
        self.assertIn("bluetooth.service", config["Unit"]["Before"].split())
        self.assertEqual(config["Service"]["ExecStart"], "/usr/lib/q25/q25-bt-vhci")

    def enable_in_temporary_root(self, migrate):
        with tempfile.TemporaryDirectory(prefix="q25-bt-unit-") as directory:
            root = Path(directory)
            units = root / "usr/lib/systemd/system"
            units.mkdir(parents=True)
            shutil.copyfile(UNIT, units / UNIT.name)
            system = root / "etc/systemd/system"
            old_link = system / "bluetooth.target.wants" / UNIT.name
            if migrate:
                old_link.parent.mkdir(parents=True)
                old_link.symlink_to("/usr/lib/systemd/system/" + UNIT.name)
            subprocess.run(["systemctl", "--root=" + directory, "--no-reload",
                            "reenable" if migrate else "enable", UNIT.name],
                           check=True, capture_output=True, text=True, timeout=10)
            new_link = system / "multi-user.target.wants" / UNIT.name
            self.assertTrue(new_link.is_symlink())
            self.assertEqual(str(new_link.readlink()), "/usr/lib/systemd/system/" + UNIT.name)
            self.assertFalse(old_link.is_symlink(), "stale device-triggered activation remains")

    def test_fresh_enable_creates_bootstrap_link(self):
        self.enable_in_temporary_root(migrate=False)

    def test_reenable_migrates_old_bluetooth_target_link(self):
        self.enable_in_temporary_root(migrate=True)


class Q25BtVhciAddressLog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="q25-bt-address-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "q25-bt-vhci-test"
        subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(REPO / "tests/q25_bt_vhci_harness.c"), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True, timeout=30)

    def run_scenario(self, scenario):
        result = subprocess.run([str(self.binary), str(scenario)], check=True,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout, "")
        return result.stderr

    def test_factory_and_fallback_logs_do_not_contain_address(self):
        for scenario in (0, 1):
            with self.subTest(scenario=scenario):
                self.assertEqual(self.run_scenario(scenario),
                                 "[q25-bt-vhci] Factory address command submitted\n")

    def test_missing_address_is_silent(self):
        self.assertEqual(self.run_scenario(2), "")

    def test_short_read_and_write_are_silent(self):
        for scenario in (3, 4):
            with self.subTest(scenario=scenario):
                self.assertEqual(self.run_scenario(scenario), "")


if __name__ == "__main__":
    unittest.main()
