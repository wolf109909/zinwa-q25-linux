# IMS protocol lab — offline Channel frame codec

**Not a live IMS backend.** This library only encodes/decodes byte buffers for
one proven `libvolte_core_shr.so` Channel envelope. It opens no socket, sends
nothing, loads no vendor code, and implements no registration sequence. Message
IDs, the third header word and payload schemas remain opaque. In particular,
there is **no interpretation of the previously observed 256-byte registration
payload** and no fabricated registration fixture.

## Proven wire format

For the pinned ARM32 little-endian library in [PROVENANCE.md](PROVENANCE.md):

| Offset | Bytes | Representation |
|---|---:|---|
| 0 | 4 | Message ID, little-endian bit pattern; semantics unvalidated |
| 4 | 4 | Payload byte length, little-endian, valid 0..0x7ffffffe |
| 8 | 4 | Opaque word8; service sender writes zero, SIP-message wrapper preserves it |
| 12 | length | Opaque payload bytes |

The vendor's **16-byte in-memory structure is not a 16-byte wire header**.
Its pointer at memory offset12 is omitted; the pointed-to bytes follow the
12-byte header. No host pointer, implicit padding or terminating NUL is encoded.
The examined transport writes header/body unchanged to an AF_UNIX stream.
There is no additional per-frame magic/version/checksum in that send path.

The large length ceiling comes from both vendor validation paths. It is **not
an allocation budget or a recommended live-message size**. This library never
allocates: encoding checks the caller's output capacity before copying;
decoding requires an already-present complete input span. Boundary tests do not
allocate gigabytes. A future transport needs its own justified resource budget.

## Build/test (host only)

```sh
make -C tools/ims-protocol-lab check
make -C tools/ims-protocol-lab check-sanitize
```

Requires a C11 compiler, libc and `ar`; no Android/device libraries. Generated
files stay in ignored `build/`. `make all` builds `build/libims_channel_frame.a`.

The optional provenance check reads a **local archived file**, not a device, and
never uses `dlopen` or executes the binary:

```sh
make -C tools/ims-protocol-lab verify-provenance \
  VENDOR_LIB=/home/maxsun/q25-work/ims-stock-complete/lib/libvolte_core_shr.so
```

Normal tests do not require that file or Python. The optional verifier uses only
Python's standard library, checks the full SHA-256 first, then ELF identity,
selected instruction words, length constants and direct branch destinations.

## API limits

`ims_channel_encode()` writes one frame to caller-owned storage;
`ims_channel_decode()` accepts **exactly one complete frame**. They return
explicit errors, leaving outputs untouched on failure. Unaligned buffers work;
serialization never depends on native struct layout, pointer size or host
endianness. Inputs/output storage must not overlap as documented in the header.

Successful decode returns a **borrowed** payload view into the caller's input
(NULL for empty payload); retain that buffer while using the view. No pointer
from the wire is dereferenced. Header IDs/word8 are not trusted event types or
permissions merely because framing validates.

This is not an incremental stream reassembler: truncated prefixes fail, and
concatenated frames/trailing bytes fail. A future stream layer must delimit
frames and apply message-specific bounds; socket connection, handshake,
endpoint selection, authentication, IPC routing and callbacks are absent.
Other CCCI/modem transports are not covered.

## Synthetic fixtures/tests

All `.hex` fixtures are manually specified, artificial bytes—not captures:

- `empty.hex`: arbitrary ID0x01020304, length0, word8=0.
- `service-binary.hex`: arbitrary ID0x89abcdef, word8=0, four binary bytes.
- `opaque-word8.hex`: arbitrary ID0x11223344 and nonzero word8, three binary bytes.

Tests assert exact encoded bytes, opaque-word preservation, unaligned I/O,
zero-length behavior, every truncation of a fixture, extra/concatenated bytes,
invalid signed/oversized lengths, source length ceiling without allocation,
undersized output and unchanged failure outputs, NULL arguments, and 513
synthetic round trips for lengths0..512. These establish codec behavior, **not
interoperability with running IMS services or successful registration**.
