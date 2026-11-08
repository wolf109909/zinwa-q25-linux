#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the board DTS without modifying KSRC and check hardware links.

KSRC defaults to the same prepared kernel tree as build/build-kernel.sh.
Requires aarch64-linux-gnu-gcc, dtc and fdtget.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class Q25HardwareDescription(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="q25-dt-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        cls.dtb = out / "q25.dtb"
        kernel = Path(os.environ.get("KSRC", "~/q25-work/linux-7.3")).expanduser()
        source = Path(__file__).resolve().parents[1] / "mt8781-zinwa-q25.dts"
        subprocess.run([
            os.environ.get("CROSS_COMPILE", "aarch64-linux-gnu-") + "gcc",
            "-E", "-nostdinc", "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
            "-I", str(kernel / "include"),
            "-I", str(kernel / "arch/arm64/boot/dts/mediatek"),
            str(source), "-o", str(out / "q25.dts"),
        ], check=True, capture_output=True)
        subprocess.run([
            "dtc", "-@", "-I", "dts", "-O", "dtb", "-o", str(cls.dtb),
            str(out / "q25.dts"),
        ], check=True, capture_output=True)

    def prop(self, node, prop, fmt="s"):
        return subprocess.check_output([
            "fdtget", "-t", fmt, str(self.dtb), node, prop,
        ], text=True).strip()

    def symbol(self, name):
        return self.prop("/__symbols__", name)

    def test_panel_uses_real_ocp2131_backlight(self):
        panel = self.symbol("panel")
        backlight = self.symbol("i2c_lcd_bias_mtk")
        self.assertEqual(self.prop(backlight, "compatible"),
                         "mediatek,mtk-gateic-drv-ocp2131")
        self.assertEqual(self.prop(panel, "backlight", "x"),
                         self.prop(backlight, "phandle", "x"))

    def test_keyboard_follows_the_dsi_panel(self):
        self.assertEqual(self.prop(self.symbol("bbqX0kbd"), "panel", "x"),
                         self.prop(self.symbol("panel"), "phandle", "x"))

    def test_sensor_has_explicit_uncalibrated_fallback(self):
        self.assertEqual(self.prop("/sensors", "compatible"), "zinwa,q25-sensors")
        self.assertEqual(self.prop("/sensors", "mount-matrix").split(),
                         ["1", "0", "0", "0", "1", "0", "0", "0", "1"])


if __name__ == "__main__":
    unittest.main()
