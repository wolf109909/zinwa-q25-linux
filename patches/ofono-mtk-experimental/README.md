# Experimental, inactive oFono/MediaTek SMS patches

**Not installed, not wired into the Q25 image build, not a telephony solution.** Current phone modem ownership stays with ModemManager. No calls/messages were sent in this work. Loading oFono/vendor RIL would require exclusive modem ownership, vendor runtime integration and a new safety policy; current MM guards do not cover those services.

## Pinned sources and order

- UBports MTK extension: https://gitlab.com/ubports/development/core/hybris-support/ofono-binder-plugin-ext-mtk at `00f9b3e161d463b67a8437430802b20298c3eb6a`.
- FuriLabs Binder core: https://github.com/FuriLabs/ofono-binder-plugin at `2c1585a4325c15de79af61af84c2ce7ecb39c679`.
- oFono API/header reference: https://github.com/FuriLabs/ofono at `45d1d49ac2ec3aa011761e46b0c53cc1933cf8ad`.

Apply `0001` and then `0003` to the extension. Apply `0002` to the separate Binder core. Do not apply all three to one source tree.

`0001` preserves the already-separated TPDU, selects 3GPP for GSM messages, validates bounded inputs and removes raw SMS dumps in this extension. Alone it rejects non-default SMSC; that is experimental behavior, not a no-send guard (ordinary core fallback still exists).

`0002` introduces an explicit opt-in SMSC-hex contract before binary SMSC bytes lose embedded NULs in a lengthless C-string API. `0003` opts the MTK extension into that contract. The private flag is not an upstream-released ABI allocation. **Explicit SMSC requires the patched pair; arbitrary existing plugin/core versions are not supported.** Other ordinary-radio SMS marshalling defects, ACK/cancel/incoming delivery, fallback/retries and call/audio gaps remain unresolved.

## Contract and validation

SMSC binary length octet is included when encoded to hex. Default SMSC maps to empty/null. TPDU bytes and length are independent of SMSC string length. Evidence includes pinned oFono `smsutil.c` and AOSP Android12 telephony `GsmSMSDispatcher.java`/`RIL.java`, plus `hardware/interfaces/radio/1.0/types.hal`.

Parent independently verified all patch hashes, reran five core and five paired tests with ASan/UBSan, and compiled seven changed production objects. The tests exercise production helpers/writers with synthetic writer-call capture, not real Binder traffic. No Binder/network runtime is linked in those tests; no live HAL, linked plugin or complete daemon was tested.

Tests are included in the patches: Binder core `unit/unit_sms_pdu`, extension `tests`. The paired extension test must be passed the patched core source (`BINDER_CORE=...`). Exact scratch reproduction and logs: `~/q25-work/ims-reuse/sms-contract-evidence/commands.sh` and neighboring test logs. Private vendor binaries are not included.

These source fixes are unrelated to the current ModemManager `No network` error unless and until a separately validated backend transition is made. See `docs/telephony-progress.md` and `docs/recovery-retention.md`.
