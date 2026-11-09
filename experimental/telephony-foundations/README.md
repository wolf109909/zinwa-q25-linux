# Inactive native telephony foundations

These are self-contained **source** bundles for four independently reviewed ARM64 library packages. They are deliberately outside `packages/` and **not included in build-all**. After explicit user authorization, the four library packages were installed on the phone and passed the native smoke test below. They do not provide a working IMS backend.

| Package | Version |
|---|---|
| libglibutil | 1.0.83-1 |
| libgbinder | 1.1.54-1 |
| libgbinder-radio | 1.6.6-1 |
| libofonobinderpluginext-q25-smsc-hex | 1.1.28.q25smschex1-1 |

`sources/` contains makepkg source bundles, each including its pinned upstream archive, PKGBUILD, metadata/provenance, licenses and any required patch. Extract each into a **separate** build directory. Building requires a matching ARM64 target environment and the dependencies declared in that bundle; this directory is not a standalone cross-toolchain installer.

`ARTIFACT-SHA256SUMS` records the original four unsigned binary review artifacts and four source bundles. Only the source bundles are committed here. Original binaries and isolated cross-build/test scripts are under `~/q25-work/ims-reuse/native-packages/` on the build host. `sources.lock.json` and `inputs.lock.json` record source pins, pairing and authenticated ALARM build-input snapshot. Public source/signature verification is not a guarantee of runtime compatibility or current revocation status.

## Independently verified

- All eight artifact hashes; actual binary archive members contained only libraries, headers, pkg-config data, documentation/licenses and package metadata.
- Installed library payloads were ELF64/AArch64, with no startup units, D-Bus activation, presets, install hooks, live configuration or daemon executables.
- ARM64/QEMU synthetic link/smoke test against the four actual libraries, five production SMSC-helper tests and three package-policy tests passed. No Binder connection/vendor loading was performed.

The API package includes the experimental `SMSC_HEX=0x04` header. It **does not contain the patched Binder daemon plugin or MTK extension**. Exact core/extension version pairing is still required; generic SONAME/API compatibility is insufficient. See `patches/ofono-mtk-experimental/README.md`.

## Native installation result

The four listed libraries were installed in one ordinary pacman transaction (0.90 MiB total installed size), after verifying matching target GLib/glibc and existing recovery pair57/home332. `/usr/local/libexec/q25-foundations-smoke` passed natively as user `max`, without creating a Binder connection. Libraries and the test remain installed for continued testing. ModemManager, NetworkManager, Phosh and sshd stayed active with unchanged PIDs.

Normal package pre/post snapshots66/67 were created; no manual home snapshot was added. Observed total afterward:22 root +4 home =26. Ordinary automatic history remains count-bounded. Remove only these four packages together with `pacman -R` if a rollback is required and nothing later depends on them; retain package archives under `/var/cache/q25-telephony-foundations`. Do not use recursive dependency removal indiscriminately.

## Not validated

No vendor RIL/oFono service startup, ownership transition, registration, message/call or audio test. Existing ModemManager data/control remains in place; its transmit guards do not protect an alternative backend. Native oFono, the patched core and MTK extension, real Android property provider, vendor runtime namespaces and single-owner data/client integration remain separate work.
