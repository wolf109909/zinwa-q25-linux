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

## Validation

See [VALIDATION.md](VALIDATION.md) for build fingerprints, handset results and
explicitly untested native-systemd boot behavior.

`makepkg` runs `test-modem-helpers-mtk-ccci.c` against GLib. Host-only tests:

```sh
cc -Wall -Wextra -Werror mm-modem-helpers-mtk-ccci.c \
    test-modem-helpers-mtk-ccci.c $(pkg-config --cflags --libs gio-2.0) \
    -o /tmp/test-modem-helpers-mtk-ccci
/tmp/test-modem-helpers-mtk-ccci
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
