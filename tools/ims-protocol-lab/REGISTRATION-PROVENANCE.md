# Registration request: bounded inspection, not construction

**Blocker to a portable registration constructor:** the receiver requires an
already-existing **private runtime account**. Its lookup is known; account
creation, binding and lifecycle are not. The52-byte block at payload+200 also
has no established portable schema. This increment therefore provides **only
an inspector**, as approved: no encoder, template editing, account creation,
registration sequence, socket functions or backend startup.

The parent is evaluating existing open-source IMS implementations before any
further custom backend/payload work. This artifact does not initiate that work.

## Proven route

| Binary | SHA-256 |
|---|---|
| `volte_imcb` | `5ce2ac3145c5eb58421d12dc489a35d930e0055ff0c709a6378df95b5d307985` |
| `volte_ua` | `91a1143c804e84a809c6b953badacfbcb71c33571e4938599ed9661639a3c6c9` |
| `volte_stack` | `6f46ece0574eaf611ca20b1bc8707450803bcef6917f7eed59f1b8d14d493cb5` |

These are the preserved ELF32 ARM little-endian files; all addresses below are
virtual addresses equal to file offsets in their first LOAD segments.

1. IMCB sender **0x58e38** allocates256 bytes at0x58ec8/0x58ed8, marshals through
   **0x58f2c**, and submits **event11001** with declared length256 at
   **0x58eec–0x58f00**. Its allocator0x8d58/0x99e0 zeroes the allocation.
2. The actual UA **service** dispatcher compares11001 at **0x9bb4**, branches
   at0x9bc0 to **0xba54**, requires length256 at **0xba58**, and replaces the
   event ID with **91001** (literal **0xca74 = 0x16379**). It preserves length
   and payload pointer at **0xbaa4/0xbaac**, then routes via0x6c68.
3. UA **0x7000–0x7038** supplies that same payload and length to
   `volte_sip_stack_send_msg`, with Channel header ID91001 and word8=0.
   This is the previously proved12-byte Channel envelope. **This does not
   establish the incoming IMCB11001 envelope or handshake.**
4. Stack dispatcher selects91001 at **0x3f8c8–0x3f8d8**, entering **0x43234**.
   It independently requires256 bytes at **0x43248**.

Other11001 constants in UA settings/TLV code are a different namespace; they
were not used to infer registration fields. The inspector accepts either proven
**event metadata** value, preserves which was supplied, and performs no remap.

## Payload map and two-sided evidence

| Payload offset | Bytes | Inspector interpretation / evidence |
|---|---:|---|
| 0 | 4 | `account_id`, decoded LE32. Producer zero-extends a source byte at **0x58ffc**, stores a word at **0x59004**. Consumer loads the word at **0x43284** and calls account lookup at **0x43290**. |
| 4 | 4 | Opaque, preserved verbatim. No inferred enum/default constraint. |
| 8 | 64 | Raw fixed cell-ID bytes. Producer passes destination+8 at **0x59010** to `imcb_rat_get_cell_id_by_acct` (0x7aa58; name literal0xa7158), then helper **0x79378** copies64 bytes at **0x79450/0x79470**. Consumer reads payload+8 and performs a bounded64-byte `voip_strncpy` at **0x43340–0x43364**. No character encoding, terminator guarantee or cell-ID grammar is asserted. |
| 72 | 128 | Opaque, preserved verbatim. |
| 200 | 52 | Opaque **in its entirety**, including the first8 bytes. Producer copies52 bytes at **0x59018–0x59038**. Consumer passes payload+200 at **0x43514**, calls helper **0x52c28**, which conditionally copies52 bytes at **0x52c50–0x52c64**. No float/pointer/handle interpretation or normalization is implemented. |
| 252 | 4 | Opaque, preserved verbatim. |

Although this producer's account source is uint8, the consumer reads a32-bit
value and applies runtime table/context checks. The inspector preserves **all
32-bit values**, without inventing a static account enum/range or promising
that any synthetic account is usable. Nor does it reject nonzero opaque bytes
just because this particular allocation starts zeroed.

The cell-ID helper sets a byte in its **source** after copying; this is not
proof that every destination has a NUL. Tests deliberately accept all64 bytes
nonzero. The inspector never calls string routines on the field.

## Exact private-account dependency

At **0x43290**, stack calls **0x5287c**, which calls the existing-account lookup
**0x52980** at0x528a0 and checks a context-dependent association. Null results
reach the failure branch0x432a4 onward, not account creation. These are lookup
entrypoints, **not constructors**. An account-creation entrypoint was **not
established** in this bounded pass; no unavailable binary or additional
library is being requested. `_volte_sip_stack_create` must not be relabeled as
account creation merely because it creates a stack interface.

## API/test boundary

`ims_reg_request_inspect(event_id, payload, length, out)` snapshots exactly256
payload bytes into typed account metadata, raw cell-ID bytes and named opaque
regions. It has no I/O, logging, allocation, pointer conversion or wire output.
Its C result struct is **not a wire struct**. Errors leave output untouched;
unaligned input works. The supplied event metadata is outside the payload.

`reg-request-inspection.hex` is a deterministic byte-pattern fixture, with an
artificial LE32 account value; it contains no subscriber identity, real cell
location or secret and is **not a valid registration recipe**. Tests include all
256 truncated prefixes, extra bytes, unknown event IDs, unaligned input,
unchanged output on failure, opaque preservation, and inconsistent Channel
lengths for the specifically proved UA-to-stack91001 route. No incoming11001
Channel envelope is fabricated.

Run normal/sanitizer tests with `make check` / `make check-sanitize` in this
directory. Optional read-only provenance verification:

```sh
make verify-reg-provenance \
  STOCK_BIN_DIR=/home/maxsun/q25-work/ims-stock-complete/bin
```

The checker pins all three identities,27 instruction/literal words and13 branch
destinations. It reads the archived binaries without loading/executing them.
No further reverse engineering is scheduled by this increment.
