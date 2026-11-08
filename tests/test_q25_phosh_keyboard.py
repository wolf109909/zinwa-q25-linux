#!/usr/bin/env python3
"""Compile the plugin with real Phosh 0.57 widgets and test under private Xvfb.

Set PHOSH_SOURCE to the pinned 0.57.0 release source. No phone/session interaction.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = os.environ.get("PHOSH_SOURCE")


@unittest.skipUnless(SOURCE, "set PHOSH_SOURCE to Phosh 0.57.0 sources")
class PhoshKeyboardPlugin(unittest.TestCase):
    def test_real_widgets_lifecycle_order_and_credentials(self):
        source = Path(SOURCE)
        plugin = REPO / "packages/phosh-q25-keyboard"
        with tempfile.TemporaryDirectory(prefix="q25-phosh-") as directory:
            tmp = Path(directory)
            (tmp / "phosh-config.h").write_text("/* No feature defines needed by these upstream widgets. */\n")
            (tmp / "phosh-plugin.h").write_text('#define PHOSH_PLUGIN_EXTENSION_POINT_STATUS_ICON_WIDGET "phosh-status-icon-widget"\n')
            (tmp / "widgets.xml").write_text('<gresources><gresource prefix="/mobi/phosh/ui">' +
                ''.join(f'<file alias="{name}.ui">ui/{name}.ui</file>' for name in ("status-icon", "status-icons-box", "revealer")) +
                '</gresource></gresources>')
            for xml, srcdir, output in ((tmp / "widgets.xml", source / "src", "widgets.c"),
                                       (plugin / "q25-keyboard.gresources.xml", plugin, "icons.c")):
                subprocess.run(["glib-compile-resources", f"--sourcedir={srcdir}", "--generate-source",
                                f"--target={tmp / output}", str(xml)], check=True)
            flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "gtk+-3.0", "gio-unix-2.0"], text=True))
            subprocess.run(["gcc", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Werror",
                            "-I", str(tmp), "-I", str(plugin), "-I", str(source / "src"),
                            f'-DQ25_STATE_SOCKET="{tmp}/state.sock"',
                            str(REPO / "tests/q25_phosh_keyboard_test.c"),
                            *[str(source / "src" / (unit + ".c")) for unit in ("status-icon", "status-icons-box", "revealer")],
                            str(tmp / "widgets.c"), str(tmp / "icons.c"), "-o", str(tmp / "test-plugin"), *flags], check=True)
            env = dict(os.environ, NO_AT_BRIDGE="1", XDG_RUNTIME_DIR=directory)
            subprocess.run(["xvfb-run", "-a", str(tmp / "test-plugin")], check=True, env=env)
            # A separate executable loads the actual shared module with upstream
            # GIOModule discovery (not the direct-linked widget above).
            modules = tmp / "modules"
            modules.mkdir()
            includes = ["-I", str(tmp), "-I", str(source / "src")]
            subprocess.run(["gcc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                            *includes, f'-DQ25_STATE_SOCKET="{tmp}/state.sock"',
                            str(plugin / "q25-keyboard.c"), str(tmp / "icons.c"),
                            "-o", str(modules / "libphosh-plugin-q25-keyboard.so"), *flags], check=True)
            module_flags = shlex.split(subprocess.check_output(["pkg-config", "--libs", "gmodule-2.0"], text=True))
            subprocess.run(["gcc", "-Wl,--export-dynamic", *includes,
                            str(REPO / "tests/q25_phosh_plugin_loader_test.c"),
                            str(source / "src/status-icon.c"), str(source / "src/plugin-loader.c"),
                            str(tmp / "widgets.c"), "-o", str(tmp / "test-loader"), *flags, *module_flags], check=True)
            subprocess.run(["xvfb-run", "-a", str(tmp / "test-loader"), str(modules)], check=True, env=env)


if __name__ == "__main__":
    unittest.main()
