#!/usr/bin/env python3
"""Local package build dependencies use archive metadata, not filename guesses."""
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("local_package_deps", REPO / "build/local-package-deps.py")
DEPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPS)


class LocalPackageDependencies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="q25-local-deps-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def archive(self, filename="arbitrary-name.pkg.tar.gz", name="keyd-q25", version="2.6.0-1", arch="aarch64", provides="keyd=2.6.0"):
        path = self.path / filename
        data = f"pkgname = {name}\npkgver = {version}\narch = {arch}\nprovides = {provides}\n".encode()
        with tarfile.open(path, "w:gz") as tar:
            info = tarfile.TarInfo(".PKGINFO")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        return path

    def select(self, *deps):
        return DEPS.select(self.path, deps, "aarch64", {"keyd-q25"})

    def test_metadata_name_version_arch_and_provides(self):
        path = self.archive()
        self.assertEqual(self.select("keyd-q25=2.6.0", "keyd>=2.6.0", "glibc"), [path])
        self.assertEqual(self.select("glibc"), [])

    def test_missing_local_dependency_has_actionable_error(self):
        result = subprocess.run(["python3", str(REPO / "build/local-package-deps.py"), "--arch", "aarch64",
                                 "--require", "keyd-q25", str(self.path), "keyd-q25=2.6.0"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing local dependency keyd-q25=2.6.0; build keyd-q25 first", result.stderr)

    def test_wrong_version_and_arch_are_rejected(self):
        self.archive(version="2.5.0-1")
        with self.assertRaisesRegex(ValueError, "incompatible local dependency"):
            self.select("keyd-q25=2.6.0")
        for item in self.path.iterdir(): item.unlink()
        self.archive(arch="x86_64")
        with self.assertRaisesRegex(ValueError, "incompatible local dependency"):
            self.select("keyd-q25=2.6.0")

    def test_ambiguous_or_stale_candidates_are_not_guessed(self):
        self.archive()
        self.archive(filename="stale.pkg.tar.gz", version="2.5.0-1")
        with self.assertRaisesRegex(ValueError, "ambiguous local dependency"):
            self.select("keyd-q25=2.6.0")

    def test_filename_cannot_impersonate_dependency(self):
        self.archive(filename="keyd-q25-2.6.0-1-aarch64.pkg.tar.gz", name="other", provides="unrelated")
        with self.assertRaisesRegex(ValueError, "missing local dependency"):
            self.select("keyd-q25=2.6.0")

    def test_unversioned_provides_cannot_satisfy_version_constraint(self):
        self.archive(provides="keyd")
        with self.assertRaisesRegex(ValueError, "incompatible local dependency"):
            self.select("keyd=2.6.0")
        self.assertEqual(len(self.select("keyd")), 1)


if __name__ == "__main__":
    unittest.main()
