#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Split a raw filesystem image into self-contained Android sparse images.

    mksparse-chunks.py <raw.img> <outdir> [MiB-per-chunk]

Every output file describes the whole image (same total block count), but only
carries the data of its own slice; everything else is DONT_CARE, so the chunks
can be flashed in any order with plain `fastboot flash userdata <chunk>`.
All-zero runs are encoded as FILL chunks, which keeps them small to transfer
while still overwriting stale data from a previous installation.

Why: Google's Windows fastboot.exe is 32-bit and runs out of memory on
multi-GiB images; small self-contained chunks avoid that on every host.
"""
import os
import struct
import sys

BLK = 4096
MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
ZERO = bytes(BLK)


def write_chunk_file(path, total_blocks, first, blocks):
    """blocks: list of (is_zero, bytes|None) for the slice starting at `first`."""
    chunks = []
    if first:
        chunks.append((DONT_CARE, first, b""))
    i = 0
    while i < len(blocks):
        zero = blocks[i] is None
        j = i
        while j < len(blocks) and (blocks[j] is None) == zero:
            j += 1
        if zero:
            chunks.append((FILL, j - i, struct.pack("<I", 0)))
        else:
            chunks.append((RAW, j - i, b"".join(blocks[i:j])))
        i = j
    tail = total_blocks - first - len(blocks)
    if tail:
        chunks.append((DONT_CARE, tail, b""))
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK,
                            total_blocks, len(chunks), 0))
        for typ, count, data in chunks:
            f.write(struct.pack("<HHII", typ, 0, count, 12 + len(data)))
            f.write(data)


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    size = os.path.getsize(src)
    if size % BLK:
        sys.exit(f"{src}: size {size} is not a multiple of {BLK}")
    total = size // BLK
    per_blocks = per * 1024 * 1024 // BLK
    os.makedirs(outdir, exist_ok=True)
    for old in os.listdir(outdir):
        if old.endswith(".simg"):
            os.unlink(os.path.join(outdir, old))
    names = []
    with open(src, "rb") as f:
        n = 0
        first = 0
        while first < total:
            # a slice holds up to `per` MiB of non-zero data; zero runs are free
            blocks, data = [], 0
            while first + len(blocks) < total and data < per_blocks:
                b = f.read(BLK)
                if b == ZERO:
                    blocks.append(None)
                else:
                    blocks.append(b)
                    data += 1
            name = f"userdata_{n:03d}.simg"
            write_chunk_file(os.path.join(outdir, name), total, first, blocks)
            names.append(name)
            first += len(blocks)
            n += 1
    with open(os.path.join(outdir, "flash-userdata.txt"), "w") as f:
        f.write("# flash in this order (any order works; all are self-contained)\n")
        for name in names:
            f.write(f"fastboot flash userdata {name}\n")
    print(f"{src}: {total} blocks -> {len(names)} sparse chunks in {outdir}")


if __name__ == "__main__":
    main()
