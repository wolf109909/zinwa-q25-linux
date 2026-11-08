#!/usr/bin/env python3
"""Check btrfs overlay; Q25_BTRFS_TOP/ROOT optionally verify read-only image mounts."""
import os
from pathlib import Path
import unittest

REPO = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get("Q25_BTRFS_ROOT", REPO / "rootfs"))
SUBVOLUMES = {
    "/home": "@home", "/root": "@root", "/srv": "@srv", "/var/cache": "@cache",
    "/var/log": "@log", "/var/tmp": "@tmp", "/.snapshots": "@snapshots",
}


class BtrfsDefaults(unittest.TestCase):
    def test_fstab_default_and_exclusions(self):
        rows = [line.split() for line in (ROOT / "etc/fstab").read_text().splitlines()
                if line and not line.startswith("#")]
        btrfs = {row[1]: row for row in rows if row[0] == "PARTLABEL=userdata"}
        self.assertEqual(set(btrfs), {"/", *SUBVOLUMES})
        for target, row in btrfs.items():
            self.assertEqual(row[2], "btrfs")
            self.assertEqual(row[4:], ["0", "0"])
            options = {"noatime", "compress=zstd:1"}
            if target != "/":
                options.add("subvol=" + SUBVOLUMES[target])
            self.assertEqual(set(row[3].split(",")), options)

    def test_snapper_root_and_bounded_retention(self):
        values = dict(line.split("=", 1) for line in (ROOT / "etc/snapper/configs/root").read_text().splitlines()
                      if line and not line.startswith("#"))
        self.assertEqual(values["SUBVOLUME"], '"/"')
        self.assertEqual(values["FSTYPE"], '"btrfs"')
        self.assertEqual(values["UNIQUE_NUMBERS"], '"yes"')
        for key, number in (("HOURLY", 6), ("DAILY", 7), ("WEEKLY", 4), ("MONTHLY", 1), ("YEARLY", 0)):
            self.assertEqual(values["TIMELINE_LIMIT_" + key], f'"{number}"')
        self.assertIn('SNAPPER_CONFIGS="root"', (ROOT / "etc/conf.d/snapper").read_text())
        packages = (REPO / "build/packages.txt").read_text().splitlines()
        for package in ("btrfs-progs", "snapper", "snap-pac", "btrfs-assistant"):
            self.assertIn(package, packages)

    def test_kernel_and_firstboot(self):
        kernel = (REPO / "cachyos/config-mt6789-mainline-7.3.aarch64").read_text().splitlines()
        self.assertIn("CONFIG_BTRFS_FS=y", kernel)
        for option in ("BTRFS_FS_RUN_SANITY_TESTS", "BTRFS_ASSERT"):
            self.assertIn(f"# CONFIG_{option} is not set", kernel)
        firstboot = (ROOT / "usr/lib/q25/q25-firstboot").read_text()
        self.assertIn("btrfs filesystem resize max /", firstboot)
        self.assertNotIn("resize2fs", firstboot)
        init = (REPO / "initramfs/init").read_text()
        self.assertNotIn("e2fsck", init)

    @unittest.skipUnless(os.environ.get("Q25_BTRFS_TOP"), "mounted image not requested")
    def test_mounted_image_layout(self):
        top = Path(os.environ["Q25_BTRFS_TOP"])
        import subprocess
        def btrfs(*args):
            return subprocess.check_output(["btrfs", *args], text=True).strip()
        default = btrfs("subvolume", "get-default", str(top))
        self.assertTrue(default.endswith(" path @"), default)
        self.assertEqual(btrfs("inspect-internal", "rootid", str(ROOT)), default.split()[1])
        subvols = btrfs("subvolume", "list", str(top)).splitlines()
        self.assertEqual({line.split(" path ", 1)[1] for line in subvols}, {"@", *SUBVOLUMES.values()})
        for path, subvol in SUBVOLUMES.items():
            self.assertTrue((top / subvol).is_dir())
            self.assertEqual(list((ROOT / path.lstrip("/")).iterdir()), [])
        self.assertEqual((top / "q25-boot").stat().st_mode & 0o777, 0o700)
        self.assertTrue((top / "@home/alarm").is_dir())
        self.assertTrue(any((ROOT / "usr/lib/modules").glob("*/modules.dep")))
        for timer in ("snapper-timeline.timer", "snapper-cleanup.timer"):
            self.assertTrue((ROOT / "etc/systemd/system/timers.target.wants" / timer).is_symlink())


if __name__ == "__main__":
    unittest.main()
