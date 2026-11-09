# Linked native stack: installed, radio backend not started

Six additional local packages were built and installed in one transaction:

- `ofono-q25-native 1.29.r45d1d49-1`
- `libandroid-properties-q25-api30 0.1.0.r7079712.api30-1`
- `ofono-binder-plugin-q25-smsc-hex 1.1.28.q25smschex1-1`
- `ofono-binder-plugin-ext-mtk-q25-smsc-hex 0.7.0.r00f9b3e.q25smschex1-1`
- `libmce-glib 1.1.0-1`
- `mce-dev-q25 1.36.0.r5973d5d-1`

Artifacts/source bundles/recipes and pins are in `~/q25-work/ims-reuse/native-packages/stack-artifacts/`, `stack-recipes/`, `stack-sources.lock.json` and `stack-inputs.lock.json`. Actual source-built property provider includes real libhybris common and Q linker; no fabricated property symbols. Parent verified artifact hashes, actual file-list absence of startup units/activation/hooks, native-symbol closure and policy tests. Neither symbol closure nor successful linking proves runtime Android compatibility.

## On-device checks

- `ofonod --version` as `max` returns `1.29`. Source review confirmed this exits before D-Bus/plugin/radio initialization.
- Native ELF loader resolves the daemon's dependencies.
- ModemManager, NetworkManager, Phosh and sshd remain active with unchanged PIDs; no persistent ofonod process was started.
- Packages remain installed per user preference. No activation/configuration files were supplied.
- Transaction installed2.81MiB and created ordinary pre/post snapshots68/69. Observed count afterward:24root +4home =28; no new manual/home snapshot. Retained pair57/home332 remains available. Exact core/extension dependency pairing is required.

## Initial runtime blocker (crashes subsequently fixed)

An isolated read-only test `/usr/local/libexec/q25-property-read-smoke` calls only public `ro.build.version.sdk` via the actual property library. With process-local SDK30 selection and default paths, native library loading succeeds but `libcutils.so` is absent, and fallback returns unavailable. Supplying explicit preserved Android bootstrap/system/vendor library paths caused this test process to segfault. No radio daemon or modem operation was involved; core services remained unchanged.

Public stock build properties establish **system SDK34/Android14; vendor SDK31/VNDK31**. The package's API30 headers are only a compile baseline. A loader/runtime defect is under investigation; SDK mismatch alone is not a diagnosed cause. Parent's probe source and evidence are in `native-packages/live-stack/`; the isolated test core is PID725540.

## Installed fixes and current IPC blocker

Provider release2 adds the native `__ctype_get_mb_cur_max` locale hook and keeps Bionic libcutils loaded with `RTLD_NODELETE`. The first fixes the Bionic/glibc TLS constructor fault; the second fixes a static-TLS unload abort. MTK extension release2 is a metadata-only companion pinning that provider; its binary payload is unchanged. Both remain installed, with old archives retained. Native public SDK read using the upstream file-backed path returns34; real Bionic-path read returns unavailable cleanly. This is not a live property store.

The stock hardware service manager then failed its mandatory `getcon` because the running kernel has no SELinux. An existing Hybris SELinux-disabled compatibility library, applied only to an isolated manager, allowed initialization and context-manager registration. It explicitly disables Android label checks; dummy contexts are not security labels. No global preload or kernel/security-policy change was made.

However, standard handle-zero HIDL ping still failed: the kernel returned `EOPNOTSUPP` trying to provide a requested transaction security context. Android14 libhwbinder requests `FLAT_BINDER_FLAG_TXN_SECURITY_CTX`, while this kernel has no context-providing LSM. Manager readiness property alone therefore does not prove RPC readiness. A narrowly scoped context-registration compatibility trial is next; exported-object SID requests remain a separate boundary.

All manager trials have stopped, with no hwbinder holder or RIL/oFono process left. The latest recorded count at2026-11-09T20:26:34Z is23root +4home =27, reflecting automatic history changes; no manual snapshot was added for these probes. Evidence: `native-packages/live-stack/provider-rel2/` and `selinux-disabled-trial/`.

Do not start vendor RIL or oFono radio control alongside MM. Radio ownership, property/runtime integration and protections for the alternate backend remain unresolved. MM-specific no-transmit rules do not automatically protect oFono/Binder. SMS/calls/audio have not been validated, and these installations do not fix the reported `No network` failure.
