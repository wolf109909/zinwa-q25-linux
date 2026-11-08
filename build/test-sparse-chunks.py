#!/usr/bin/env python3
"""Reconstruct self-contained sparse chunks and compare with a raw image.

Without arguments, test the chunk generator using a small synthetic image.
With RAW CHUNKS, validate the complete build (temporary reconstruction is removed).
"""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

GENERATOR = Path(__file__).with_name("mksparse-chunks.py")
BLOCK = 4096


def digest(source):
    source.seek(0)
    result = hashlib.sha256()
    for data in iter(lambda: source.read(1024 * 1024), b""):
        result.update(data)
    return result.hexdigest()


def verify(raw, chunks):
    size = raw.stat().st_size
    covered = bytearray(size // BLOCK)
    files = sorted(chunks.glob("*.simg"), reverse=True)  # deliberately reverse flash order
    assert files, "no sparse chunks"
    with tempfile.TemporaryFile(dir=raw.parent) as restored:
        restored.truncate(size)
        for path in files:
            assert path.stat().st_size < 64 * 1024 * 1024, f"LK download limit: {path}"
            with path.open("rb") as source:
                header = struct.unpack("<IHHHHIIII", source.read(28))
                assert header[:6] == (0xED26FF3A, 1, 0, 28, 12, BLOCK), header
                assert header[6] * BLOCK == size, "chunk is not self-contained"
                position = 0
                for _ in range(header[7]):
                    kind, _, count, total = struct.unpack("<HHII", source.read(12))
                    data = source.read(total - 12)
                    assert len(data) == total - 12
                    assert 0 < count <= size // BLOCK - position
                    if kind == 0xCAC1:
                        assert len(data) == count * BLOCK
                    elif kind == 0xCAC2:
                        assert len(data) == 4
                    elif kind == 0xCAC3:
                        assert not data
                    else:
                        raise AssertionError(f"unexpected chunk type {kind:x}")
                    if kind != 0xCAC3:
                        assert not any(covered[position:position + count]), "overlapping data"
                        covered[position:position + count] = b"\1" * count
                        restored.seek(position * BLOCK)
                        if kind == 0xCAC1:
                            restored.write(data)
                        elif data != b"\0" * 4:
                            for _ in range(count):
                                restored.write(data * (BLOCK // 4))
                        # FILL zero is already present in the sparse temporary file.
                    position += count
                assert position * BLOCK == size
                assert not source.read(1), "trailing bytes"
        assert all(covered), "unwritten blocks (stale userdata would survive flashing)"
        with raw.open("rb") as source:
            expected, actual = digest(source), digest(restored)
        assert expected == actual, "reconstructed image differs"
    return f"{len(files)} chunks, {size} bytes, SHA256 {actual}, reverse-order reconstruction matches"


class SparseChunks(unittest.TestCase):
    def test_mixed_raw_and_zero_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            raw = base / "test.img"
            raw.write_bytes(b"\0" * BLOCK * 5 + b"Q" * BLOCK * 260 +
                            b"\0" * BLOCK * 300 + b"R" * BLOCK * 270 + b"\0" * BLOCK)
            subprocess.run([sys.executable, str(GENERATOR), str(raw), str(base / "chunks"), "1"], check=True)
            verify(raw, base / "chunks")


if __name__ == "__main__":
    if len(sys.argv) == 3:
        print(verify(Path(sys.argv[1]), Path(sys.argv[2])))
    else:
        unittest.main()
