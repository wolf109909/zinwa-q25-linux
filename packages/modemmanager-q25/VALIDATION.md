# Validation record — 2026-11-08 UTC

## Build and automated checks

* C helper tests: **4/4 passed** (`eif`, `dns`, `apn`,
  `dns-no-local-address`), both on the host and in the AArch64 package build.
* `test-q25-android-setup.py`: **2/2 passed**. An existing mapped library and
  its pathname remain valid during replacement; symlinks are copied intact.
* Shell syntax checks and `git diff --check`: passed.
* Clean AArch64 package build (no incremental build dependency): passed.
  Exact command on this machine, with the PKGBUILD/source files already copied
  to the builder directory and dependencies installed:

  ```sh
  sudo arch-chroot /home/maxsun/q25-work/mm-buildroot \
      runuser -u builder -- bash -c \
      'cd /home/builder/modemmanager-q25 && makepkg --noconfirm -Cf'
  ```

  On a native AArch64 Arch/CachyOS builder, run `makepkg --syncdeps --cleanbuild`
  in this package directory. Build dependencies are declared in `PKGBUILD`.
  Install the resulting package with `pacman -U`; the target also needs the
  repository's kernel module and rootfs modem integration.
* Package contains only the plugin and udev rule (plus package metadata).
  `readelf -d` confirms no RPATH/RUNPATH. The clean-built plugin's SHA-256
  **matches the plugin tested on the phone**:

  ```text
  plugin: 60973f1a85f2a9444c72c72e21bd67b9f9ae1b1f5522769084a4acfb153c691e
  package: ada33171becc54f73c3394a746279d2394c9b2795cdafd28c005bae66cea4755
  ```

  Copied package artifact:
  `/home/maxsun/q25-work/packages/modemmanager-q25-1.24.2-1-aarch64.pkg.tar.xz`.
* The matching live-kernel module was built from an isolated copy of the driver
  sources, **not** the shared integration build output:

  ```sh
  make -C /home/maxsun/q25-work/linux-7.3-running \
      M=/home/maxsun/q25-work/modem-test-drivers/mediatek \
      ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j16 modules
  ```

  The output `eccci/ccci_md_all.ko` has vermagic
  `7.3.3-cachyos-q25-00024-ge50c42ce375d-dirty SMP preempt mod_unload aarch64`.
  Its SHA-256, also verified against the phone, is
  `fe1709eecb9c062bf24ce0acbeb50cf8aa3c7e4c2fda81d7c886eb6ea7d59992`.
  Unrelated pre-existing module-description warnings remain.

## Live handset checks

Target: Q25, matching kernel above, stock Arch Linux ARM
`modemmanager 1.24.2-2`, NetworkManager 1.58.1, MOLY.LR13.R2.MP.V85.3.
Testing used the existing legacy `/rootfs` chroot. No raw boot partition was
written. The old modem module backup remains under
`/rootfs/root/q25-modem-backup` on the handset.

1. `/sys/class/tty/ttyC0` exists beneath the modem platform device;
   `tcgetattr`, polling/reading, and finally `stty -F /dev/ttyC0 -a` work.
   Firmware reaches `md1:4` after reboot. Vendor RPC/MD-init daemons still run.
2. After firmware readiness, stock MM with the plugin discovers modem 0;
   SIM is ready, registration is `home`, packet service `attached`, signal
   quality updates, and NM exposes `ttyC0` as a GSM device.
3. Standard NM profile `q25-lte-test` with APN `ctnet`,
   `gsm.initial-eps-bearer-configure=yes`,
   `gsm.initial-eps-bearer-apn=ctnet`, IPv4 and no roaming activates.
   Logs show `EIAREG=1`, `EAPNACT`, actual EIF parsing/acknowledgement and DNS
   extraction. MM/NM expose IPv4 **/32**, MTU **1400**, and carrier DNS
   `218.2.2.2`, `218.4.4.4`.
4. `nmcli con down` removes the bearer address and routes. Reconnection maps
   EIF `201` to `ccmni1`, then EIF `302` to `ccmni2`; it is not fixed at ccmni0.
5. `curl -4 --noproxy '*' --interface ccmniN --fail -I
   https://www.baidu.com` returns **HTTP 200 with TLS verification**.
   This also passed with Wi-Fi disabled: `wlan0` was unavailable and the sole
   default route was `default dev ccmni2 ... metric 700`. USB provided only a
   local management subnet, not an Internet default route. Wi-Fi was restored.
6. With the NM connection down, `mmcli -m 0 --command='AT+CFUN=1,1'` in MM
   debug mode returned OK, produced a real TTY hangup, and restarted MD1.
   After `q25-modem-ready` succeeded, restarting stock MM, then NM activation,
   restored registration and **HTTP 200 over ccmni0** with a fresh IPv4 /32.
   This tests a controlled firmware reset **plus an explicit MM restart**,
   not unattended modem-crash recovery.
7. Final state: MD1 READY; MM connected/registered/attached; NM and Wi-Fi
   running; `q25-lte-test` connected and autoconnect enabled. Wi-Fi default
   metric 600, LTE default metric 700. No stopped/suspended NM process remains.

## Failures found and resolved during validation

* Shared module output was overwritten by a build for a different kernel
  release. Boot rejected that module. Isolating **both** the kernel tree and
  module output directory, checking vermagic, and replacing the module fixed it.
  Do not install this live-test binary into an image with a different kernel.
* Overriding initial EPS settings initially left MM's inherited CGDCONT-CID
  bearer loader enabled, which asserted. CCCI now disables that incompatible
  loader and uses its own EIAAPN settings path.
* The legacy boot script regenerated its private APN file and left a stale
  external ccmni0 IPv4 /24 and metric-100 route. This contaminated the first
  reset test. Those runtime addresses/routes were removed; a second reset
  starting from clean MM/NM-owned state passed without manual link repair.
  The new rootfs must not start that removed legacy controller.

## Explicitly untested / limitations

* **Native-systemd cold-boot automatic discovery and reboot autoconnect:
  UNTESTED.** The live legacy chroot does not execute the new MM service
  drop-in. Per supervisor direction, no legacy startup harness was added.
  Validate the new rootfs's CCCI ordering and ModemManager enablement on target.
* Automatic recovery without restarting MM after a modem crash/reset, suspend
  and resume, roaming, another SIM/carrier, voice/IMS and IPv6 are untested.
* One default IPv4 bearer, no PAP/CHAP credentials. Unsupported settings are
  rejected. No carrier APN is hardcoded in the plugin.
* Plasma GUI activation was not exercised; GUI-created profiles need the
  standard initial-EPS fields set once, as documented in `README.md`.
* The plugin uses MM's private ABI and is pinned to 1.24.2. Version upgrades
  need a rebuild and retest. Firmware cannot read back EIAAPN settings; only
  successfully configured settings in the current daemon session are cached.

Host build logs remain in `/tmp/q25-mm/`; live debug logs are under the
handset's `/rootfs/var/log/ModemManager-q25*.log`, and the Wi-Fi-off test log is
`/rootfs/root/q25-mm/lte-only-test.log`. These logs can include SIM identifiers
and must not be published without redaction.

## Offline IMS observer candidate — 2026-11-09 UTC

**Built, not installed or handset-tested.** This opt-in diagnostic does not fix
SMS/calls. The historical live checks above describe the unchanged `1.24.2-1`
plugin, not this candidate. No device command, MM restart, RF enable or snapshot
was performed during this implementation phase.

- Host helper tests: **6/6** (four existing tests plus CIREG/CIREP fixtures).
- Production observer with mocked MM AT boundary and real GObject/GTask behavior:
  **14/14**, including no registration, opt-out, one-shot, pre-enqueue/between-query
  cancellation, disable/dispose in both transactions, late-result suppression,
  lifetime, timeout/malformed results and no retry. ASan/UBSan with leak detection:
  **14/14**, no diagnostics. Also compiled with GLib 2.56 API limits and `-Werror`.
- Existing Android-setup tests: **2/2**; their code is unchanged.
- Clean aarch64 `makepkg --noconfirm -Cf`: **6/6 + 14/14**, final build without
  compiler warnings. Isolated builder directory:
  `/home/builder/modemmanager-q25-ims-27074db8` in the existing host buildroot.
  Original builder directory/package were not overwritten; chroot mounts exited.
- Package contains only the plugin and unchanged udev rule, with no RPATH/RUNPATH.
  `.BUILDINFO` recipe hash matches the repository PKGBUILD. The core, libmm-glib,
  installed service configuration and unrestricted Command API are unchanged.

Final candidate in `/home/maxsun/q25-work/ims-observer-27074db8/`:

```text
modemmanager-q25-1.24.2-2-aarch64.pkg.tar.xz
SHA-256: 58faf78ed188b50b70e737bba0f3f49dc4378c44258e09ed8313ec2f8369324b
plugin SHA-256: b330efda374aaa4d304984fcb7f2be1c42fd6304008541c02ad04f2c4edea0f6
PKGBUILD SHA-256: 317fe2d2b03eda7b365bb19ef863c6e75780bf8395c69978efe276015d80da08
```

The verified original `modemmanager-q25-1.24.2-1-aarch64.pkg.tar.xz` remains in
`/home/maxsun/q25-work/packages/` and is copied under the candidate scratch's
`rollback/`: SHA-256 `ada33171becc54f73c3394a746279d2394c9b2795cdafd28c005bae66cea4755`;
its plugin still matches `60973f1a85f2a9444c72c72e21bd67b9f9ae1b1f5522769084a4acfb153c691e`.

`upstream-response-ownership.txt`, host test/sanitizer logs and
`aarch64-build.log` are retained in that scratch. Pristine 1.24.2 has no
CIREG/CIREP consumers; generic handlers do not directly match these tags.
No raw-observation handler or handler suppression was introduced. See README
for logical-cancellation/serial-timeout limitations and the exact opt-in.

## Offline IMS diagnostic classification — release 3, 2026-11-09 UTC

Parent's sanitized r3 receipt (`ims-observer-runner-r3/live-receipts.log`) shows
CIREG fields all unknown, but CIREP reporting=1/vops=1, in a 30.066s window.
That release-2 record does not identify whether CIREG errored or was rejected.
The parent restored original software and low power; this worker phase performed
**no device access, install, restart, radio operation or snapshot action**.

Release 3 keeps the exact two-query one-shot/cancellation behavior and strict
parsers. It captures GError before it is freed and appends bounded diagnostic
classification to the same MSG record. Only successful rejected replies get
shape predicates; no raw reply, token, error message or unknown domain name is
logged. Existing anchored collectors must be updated for the suffix before use.

- Changed helper tests: **7/7**, including bounded shape/overflow/mixed-line
  predicates, retained `+CIREG:0,0`, and rejection of hypothetical vendor tails.
- Changed observer tests: **19/19**, including normalized error domains/codes,
  unknown-domain redaction, empty/missing responses and late errors after
  disable/dispose. Same results with **ASan/UBSan + leak detection**, no diagnostics.
- Clean aarch64 build/check: **7/7 + 19/19**, no compiler warnings. Test compilation
  uses the configured build's generated names header for upstream error enums.
- Existing core/guard/Android-setup/deployment suites were not repeated. The
  plugin entry-point/lifecycle wiring, Meson wiring and udev rule are unchanged
  from release 2. No RPATH/RUNPATH. Original -1 and accepted -2 archives retained.

Artifact directory: `/home/maxsun/q25-work/ims-observer-diagnostics-89f9ea1f/`.

```text
modemmanager-q25-1.24.2-3-aarch64.pkg.tar.xz
SHA-256: 5e6eb983ad40bbe19383dc9e3c2752cb00aad758b06555c28b0be96c6a701fc7
plugin SHA-256: 56a91949548ac7fec0fb9019d053407082d95dbf38ed25e8337c3e24c34cc577
PKGBUILD SHA-256: 7a684b41d95a9a5a0ae6e1cc74e63fa845ac2883f48a196da5f4bfe6757c5487
```

Isolated builder: `/home/builder/modemmanager-q25-ims-diagnostics-89f9ea1f`.
`delta-from-release2.diff`, focused static RIL/MM evidence and build/test logs
are retained in the artifact directory. Static RIL parsing is more permissive,
but no new live vendor response shape was proved; parser acceptance was not
relaxed. No fresh IMS registration state or transport fix is claimed.

## Offline CIREG first-two-field compatibility — release 4, 2026-11-09 UTC

Parent's r4 receipt (`ims-observer-runner-r4/live-receipts.log`, 30.051s) records
CIREG `rejected/none/-1`, length13, prefix1, fields3, decimal7/hex7, multiline0;
CIREP parsed reporting1/VoPS1. Shape does not identify the handset's values.
Rejection could arise from enum validity or registration/extension semantics;
known reporting modes did not gate capability extraction.

Release 4 retains valid first-two enums with a complete 1–8-digit hex tail,
but interprets capabilities only when registered and the tail is nonzero.
Unregistered or zero padding leaves capabilities unknown, without erasing valid
registration. Empty/malformed/oversized/extra tails, mixed lines and unknown
enums still reject atomically. This is diagnostic compatibility, not an SMS fix.

- Host and isolated aarch64 checks: **7/7 helpers + 20/20 observer tests**.
- ASan/UBSan/leak detection: same **27/27**, clean. Fixtures include `0,0,0`,
  `0,1,0`, unregistered nonzero padding, registered nonzero capabilities under
  reporting0, invalid enums with the same r4 shape, and the 96/97-byte boundary.
- Warning-free `makepkg --noconfirm -Cf`; ELF64/AArch64, no RPATH/RUNPATH.
- Observer/classifier implementation, headers, command/timeouts/cancellation,
  default-off behavior, log schema, plugin wiring, Meson and udev rule are
  byte-identical to release 3. Existing r4 collector schema remains compatible.
- Accepted -3 and original -1 archives preserved. No phone access, deployment,
  snapshots, commits or repeated guard/deployment suites in this phase.

Artifacts and incremental source diff:
`/home/maxsun/q25-work/ims-observer-cireg-compat-31e0a8bf/`.
Separate builder: `/home/builder/modemmanager-q25-cireg-compat-31e0a8bf`.

```text
modemmanager-q25-1.24.2-4-aarch64.pkg.tar.xz
SHA-256: 9517a5a03d4bee74aaa2dec3e43d312837ea27bdbd3f5526ec1e7ef56a070047
plugin SHA-256: eda3974a1919ffcac81b0d405ec1ce7389408a27e25184aee3b1e30f05ae59b7
PKGBUILD SHA-256: 91db5945a94d5213a4c5d64a320cafec044f5bad2be74316085e9fbabc1c65b1
```
