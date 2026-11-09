# Channel envelope provenance

Source: parent-preserved **ARM32 Android little-endian**
`libvolte_core_shr.so`, SHA-256:

```text
0a4db679d65bdad2a098ecf752cde88c50907f8682a3d0fbfc47006a2a8e752f
```

No vendor executable/library was loaded, linked into tests or executed. Evidence
was read with `readelf` and GNU `aarch64-linux-gnu-objdump` (which also supports
ELF32 ARM). No SIP engine reversal was required: all framing helpers below are
in this one DSO. No further dependency is needed for this codec.

## Entry → memory descriptor → raw stream

All addresses are ELF virtual addresses, equal to file offsets within the first
LOAD segment (VA0/file0 through0x7d7d6).

| Site | Observed operation | Codec consequence |
|---|---|---|
| `volte_service_send_msg_by_channel` 0x66808–0x66978 | Builds a zeroed 16-byte stack descriptor; argument1 stored at descriptor+0 (0x668b0), argument2 at+4 (0x668b8), zero at+8 (0x668c0), argument3/payload pointer at+12 (0x668c8). Calls `Channel_Send` at0x668d4. | Service header has three scalar words; word8 is zero in this wrapper. |
| `volte_sip_stack_send_msg` 0x6b2c4–0x6b440 | Copies supplied descriptor words at offsets0/4/8/12, then calls `Channel_Send` at0x6b39c. `send_msg_by_channel` 0x6b440 does the same. | Channel transport does not require word8=0. Preserve it without guessing flags/account/pointer semantics. |
| `Channel_Send` 0x16d64; validation helper0x16eb0 | Length at descriptor+4; zero accepted at0x16eec. Nonzero must be nonnegative (0x16efc) and ≤literal0x7ffffffe at0x16fe8; nonzero payload requires a pointer at+12 (0x16f78). | Length interval0..0x7ffffffe, not an assumed 256-byte limit. |
| Writer0x17004 | Loads write length12 at0x17010 and calls raw send helper at0x1702c. Then loads pointer from descriptor+12 at0x17058 and length from+4 at0x17060, calls the same helper at0x17064. Zero body skips this second write. | **Exactly12 header bytes, then payload length bytes. Pointer is NOT sent.** |
| Raw sender0x183e8–0x1852c | Passes supplied bytes directly to `send@plt` at0x18444, flags0; advances pointer/count after partial writes. | No endian conversion, packing transform or extra envelope in this path. ELF is little-endian, hence fixed LE scalar encoding. |

`Channel_Send` also serializes writes under its channel mutex; this does not
make separate stream writes into datagrams. The lab deliberately has no
transport/threading implementation.

## Independent receive-side confirmation

Reader helper **0x18114–0x18284**:

- Requests12 bytes at0x18120/0x1814c and requires exactly12 at0x18158.
- Reads payload length at descriptor+4 (0x18164); zero returns after header.
- Rejects signed negative values at0x1817c and values above **0x7ffffffe**
  (literal0x18274, comparison0x1818c).
- Allocates `length+1` locally (0x181f8), stores its local pointer at+12
  (0x18204), but receives only `length` bytes at0x1821c. The extra allocation
  byte is **not a wire terminator**.
- Raw receiver0x18284 calls `recv@plt` at0x182e0 with flags0 and no byte-order
  transformation. Successful body count is increased by12 at0x18260.

The codec preserves those proven field/length rules. Its exact-span rejection
of trailing bytes and allocation-free borrowed view are explicit **lab API
choices**, not claims that a stream peer rejects subsequent frames. It does
not reproduce vendor allocation/short-read quirks or infer safe live resource
limits from the large validation ceiling.

## Socket context, not implemented

`Channel_Create` calls `socket_local_client` at0x15e28 with namespace1/type1.
`socket_local_client` 0x72a40 calls `socket(AF_UNIX, type, 0)` at0x72a70.
`socket_make_sockaddr_un` 0x728b4 selects namespace1 at0x728e4 and constructs
`/dev/socket/` plus the supplied name at0x7294c–0x72994. This proves a local
**SOCK_STREAM** transport for that caller, not any authorization to connect.
Endpoint/handshake/routing contracts and payload definitions remain out of scope.

## Reproduction

The repository does not contain proprietary binary copies. Against the local
preserved file, the relevant disassembly ranges are:

```text
0x66808..0x66978  service wrapper
0x6b2c4..0x6b55c SIP message wrappers (not SIP engine)
0x16d64..0x170a0 send validation and frame writer
0x18114..0x1852c frame reader and raw recv/send loops
0x728b4..0x72af4 AF_UNIX pathname/client helpers
```

Use `aarch64-linux-gnu-objdump -d --start-address=... --stop-address=...` and
`readelf -hW -lW -Ws` with that file. The optional `verify_provenance.py` pins its
identity and independently checks21 instruction/literal words and6 branch
destinations. It is static validation, not a vendor conformance/runtime test.

## Remaining unknowns

- Message IDs and opaque word8 meanings; no registration/control permission map.
- Complete payload schemas. The bounded account/cell-byte inspection of the
  256-byte registration request is now documented in
  [REGISTRATION-PROVENANCE.md](REGISTRATION-PROVENANCE.md); its opaque regions
  and private-account setup remain unresolved.
- Cross-process handles/pointers inside any particular payload, handshake,
  readiness ordering, response correlation and service lifecycle.

Resolving those is separate from this completed transport-frame codec and its
inspection-only companion. Nothing here synthesizes an IMS registration
request or enables a live backend.
