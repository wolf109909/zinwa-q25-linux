# Native Q25 telephony investigation — 2026-11-09

**SMS and calls are not fixed or end-to-end validated.** No investigator call, SMS, USSD, queued retry or private-message inspection was performed. Chatty/Calls remain frozen/masked and transmission guards remain in place.

## Established observations

- The user's prior Chatty send reached ModemManager and failed `No network`. No raw CMS code was captured; source-derived331 is not wire evidence.
- The deployed core CPMS whitespace repair addresses storage parsing, not SMS transport.
- One normal registration transition showed CS denied, PS/EPS home, consolidated home. This is a transition sample, not continuous CS denial; LTE data does not establish voice/SMS.
- An opt-in, MM-owned read-only observer queries only `CIREG?` and `CIREP?` once at first home/roaming registration. Default off; no unrestricted command API or competing serial reader. Tests cover strict parsing, safe scalar diagnostics, cancellation/lifetimes and no rearming. It is diagnostic code, not telephony enablement.
- Release3 showed a successful CIREG command with three numeric fields rejected by the original optional-extension parser. Stock RIL reads the first two fields. Release4 retains strictly validated first-two enums with a syntactically valid hex tail, but treats unregistered/zero padding as unknown capabilities.
- Actual release4/r5 observation parsed **reporting0, IMS registered0, capabilitiesunknown**; CIREP **reporting1, VoPS1, optional non3GPPunknown**. Both commands parsed successfully. This proves unregistered IMS at that sampled point, NOT throughout the30-second window or indefinitely. VoPS support is not subscriber registration, SMS support or delivery.

## Device restoration

The final r5 trial automatically restored original `modemmanager-q251.24.2-1`, accepted core1.24.2-2.1 and unchanged libmm-glib. WWANoff, MSG logging and low-power state restored. Exact before/after public metadata and non-MM state matched, including service/client PIDs, Wi-Fi/USB/VPN and35snapshot IDs. No snapshot created by trial. Subsequent reviewed cleanup reduced recovery points to25; see `recovery-retention.md`.

Original plugin SHA256: `60973f1a85f2a9444c72c72e21bd67b9f9ae1b1f5522769084a4acfb153c691e`.

Final diagnostic package SHA256: `9517a5a03d4bee74aaa2dec3e43d312837ea27bdbd3f5526ec1e7ef56a070047` (temporary trial only, not left installed).

Host evidence: `~/q25-work/ims-observer-runner-r5/live-receipts.log`; earlier attempts/results in separate `ims-observer-runner-r2/r3/r4` directories. Two earlier pre-transaction aborts needed restoration-only recovery; they obtained no IMS observation. Preserve that distinction rather than treating every runner failure as a modem failure.

## Stock registration ownership established

Static analysis of the exact stock `volte_md_status`, `volte_imcb` and `volte_stack` binaries found no modem-auto bypass in the status watcher. The ready event starts the host IMS pipeline. IMCB sends a registration request toward UA; `volte_stack` constructs SIP REGISTER and invokes the host SIP transport. The demonstrated stock route therefore requires host IMS integration, not just an AT initialization toggle. This does not prove no hypothetical alternate firmware interface exists.

The concrete integration requirements are lifecycle handling, registration-control/account IPC, IMS PDN/P-CSCF discovery, SIM/AKA authentication, and the host SIP/security/retry backend. Existing `md_auto_setup_ims=1` property emulation does not implement Android init triggers or that backend. Native APN initialization also lacks the stock IMS profile/policy path, but no reversible setter contract is yet established.

The first offline bridge component is in `tools/ims-protocol-lab/`: a C11 codec for the statically verified 12-byte little-endian Channel envelope. It performs memory-only encode/decode, uses synthetic tests, and has no socket, registration sequence or vendor-code execution. Payload schemas and authentication/lifecycle remain unimplemented. A framing test is not evidence of telephony operation.

Do not force APN, CEMODE or IMS setters, launch Android daemons, or claim missing IMS alone proves the SMS failure cause. Call-audio routing remains unverified. End-to-end acceptance requires separate user-authorized tests with a consenting peer.
