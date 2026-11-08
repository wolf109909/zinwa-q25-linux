#!/usr/bin/env python3
"""Real keyd 2.6 engine/daemon tests with synthetic events and no input devices.

KEYD_SOURCE must point to pristine keyd v2.6.0 sources. The test applies the
packaged patch in a temporary copy; no daemon, root privileges or uinput needed.
"""
import ctypes
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = os.environ.get("KEYD_SOURCE")


@unittest.skipUnless(SOURCE, "set KEYD_SOURCE to pristine keyd 2.6.0 sources")
class KeyboardEngine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="q25-keyboard-")
        cls.work = Path(cls.tmp.name)
        src = cls.work / "keyd"
        shutil.copytree(SOURCE, src, ignore=shutil.ignore_patterns(".git", "bin"))
        subprocess.run(["patch", "-p1", "-i", str(REPO / "packages/keyd-q25/0001-q25-read-only-state.patch")], cwd=src, check=True)
        for name in ("q25-state.c", "q25-state.h"):
            shutil.copy(REPO / "packages/keyd-q25" / name, src / "src" / name)
        conf = cls.work / "config"
        conf.mkdir()
        shutil.copy(REPO / "rootfs/etc/keyd/default.conf", conf / "default.conf")
        units = ["keyboard", "string", "macro", "config", "log", "ini", "keys", "unicode", "ipc", "util", "q25-state"]
        # Compile the real IRQ worker/keymap too, replacing only kernel IO.
        driver = (REPO / "drivers/keyboard/bbqX0kbd_main.c").read_text()
        mod_start = driver.index("static uint8_t bbqX0kbd_modkeys_to_bits(")
        mod_end = driver.index("#if (BBQX0KBD_INT", mod_start)
        irq_start = driver.rindex("static void bbqX0kbd_work_fnc(")
        irq_end = driver.index("static irqreturn_t bbqX0kbd_irq_handler", irq_start)
        (cls.work / "driver-test-functions.c").write_text(driver[mod_start:mod_end] + driver[irq_start:irq_end])
        so = cls.work / "keyd-test.so"
        subprocess.run(["gcc", "-shared", "-fPIC", "-g", "-Wall", "-Werror=format-security",
                        "-D_DEFAULT_SOURCE", '-DDATA_DIR=""', '-DVERSION="test"',
                        f'-DCONFIG_DIR="{conf}"', f'-DSOCKET_PATH="{cls.work}/unused-control"',
                        "-iquote", str(src / "src"), "-iquote", str(cls.work),
                        str(REPO / "tests/q25_keyd_harness.c"), str(REPO / "tests/q25_keyboard_driver_harness.c"),
                        *[str(src / "src" / (u + ".c")) for u in units], "-o", str(so)], check=True)
        cls.lib = ctypes.CDLL(str(so))
        cls.lib.test_name.restype = ctypes.c_char_p
        cls.path = str(cls.work / "state.sock")
        assert cls.lib.q25_state_init(cls.path.encode()) >= 0
        cls.now = 100

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.lib.test_setup()

    def event(self, name=None, pressed=0, after=10):
        type(self).now += after
        code = self.lib.test_code(name.encode()) if name else 0
        self.assertGreaterEqual(code, 0, name)
        self.lib.test_event(code, pressed, self.now)

    def tap(self, name):
        self.event(name, 1)
        self.event(name, 0)

    def output(self):
        return [(self.lib.test_name(self.lib.test_output_code(i)).decode(), self.lib.test_output_state(i))
                for i in range(self.lib.test_output_count())]

    def strokes(self):
        """Keys down with *all* emitted modifiers, not just presence of a key."""
        mods, strokes = set(), []
        for key, down in self.output():
            if key in {"leftshift", "leftcontrol", "leftalt", "leftmeta", "rightalt"}:
                (mods.add if down else mods.discard)(key)
            elif down:
                strokes.append((key, frozenset(mods)))
        return strokes

    def connection(self):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(0.25)
        client.connect(self.path)
        self.lib.q25_state_accept()
        self.addCleanup(client.close)
        return client

    def snapshot(self, client, shift, sym):
        self.assertEqual(client.recv(4), bytes([ord("Q"), 1, shift, sym]))

    def test_pickup_tap_and_function_positions_no_tab_leak(self):
        self.tap("send")
        self.assertEqual(self.strokes(), [("tab", frozenset())])
        for n, key in enumerate("w e r s d f z x c 0 v b".split(), 1):
            for early_release in (False, True):
                with self.subTest(key=key, early_release=early_release):
                    self.lib.test_reset_output()
                    self.event("send", 1)
                    self.event(key, 1)
                    self.event("send" if early_release else key, 0)
                    self.event(key if early_release else "send", 0)
                    self.assertEqual(self.strokes(), [(f"f{n}", frozenset())])
                    self.assertNotIn(("tab", 1), self.output())

    def test_red_threshold_release_repeat_and_rearm(self):
        self.event("homepage", 1)
        self.event("homepage", 0, after=599)
        self.assertEqual(self.strokes(), [("d", frozenset({"leftmeta"}))])
        for hold in (600, 5000, 60000):
            self.lib.test_reset_output()
            self.event("homepage", 1)
            self.event(after=600)
            # Real repeated downs and unrelated timeout events must not close twice.
            for _ in range(5):
                self.event("homepage", 1, after=100)
                self.event(after=100)
            self.event("homepage", 0, after=hold)
            self.assertEqual(self.strokes(), [("f4", frozenset({"leftalt"}))])
            self.assertEqual(self.output().count(("f4", 0)), 1)
        self.lib.test_reset_output()
        self.tap("homepage")
        self.assertEqual(self.strokes(), [("d", frozenset({"leftmeta"}))])

    def test_red_interrupted_hold_never_closes(self):
        self.event("homepage", 1)
        self.tap("a")
        self.event(after=1000)
        self.event("homepage", 0)
        self.assertNotIn(("f4", 1), self.output())

    def test_back_forward_consumes_typing_and_printed_shift(self):
        self.tap("back")
        self.assertEqual(self.strokes(), [("left", frozenset({"leftalt"}))])
        for modifier in ("leftalt", "leftshift"):
            self.lib.test_setup()
            self.event(modifier, 1)
            self.tap("back")
            self.event(modifier, 0)
            self.assertEqual(self.strokes(), [("right", frozenset({"leftalt"}))])

    def test_shifted_pipe_plain_dollar_and_modifiers_unchanged(self):
        self.tap("grave")
        self.assertEqual(self.strokes(), [("4", frozenset({"leftshift"}))])
        self.lib.test_reset_output()
        self.event("leftalt", 1)
        self.tap("grave")
        self.event("leftalt", 0)
        self.assertEqual(self.strokes(), [("backslash", frozenset({"leftshift"}))])
        for modifier, output in (("leftshift", "leftcontrol"), ("0", "leftalt")):
            self.lib.test_reset_output()
            self.event(modifier, 1)
            self.tap("a")
            self.event(modifier, 0)
            self.assertEqual(self.strokes(), [("a", frozenset({output}))])

    def test_berry_is_unmodified_super(self):
        self.tap("leftmeta")
        self.assertEqual(self.output(), [("leftmeta", 1), ("leftmeta", 0)])

    def test_preserved_sym_and_rightshift_layers(self):
        self.event("rightalt", 1)
        self.tap("w")
        self.event("rightalt", 0)
        self.assertEqual(self.strokes(), [("1", frozenset())])
        self.lib.test_reset_output()
        self.event("rightshift", 1)
        self.tap("q")
        self.tap("w")
        self.event("rightshift", 0)
        self.assertEqual(self.strokes(), [("esc", frozenset()), ("pageup", frozenset())])

    def test_sym_navigation_and_backspace_held_oneshot_locked(self):
        for mode in ("held", "oneshot", "locked"):
            for key in ("backspace", "up", "down", "left", "right", "home"):
                with self.subTest(mode=mode, key=key):
                    self.lib.test_setup()
                    if mode == "held":
                        self.event("rightalt", 1)
                        expected = 1
                    else:
                        self.tap("rightalt")
                        expected = 2
                        if mode == "locked":
                            self.tap("rightalt")
                            expected = 4
                    self.assertEqual(self.lib.test_layer_flags(), expected << 8)
                    self.lib.test_reset_output()
                    self.tap(key)
                    self.assertEqual(self.strokes(), [(key, frozenset())])
                    self.assertEqual(self.lib.test_layer_flags(), (0 if mode == "oneshot" else expected) << 8)
                    if mode == "held":
                        self.event("rightalt", 0)
                        self.assertEqual(self.lib.test_layer_flags(), 0)
                    elif mode == "locked":
                        self.tap("rightalt")
                        self.assertEqual(self.lib.test_layer_flags(), 0)
        # Punctuation stays on its printed letter positions, not navigation.
        self.lib.test_setup()
        self.event("rightalt", 1)
        for key in ("y", "g", "h", "j", "b"):
            self.tap(key)
        self.event("rightalt", 0)
        self.assertEqual(self.strokes(), [("0", frozenset({"leftshift"})),
                                         ("/", frozenset()),
                                         (";", frozenset({"leftshift"})),
                                         (";", frozenset()),
                                         ("1", frozenset({"leftshift"}))])

    def test_driver_fifo_trackpad_to_keyd_full_path(self):
        def fifo(scan, down):
            type(self).now += 10
            self.lib.driver_key(scan, down, self.now)
        def tap_scan(scan):
            fifo(scan, 1)
            fifo(scan, 0)
        for mode in ("held", "oneshot", "locked"):
            for motion, expected in (((0, -30), "up"), ((0, 30), "down"),
                                     ((-30, 0), "left"), ((30, 0), "right"),
                                     (8, "backspace"), (5, "enter"), (ord("m"), "home")):
                with self.subTest(mode=mode, event=expected):
                    self.lib.test_setup()
                    self.lib.driver_init()
                    fifo(0x1D, 1)  # Physical Sym through driver modifier tracking.
                    if mode != "held":
                        fifo(0x1D, 0)
                    if mode == "locked":
                        tap_scan(0x1D)
                    self.lib.test_reset_output()
                    if isinstance(motion, tuple):
                        type(self).now += 10
                        self.lib.driver_motion(*motion, self.now)
                    else:
                        tap_scan(motion)
                    self.assertEqual(self.strokes(), [(expected, frozenset())])
                    self.assertEqual(self.lib.test_layer_flags(), {"held": 1, "oneshot": 0, "locked": 4}[mode] << 8)
        self.lib.test_setup()
        self.lib.driver_init()
        # IRQ driver must pass Sym+H as H, not legacy polling-path KEY_HOME.
        fifo(0x1D, 1)
        tap_scan(ord("H"))
        fifo(0x1D, 0)
        self.assertEqual(self.strokes(), [(";", frozenset({"leftshift"}))])
        # RightShift+T remains the driver toggle: mouse movement/click is not a
        # punctuation key, and switching back restores arrows and Enter.
        fifo(0x1C, 1)
        tap_scan(ord("T"))
        fifo(0x1C, 0)
        self.lib.test_reset_output()
        fifo(0x1D, 1)
        type(self).now += 10
        self.lib.driver_motion(30, 0, self.now)
        tap_scan(5)
        fifo(0x1D, 0)
        self.assertEqual(self.strokes(), [])
        self.assertEqual(self.lib.driver_mouse_code(), 0x110)  # BTN_LEFT
        self.assertEqual(self.lib.driver_relative_events(), 2)
        fifo(0x1C, 1)
        tap_scan(ord("T"))
        fifo(0x1C, 0)
        self.lib.test_reset_output()
        type(self).now += 10
        self.lib.driver_motion(0, -30, self.now)
        tap_scan(5)
        self.assertEqual(self.strokes(), [("up", frozenset()), ("enter", frozenset())])

    def test_authoritative_held_oneshot_locked_and_consumption(self):
        for name, bitshift in (("leftalt", 0), ("rightalt", 8)):
            self.lib.test_setup()
            client = self.connection()
            self.snapshot(client, 0, 0)
            def expect(flags):
                self.assertEqual(self.lib.test_layer_flags(), flags << bitshift)
                self.snapshot(client, flags if bitshift == 0 else 0, flags if bitshift else 0)
            self.event(name, 1)
            expect(1)
            self.event(name, 0)
            expect(2)
            self.event("a", 1)
            expect(0)
            self.event("a", 0)
            self.event(name, 1)
            expect(1)
            self.event(name, 0)
            expect(2)
            self.tap(name)
            expect(4)
            self.tap("a")
            self.assertEqual(self.lib.test_layer_flags(), 4 << bitshift)
            # A new observer must see current lock, not infer from past events.
            reconnect = self.connection()
            self.snapshot(reconnect, 4 if bitshift == 0 else 0, 4 if bitshift else 0)
            self.tap(name)
            expect(0)
            client.close()
            reconnect.close()

    def test_clear_reload_hotplug_and_real_caps_led(self):
        client = self.connection()
        self.snapshot(client, 0, 0)
        self.tap("rightalt")
        self.snapshot(client, 0, 1)
        self.snapshot(client, 0, 2)
        self.lib.test_clear()
        self.tap("f24")
        self.snapshot(client, 0, 0)
        self.tap("leftalt")
        self.snapshot(client, 1, 0)
        self.snapshot(client, 2, 0)
        self.lib.test_reload()
        self.snapshot(client, 0, 0)
        # Regression: active_kbd used to dangle across reload.
        self.event(after=1000)
        self.lib.test_led(1, self.now + 1)
        self.snapshot(client, 8, 0)
        self.lib.test_reload()
        self.assertEqual(self.lib.test_layer_flags(), 0)
        # Real compositor CapsLock isn't a keyd layer; reload must not lie about it.
        again = self.connection()
        self.snapshot(again, 8, 0)
        self.lib.test_led(0, self.now + 2)
        self.snapshot(client, 0, 0)
        self.event("rightalt", 1)
        self.snapshot(client, 0, 1)
        self.lib.test_remove(self.now + 3)
        self.snapshot(client, 0, 0)
        self.assertEqual(self.lib.test_layer_flags(), 0)
        self.event(after=1000)
        self.lib.test_reload()
        self.assertEqual(self.lib.test_layer_flags(), 0)

    def test_state_socket_is_bounded_nonblocking_receive_only(self):
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o666)
        client = self.connection()
        self.snapshot(client, 0, 0)
        client.sendall(b"input malicious-text\n")
        self.lib.q25_state_publish(0, 0)
        try:
            self.assertEqual(client.recv(128), b"")
        except ConnectionResetError:
            pass
        self.assertEqual(self.output(), [])
        peers = [self.connection() for _ in range(9)]
        self.assertEqual(peers[-1].recv(4), b"")
        start = time.monotonic()
        for i in range(10000):
            self.lib.q25_state_publish(i % 2, 0)
        self.assertLess(time.monotonic() - start, 2)
        # Stalled clients were dropped; an observer can reconnect and resync.
        fresh = self.connection()
        self.snapshot(fresh, 1, 0)
        for peer in peers:
            data = b""
            while True:
                chunk = peer.recv(4096)
                if not chunk:
                    break
                data += chunk
            self.assertEqual(len(data) % 4, 0)
            self.assertLess(len(data), 16384)
            self.assertTrue(all(data[i:i+2] == b"Q\1" for i in range(0, len(data), 4)))


if __name__ == "__main__":
    unittest.main()
