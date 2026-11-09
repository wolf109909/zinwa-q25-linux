# ModemManager: MediaTek CCCI on Zinwa Q25

This package adds `mtk-ccci` to **stock ModemManager 1.24.2**. NetworkManager,
Plasma's mobile-network settings, `nmcli`, and `mmcli` remain the user interfaces.
There is no Q25-specific network daemon, AT proxy, or private D-Bus API.

## Requirements and build

* Build the repository's MediaTek modules against the **exact target kernel**.
  The ECCCI UART2 port must appear in `/sys/class/tty/ttyC0`; a vendor character
  device named `/dev/ttyC0` is not sufficient. Other CCCI character ports retain
  their vendor ABI. UART2 remains an AT byte stream, not a PPP transport.
* Build this directory with `makepkg` in an AArch64 Arch/CachyOS build environment
  (a separate QEMU chroot works). Install the resulting `modemmanager-q25` package
  alongside `modemmanager` and `networkmanager` in the image. Only the plugin and
  udev identification rules are packaged, not a replacement ModemManager daemon.
* This is a private ModemManager plugin ABI. The package pins ModemManager and
  libmm-glib to 1.24.2; rebuild and retest before upgrading that version.
* Install the repository rootfs modem integration: `q25-android-setup`,
  `q25-modem-ready`, the CCCI units, ModemManager's `q25-ccci.conf` drop-in,
  and the NetworkManager/udev configuration. Enable ModemManager and
  NetworkManager. Do not enable the removed `q25-modemd.service`.

Android `ccci_rpcd` must start before `ccci_mdinit`; the latter embeds the
filesystem service and owns the monitor port. They provide firmware boot/FS/RPC
only. Do not start standalone `ccci_fsd`, a legacy AT controller, or another
reader of `ttyC0`. Never truncate mapped Android libraries when updating them;
`q25-android-setup` uses atomic replacement for this reason.

## Standard connection profile

This firmware needs the APN **before network registration**, not only when a
data bearer connects. Configure NetworkManager's standard initial-EPS-bearer
settings as well as its ordinary APN. The following is an example for China
Telecom; **replace both `ctnet` values with the SIM carrier's APN**:

```sh
nmcli connection add type gsm ifname '*' con-name mobile \
    gsm.apn ctnet \
    gsm.initial-eps-bearer-configure yes gsm.initial-eps-bearer-apn ctnet \
    gsm.home-only yes ipv6.method disabled connection.autoconnect yes
nmcli connection up mobile
```

For a profile created with Plasma's mobile-network settings or another standard
GUI, set the attach fields once, then use the GUI normally to connect/disconnect:

```sh
nmcli connection modify mobile \
    gsm.initial-eps-bearer-configure yes gsm.initial-eps-bearer-apn ctnet \
    ipv6.method disabled
```

Carrier/APN selection belongs to NetworkManager and its
`mobile-broadband-provider-info` database. The plugin does not guess the APN from
an IMSI or embed a carrier default. A GUI that does not expose the initial EPS
fields still requires the one-time `nmcli` adjustment above. Roaming is governed
by the standard `gsm.home-only` / ModemManager bearer policy, not `EROAMING`.

## Protocol boundaries

* Cold `CFUN=0` leaves the SIM unavailable. Initialization changes only that
  state to `CFUN=4` (SIM available, RF off); ordinary MM enable/disable controls RF.
* Initial EPS settings and data setup share the vendor APN configuration sequence
  (`EAPNLOCK`/`EAPNSET`, `ECNCFG`, `EIAAPN`, `EIAREG`). Configuration failures
  release the APN lock. `EIAAPN?` is unsupported: MM reports only settings
  acknowledged in this daemon session and reapplies unknown settings on restart.
* `EAPNACT` activates the data bearer. An actual `+EIF` indication selects
  `ccmni{if_id % 100}`; there is no fixed `ccmni0` assumption. The plugin
  acknowledges the selected interface and obtains its DNS via `CGCONTRDP`.
  MM exposes static IPv4 /32, MTU 1400; NetworkManager owns addresses/routes/DNS.
* DNS matching tolerates MOLY's empty local-address field, but requires the
  requested APN (including an optional operator suffix). IPv6 DNS is not treated
  as IPv4. `+EIF: ...,ifdown` updates the bearer state.
* One active default data bearer is supported. IPv6-only, dual-stack host
  configuration, and PAP/CHAP credentials are rejected rather than silently
  emulated. The firmware's attach request itself uses its IPv4v6 dialect.
  Voice/IMS, suspend recovery, and modem-crash rediscovery are not certified by
  this data integration.

## Temporary IMS registration observer (default off)

`modemmanager-q25 1.24.2-2` adds a diagnostic only, **not an SMS/call fix**.
Only the exact daemon-start environment `Q25_MM_IMS_OBSERVE_ONCE=1` enables it.
With the variable absent (or any other value), no observer signal handlers,
commands or logs are installed. No D-Bus API or persistent setting is added.

At the first normal home/roaming registration with MM state at least registered,
the plugin schedules `+CIREG?` followed by `+CIREP?`, once per modem object.
Both use MM's existing best AT port/queue, a private cancellable, three-second
command timeouts, no cached replies, no raw mode and no observer retries.
The deadline is per serial transaction, not a six-second wall-clock guarantee
including queue wait. A failed/malformed first reply does not prevent the second
query, unless registration is lost, disabling/disposal begins, or the port is
unavailable. No setters, additional ports or unsolicited handlers are added.

One normal **MSG** record contains only parsed values:

```text
q25-ims-observer CIREG reporting=2 registered=1 capabilities=0x00000005; CIREP reporting=1 vops=1 vops-n3gpp=unknown
```

This is the release-2 format example, not a handset result. **Release 3 appends
one diagnostic suffix to this same single MSG record** (it does not add queries
or intermediate log records):

```text
; CIREG-diag[status=error class=mobile-equipment code=100 length=-1 prefix=-1 fields=-1 dec=-1 hex=-1 multiline=-1] CIREP-diag[status=parsed class=none code=-1 length=-1 prefix=-1 fields=-1 dec=-1 hex=-1 multiline=-1]
```

This suffix is also a synthetic example. **An anchored release-2 collector must be
updated/reviewed for the suffix before a release-3-or-later trial**; otherwise it will
silently miss the record. The live release-2 result `CIREG ... unknown` cannot
retrospectively distinguish an AT/transport error from a rejected response.

- `status`: `not-run`, `error`, `no-response`, `parsed`, or `rejected`.
- `class`: fixed `none`, `io`, `core`, `serial`, `mobile-equipment`, `message`,
  `connection`, or `other`. Only recognized domains retain a numeric error code
  in `0..65535`; `-1` means absent/unknown. No domain name or error message is
  copied into the log. Codes are MM/GLib-normalized errors, **not captured wire
  text**: for example, plain `ERROR` maps to mobile-equipment unknown (100).
- Shape fields are inspected only for a successful but rejected response:
  `length` is capped at **97** (= at least 97 bytes); `prefix` compares the exact
  expected tag after outer whitespace; `multiline` detects internal CR/LF/VT/FF.
  Wrong-prefix, multiline and oversized inputs are not token-inspected.
- `fields` is a comma-field count capped at **4** (= four or more); `dec` and
  `hex` are three-bit grammar masks for the first three fields, not their values.
  Decimal means complete unsigned uint32 decimal; hex means complete unquoted
  1–8-digit hex. Quotes, signs, `0x`, junk and overflow do not satisfy these
  predicates. Grammar alone does not establish enum values or IMS capabilities.
  `-1` means uninspected/not applicable.

**Release 4: validated first-two-field CIREG compatibility.** Parent's r4 trial
with release 3 returned a successful but rejected CIREG response: length13,
expected prefix, three comma fields, decimal/hex masks7, no internal multiline
content; CIREP parsed reporting1/VoPS1. These shape flags do **not** reveal the
handset's field values. Release 3 could reject valid enums because registration
was zero with a tail, or because the tail was zero; invalid enum values could
also reject. Capability extraction was not gated on reporting mode.

Preserved stock RIL `requestImsRegistrationState` reads the first two decimal
fields independently. Release 4 preserves known reporting0..2/registered0..1
when an optional tail is present, while still requiring a complete bounded
single response with exactly two or three fields and a **whole unquoted
1–8-digit hexadecimal tail**. Extra, empty, quoted, signed, oversized or junk
tails and unknown first-two enums still reject the entire response. The RIL's
permissive numeric-prefix tokenization is not adopted.

TS 27.007 extension information is absent when unregistered and ranges from
1..FFFFFFFF when present. Consequently an unregistered or zero tail is treated
as uninterpreted vendor padding: registration stays valid, but capabilities
remain unknown (the padding value is not retained as capabilities). Only a
registered result with a nonzero valid tail yields known capabilities, for
**any** valid reporting mode. Synthetic examples `0,0,0`, `0,1,0`, and `0,0,5`
retain their first two enums with capabilities unknown; `0,1,5` yields known
capabilities5. These are test fixtures, not inferred r4 handset values.

Release 4 keeps the release-3 diagnostic schema, two-query lifecycle and default
opt-out unchanged. This is diagnostic parser compatibility, **not an SMS
transport fix**. Missing optional fields and failed/unsupported/malformed
responses remain `unknown`, never invented zeroes.
CIREG capabilities are hexadecimal; reserved bits are retained without assigning
meaning. CIREP `vops=0` means negative **or not received**, not a conclusive lack
of network support. The two sequential samples are not simultaneous. Early IMS
non-registration does not prove IMS can never register or that SMS requires IMS.
The observer neither retries later registration nor certifies SMS routing,
delivery, calls or audio. Raw replies/error text are never included in its log;
keep normal MM logging and `log-personal-info` disabled during any trial.

Cancellation is **logical**: pending idle dispatch is removed, the consumed
one-shot is never reset, and late completions are finished but not logged or
followed by another query. Already submitted AT commands cannot be retracted.
They drain through MM's normal completion/three-second timeout with modem/port
references held by its AT task. MM 1.24.2 `mm-port-serial.c:790–796` warns that
forced in-flight cancellation can misattribute a late reply to the next command,
so this observer does not force-cancel submitted work. Ordinary MM timeout and
modem-removal behavior still have their existing serial late-response limits;
this is not hard isolation. Disable/dispose may therefore wait for normal core
completion, rather than instantly cancelling a serial transaction.

Source audit of pristine MM 1.24.2 found **no CIREG/CIREP consumers** in `src/`.
Its generic registration regexes in `mm-modem-helpers.c:855–909` match only
CREG/CGREG/CEREG/C5GREG; no existing handlers are disabled or bypassed. Parser
fixtures and a mocked-MM GObject/GTask harness test the production observer,
including opt-out, one-shot registration, cancellation before/between queries,
disable/dispose with late completions, object/port lifetime and sanitized errors.
The harness is not an end-to-end serial or handset test.

Do not enable this flag or install the package on a live phone without a reviewed
reversible window. Keep Chatty/Calls frozen and masked and transmit guards intact.
Use the verified original `1.24.2-1` plugin package for rollback, with MM stopped
during either package transaction. Remove the temporary environment override and
restart normal MM when restoring; do not replace the accepted core/libmm-glib,
change APN/RAT/CEMODE/IMS settings, thaw clients or roll back whole snapshots.

## Validation

See [VALIDATION.md](VALIDATION.md) for build fingerprints, handset results and
explicitly untested native-systemd boot behavior.

`makepkg` runs the helper and observer tests against GLib. For host-only tests,
set `MM_SOURCE` to the unpacked, pinned ModemManager 1.24.2 source tree and
`MM_BUILD` to its configured Meson build directory (for generated names headers):

```sh
cc -Wall -Wextra -Werror mm-modem-helpers-mtk-ccci.c \
    test-modem-helpers-mtk-ccci.c $(pkg-config --cflags --libs gio-2.0) \
    -o /tmp/test-modem-helpers-mtk-ccci
/tmp/test-modem-helpers-mtk-ccci
cc -Wall -Wextra -Werror -I"$MM_BUILD/include" -I"$MM_SOURCE/include" -I"$MM_SOURCE/src" \
    mm-modem-helpers-mtk-ccci.c test-ims-observer-mtk-ccci.c \
    $(pkg-config --cflags --libs gio-2.0) -o /tmp/test-ims-observer-mtk-ccci
/tmp/test-ims-observer-mtk-ccci
python3 test-q25-android-setup.py
```

On the target, check `mmcli -L`, `mmcli -m 0`, and `nmcli device status`.
Activate the profile, inspect `mmcli -b <bearer>` and NetworkManager's IP config,
then disconnect and reconnect. Confirm the dynamically selected CCMNI port
matches `+EIF`, stale addresses/routes disappear on disconnect, and HTTPS works
with `curl --interface ccmniN` (not merely over Wi-Fi). Repeat after a cold boot.
Do not publish debug logs containing SIM identifiers, phone numbers or APN
credentials. Kernel/module mismatch, chroot-only service testing, and testing
with Wi-Fi still providing the default route must not be reported as a complete
boot/network acceptance test.
