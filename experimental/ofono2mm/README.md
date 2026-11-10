# Inert oFono2MM integration candidate

**Offline-tested source, not a working Q25 telephony stack. Not installed on the phone.** This directory is deliberately outside `packages/` and the image build. Do not enable it alongside native ModemManager: both would own `org.freedesktop.ModemManager1`.

Upstream: FuriLabs `forky`, commit `82d43ea178977ee2bf90baa202720413d39a691e`. The native Message API was checked against Furi oFono `45d1d49ac2ec3aa011761e46b0c53cc1933cf8ad`. This adapts the existing bridge; it does not implement an SMS protocol or IMS stack.

## Seven bounded patches

1. `Messaging.Create` allocates a draft; only explicit `Sms.Send` submits it. Uncertain outcomes cannot trigger an automatic bridge retry.
2. Opt-in `--diagnostic` suppresses implicit radio/data activation, NM connection creation and automatic reconnect paths. **It is not an authorization guard.** Explicit mutators remain capable of changing the backend.
3. `Voice.CreateCall` allocates a draft; only `Call.Start` dials. Calls are correlated by their actual returned native path, not by selecting the last/unrelated call.
4. Sensitive SMS bodies, recipient/caller numbers and DTMF values are excluded from the changed logging paths, including verbose mode.
5. SMS submission state follows native signals. Positive native `sent` means submitted, **not recipient delivery**. Missing/uncertain outcomes stay UNKNOWN; DeliveryState stays UNKNOWN. No queue or history enumeration.
6. Call removal is idempotent if native CallRemoved arrives during Hangup. Create input Variants are validated before allocation/submission.
7. SMS observers retire automatically after completion/removal or authenticated native owner loss. Submission is pinned to the captured unique owner. No observer reattachment, queue replay or resend occurs after an uncertain result.

Detailed native-source semantics, races and limitations: [SMS-STATE.md](SMS-STATE.md), [SMS-LIFECYCLE.md](SMS-LIFECYCLE.md).

## Evidence

The worker passed46 private-bus tests against the working, freshly patched and packaged trees. Parent independently reran all46 against an extracted pkgrel3 package, then audited the package:33 non-executable source/license files, **no launcher, service, activation, policy, configuration or install hook**. Actual broker MatchRules and handler counts returned to baseline after48 synthetic completed SMS retained without Delete. All modem/send/dial endpoints in these tests are synthetic; no phone, real radio, host system/session bus or private queue was used.

The first parent packaged-tree invocation pointed one directory too high and failed importing `main`; correcting `OFONO2MM_SOURCE` to the package's `/usr/lib/ofono2mm` root passed. This was a test invocation error, not an installed runtime test.

Archived source bundle SHA-256:

```
440574a9cee56fed13bf092bff517b1f16b024de113e49bddd5feb7295bf1139  ofono2mm-furilabs-inert-1.24.0.r82d43ea-3.src.tar.gz
```

The tested binary package (retained in host scratch, not committed/installed) has SHA-256 `eab34fce45b7a3edaa9cfac3949063ffd163d90c3d8218eb0f4d69c088db9967`. `SHA256SUMS` covers the reviewable recipe, patches, test and source archives. `makepkg --verifysource --nodeps` passed after repository archival.

## Reproduction (isolated host only)

Use the recipe's `check()` with installed standard test dependencies, or explicitly set:

```
OFONO2MM_SOURCE=/path/to/patched/upstream/tree python test_contract.py
# For an extracted package, use /path/to/extraction/usr/lib/ofono2mm instead.
```

The test's historical scratch-directory default is not a repository-relative source location: always set the variable. Tests use actual dbus-fast decorators/methods/signals on a private non-activating daemon. `python-dbus-fast`, `python-dbus`, Python GObject, Geoclue typelibs and `dbus-daemon` are required. No deployment command is provided here.

## Gates still open

- Exact stock Android runtime/loader closure and native RIL/HAL/IMS integration are not established. Stock public loader files must be collected after renewed phone access; do not substitute arbitrary firmware.
- Native oFono may restore/retry its own persistent queues. Blocking bridge Send alone is insufficient. Plan a fresh private native state directory and protect direct oFono/Binder interfaces before any live ownership change.
- Diagnostic mode does not block explicit user-facing mutators. Existing MM-only guards do not cover every backend interface.
- Delivery reports, durable deduplication, silent per-modem teardown, Chatty/Calls/NetworkManager interoperability, native ABI, IMS registration and call audio are not validated by these host tests.
- No call/SMS transmission or queued retry is authorized. End-to-end acceptance needs separate user-assisted permission.
- Preserve native ModemManager as the fallback and re-establish the actual post-reboot safety baseline before any hotel trial. Never reuse old process IDs/runtime freezes or assume the phone's installed module matches a host archive.
