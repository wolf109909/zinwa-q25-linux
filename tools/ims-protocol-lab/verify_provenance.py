#!/usr/bin/env python3
"""Read-only checks of the pinned ARM32 DSO; never dlopen or execute it."""
import argparse
import hashlib
from pathlib import Path
import struct

SHA256 = "0a4db679d65bdad2a098ecf752cde88c50907f8682a3d0fbfc47006a2a8e752f"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path, help="local preserved libvolte_core_shr.so")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without Python assertion optimization")
    try:
        data = args.library.read_bytes()
    except OSError as error:
        parser.error(str(error))
    if hashlib.sha256(data).hexdigest() != SHA256:
        parser.error("library identity differs from the analyzed artifact")
    assert data[:6] == b"\x7fELF\x01\x01"  # ELF32, little endian
    assert struct.unpack_from("<H", data, 18)[0] == 40  # EM_ARM
    phoff = struct.unpack_from("<I", data, 28)[0]
    phsize, phnum = struct.unpack_from("<HH", data, 42)
    loads = [struct.unpack_from("<8I", data, phoff + i * phsize) for i in range(phnum)]
    assert any(p[0] == 1 and p[1] == 0 and p[2] == 0 and p[4] > 0x72AF4 for p in loads)

    # All checked virtual addresses lie in that VA==file-offset LOAD segment.
    words = {
        0x668B0: 0xE50B0014,  # service header+0 = argument1
        0x668B8: 0xE50B0010,  # service header+4 = argument2
        0x66880: 0xE3001000,  # zero value for service header+8
        0x6689C: 0xE58D1010,  # save zero at stack+16
        0x668BC: 0xE59D0010,  # reload zero
        0x668C0: 0xE50B000C,  # service header+8 = zero
        0x668C8: 0xE50B1008,  # in-memory +12 = payload pointer
        0x6B380: 0xE5900008,  # SIP wrapper copies opaque word8
        0x6B384: 0xE50B0014,
        0x16EEC: 0x0A00003A,  # zero length bypasses nonzero-payload validation
        0x16EFC: 0xBA000004,  # signed negative length rejected
        0x16FE8: 0x7FFFFFFE,  # sender payload ceiling
        0x17010: 0xE300200C,  # header write length = 12
        0x17058: 0xE591100C,  # body buffer comes from memory+12
        0x17060: 0xE5922004,  # body write length comes from header+4
        0x18120: 0xE300200C,  # receiver header read length = 12
        0x18164: 0xE5900004,  # receiver payload length at +4
        0x1817C: 0xBA000004,  # receiver rejects signed negative length
        0x18274: 0x7FFFFFFE,  # receiver payload ceiling
        0x18434: 0xE3003000,  # send flags zero; buffer goes unchanged to send
        0x182D0: 0xE3003000,  # recv flags zero
    }
    for address, expected in words.items():
        actual = struct.unpack_from("<I", data, address)[0]
        assert actual == expected, f"instruction/literal mismatch at 0x{address:x}"

    def branch_target(address):
        word = struct.unpack_from("<I", data, address)[0]
        assert word & 0x0F000000 == 0x0B000000  # ARM BL
        relative = word & 0xFFFFFF
        if relative & 0x800000:
            relative -= 0x1000000
        return address + 8 + relative * 4

    for address, target in {
        0x1702C: 0x183E8,  # header -> raw send helper
        0x17064: 0x183E8,  # body -> same helper
        0x18444: 0xF158,   # send@plt
        0x1814C: 0x18284,  # header -> recv helper
        0x1821C: 0x18284,  # body -> same helper
        0x182E0: 0xF14C,   # recv@plt
    }.items():
        assert branch_target(address) == target, f"branch mismatch at 0x{address:x}"
    print(f"PASS pinned ELF identity, {len(words)} instruction/literal checks, 6 transport branches")
    print("Static provenance only; no vendor code loaded or executed.")


if __name__ == "__main__":
    main()
