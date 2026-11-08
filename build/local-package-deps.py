#!/usr/bin/env python3
"""Select local build dependencies by .PKGINFO, never archive filenames.

Print one archive path per line. Ambiguous/incompatible local candidates are
errors; dependencies with no local candidate are left to pacman, except --require.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def dependency(text):
    match = re.fullmatch(r"([^<>=\s]+)(>=|<=|=|>|<)?([^\s]*)", text)
    if not match or (match[2] and not match[3]):
        raise ValueError(f"invalid dependency: {text}")
    return match[1], match[2], match[3]


def compatible(version, operator, required):
    if not operator:
        return True
    if not version:
        return False
    order = int(subprocess.check_output(["vercmp", version, required], text=True))
    return {"=": order == 0, ">=": order >= 0, "<=": order <= 0,
            ">": order > 0, "<": order < 0}[operator]


def select(directory, dependencies, arch, required_local):
    if not dependencies:
        return []
    archives = []
    for path in sorted(Path(directory).glob("*.pkg.tar.*")):
        if path.suffix == ".sig":
            continue
        info = subprocess.check_output(["bsdtar", "-xOf", str(path), ".PKGINFO"], text=True)
        metadata = {}
        for line in info.splitlines():
            if " = " in line:
                key, value = line.split(" = ", 1)
                metadata.setdefault(key, []).append(value)
        name, version, package_arch = [metadata[key][0] for key in ("pkgname", "pkgver", "arch")]
        provides = [(name, version)]
        for item in metadata.get("provides", []):
            provided, operator, provided_version = dependency(item)
            if operator not in (None, "="):
                raise ValueError(f"invalid provides in {path}: {item}")
            provides.append((provided, provided_version))
        archives.append((path.resolve(), package_arch, provides))
    selected = set()
    for spec in dependencies:
        name, operator, version = dependency(spec)
        candidates = [(path, package_arch, v) for path, package_arch, provides in archives
                      for n, v in provides if n == name]
        # Multiple versions/builds must not silently choose a stale artifact.
        if len({p for p, _, _ in candidates}) > 1:
            raise ValueError(f"ambiguous local dependency {spec}; clean stale archives in {directory}")
        if not candidates:
            if name in required_local:
                raise ValueError(f"missing local dependency {spec}; build {name} first into {directory}")
            continue
        path, package_arch, provided_version = candidates[0]
        if package_arch not in (arch, "any") or not compatible(provided_version, operator, version):
            raise ValueError(f"incompatible local dependency {spec}: {path} ({package_arch}, {provided_version})")
        selected.add(path)
    return sorted(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--require", action="append", default=[])
    parser.add_argument("directory")
    parser.add_argument("dependencies", nargs="*")
    args = parser.parse_args()
    try:
        for path in select(args.directory, args.dependencies, args.arch, set(args.require)):
            print(path)
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"local build dependencies: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
