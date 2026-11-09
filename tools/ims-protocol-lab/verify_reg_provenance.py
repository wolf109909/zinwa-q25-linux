#!/usr/bin/env python3
"""Pin producer/consumer registration evidence by reading archived ARM32 files only."""
import argparse
import hashlib
from pathlib import Path
import struct

# Literal words/instructions, not executable code. See REGISTRATION-PROVENANCE.md.
IMAGES = {
    "volte_imcb": (
        "5ce2ac3145c5eb58421d12dc489a35d930e0055ff0c709a6378df95b5d307985",
        {
            0x58EC8: 0xE3001100,  # allocate256
            0x58EEC: 0xE3020AF9,  # event11001
            0x58EF4: 0xE3002100,  # declared length256
            0x58FFC: 0xE5D00000,  # account source byte
            0x59004: 0xE5810000,  # account destination word+0
            0x59010: 0xE2811008,  # cell-ID destination+8
            0x79450: 0xE3002040,  # cell-ID copy64
            0x59018: 0xE3002034,  # opaque copy52
            0x59024: 0xE28EE0C8,  # opaque destination+200
        },
        {0x58EE8: 0x58F2C, 0x59014: 0x7AA58, 0x79470: 0x2094, 0x59038: 0x2094},
    ),
    "volte_ua": (
        "91a1143c804e84a809c6b953badacfbcb71c33571e4938599ed9661639a3c6c9",
        {
            0x9BB4: 0xE3020AF9,  # select event11001
            0xBA58: 0xE3500C01,  # require256
            0xCA74: 0x00016379,  # remap literal91001
            0xBAA4: 0xE58D165C,  # keep length
            0xBAAC: 0xE58D1660,  # keep payload pointer
            0x7008: 0xE50B0024,  # Channel header ID
            0x7014: 0xE50B0020,  # Channel header length
            0x701C: 0xE50B001C,  # Channel header word8 zero
            0x7028: 0xE50B1018,  # local payload pointer (not on wire)
        },
        {0x9BC0: 0xBA54, 0xBAC0: 0x6C68, 0x7038: 0x5E34},
    ),
    "volte_stack": (
        "6f46ece0574eaf611ca20b1bc8707450803bcef6917f7eed59f1b8d14d493cb5",
        {
            0x3F8C8: 0xE3060379,  # event91001 low16
            0x3F8CC: 0xE3400001,  # event91001 high16
            0x43248: 0xE3500C01,  # require256
            0x43284: 0xE5900000,  # account word+0
            0x43340: 0xE1D000D8,  # test cell-ID first byte at+8
            0x4334C: 0xE3002040,  # bounded cell-ID copy64
            0x43360: 0xE2811008,  # cell-ID source+8
            0x43514: 0xE28110C8,  # opaque source+200
            0x52C50: 0xE3002034,  # opaque copy52
        },
        {0x3F8D8: 0x43234, 0x43290: 0x5287C, 0x528A0: 0x52980,
         0x43364: 0x5438, 0x43518: 0x52C28, 0x52C64: 0x5330},
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_dir", type=Path, help="local preserved bin/ directory")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without Python assertion optimization")
    for name, (digest, words, branches) in IMAGES.items():
        try:
            data = (args.bin_dir / name).read_bytes()
        except OSError as error:
            parser.error(str(error))
        if hashlib.sha256(data).hexdigest() != digest:
            parser.error(f"{name}: identity differs from analyzed artifact")
        assert data[:6] == b"\x7fELF\x01\x01"
        assert struct.unpack_from("<H", data, 18)[0] == 40
        phoff = struct.unpack_from("<I", data, 28)[0]
        phsize, phnum = struct.unpack_from("<HH", data, 42)
        loads = [struct.unpack_from("<8I", data, phoff + i * phsize) for i in range(phnum)]
        last = max(max(words), max(branches)) + 4
        assert any(p[0] == 1 and p[1] == p[2] == 0 and p[4] >= last for p in loads)
        for address, expected in words.items():
            assert struct.unpack_from("<I", data, address)[0] == expected, (name, hex(address))
        for address, target in branches.items():
            word = struct.unpack_from("<I", data, address)[0]
            assert word & 0x0E000000 == 0x0A000000  # ARM B or BL
            relative = word & 0xFFFFFF
            if relative & 0x800000:
                relative -= 0x1000000
            assert address + 8 + relative * 4 == target, (name, hex(address))
        print(f"PASS {name}: identity, {len(words)} words, {len(branches)} branches")
    print("Static producer/consumer proof only; no vendor code loaded or executed.")


if __name__ == "__main__":
    main()
