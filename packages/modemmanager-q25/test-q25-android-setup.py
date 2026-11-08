#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ensure runtime refresh never truncates mapped Android libraries."""
import mmap
from pathlib import Path
import runpy
import shutil
import tempfile
import unittest
from unittest import mock

SETUP = Path(__file__).resolve().parents[2] / "rootfs/usr/lib/q25/q25-android-setup"
sync_dir = runpy.run_path(str(SETUP))["sync_dir"]


class AndroidRuntimeCopyTest(unittest.TestCase):
    def test_sync_dir_replaces_inode_without_truncating_mapping(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "src", Path(tmp) / "dst"
            src.mkdir()
            dst.mkdir()
            (src / "libc.so").write_bytes(b"new")
            (dst / "libc.so").write_bytes(b"old mapped library")
            with (dst / "libc.so").open("rb") as old:
                with mmap.mmap(old.fileno(), 0, access=mmap.ACCESS_READ) as mapping:
                    copy2 = shutil.copy2

                    def copy_before_replace(source, target, **kwargs):
                        self.assertEqual((dst / "libc.so").read_bytes(), b"old mapped library")
                        return copy2(source, target, **kwargs)

                    with mock.patch("shutil.copy2", side_effect=copy_before_replace):
                        sync_dir(str(src), str(dst))
                    self.assertEqual(mapping[:], b"old mapped library")
                    self.assertEqual((dst / "libc.so").read_bytes(), b"new")
            self.assertEqual(list(dst.glob(".q25-*")), [])

    def test_sync_dir_preserves_symlinks_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "src", Path(tmp) / "dst"
            src.mkdir()
            (src / "linker64").symlink_to("/system/bin/linker64")
            sync_dir(str(src), str(dst))
            sync_dir(str(src), str(dst))
            self.assertEqual((dst / "linker64").readlink(), Path("/system/bin/linker64"))


if __name__ == "__main__":
    unittest.main()
