# Q25 native Bluetooth bootstrap and validation limits

## Bootstrap repair

`q25-bt-vhci` creates the first HCI controller by bridging `/dev/stpbt` to
`/dev/vhci`. It must be enabled under `multi-user.target`, not only
`bluetooth.target`: systemd's Bluetooth udev rule starts `bluetooth.target`
**after** an HCI device appears. Waiting for that target to start the device's
creator leaves an otherwise enabled bridge inactive. The existing ordering
against `q25-connsys.service` and `bluetooth.service` is unchanged.

The helper's factory-address command is unchanged, but its log no longer prints
the per-device address. The log says only that the command was submitted; the
existing helper does not validate the command-complete status, so this is not
proof that the controller accepted the address. Address files remain read-only.

Hardware-free checks and helper-only build (no image build or device access):

```sh
python3 tests/test_q25_bt_vhci.py -v
aarch64-linux-gnu-gcc -static -O2 -Wall -o /tmp/q25-bt-vhci src/q25-bt-vhci.c
```

The tests compile the real helper with mocked file/device I/O, validate unchanged
command bytes and read-only factory/fallback access, and check identifier-free
logging. `systemctl --root` tests fresh enablement and migration in temporary
roots; they never start a service or contact a running system manager.

## Installation/migration: not authorized on the current overnight device

Merely replacing the unit file does **not** migrate an existing enablement
symlink. In a separately authorized, attended maintenance window, back up the
installed helper, unit and enablement links; take fresh read-only root **and
separate-home** checkpoints; verify recovery access and retained transmit guards.
Install the helper and unit only after review. Then, to migrate enablement without
starting the bridge:

```sh
sudo systemctl daemon-reload
sudo systemctl reenable q25-bt-vhci.service  # deliberately no --now
```

This replaces `bluetooth.target.wants/q25-bt-vhci.service` with
`multi-user.target.wants/q25-bt-vhci.service`. It schedules activation on a future
normal boot; it is **not** permission to reboot or start the service now. An
offline target root can instead use `systemctl --root="$ROOT" reenable
q25-bt-vhci.service`. New rootfs builds already enable this unit through
`build/build-rootfs.sh`. To undo a future installation, restore the saved helper
and unit, reenable the restored unit, and reload the manager; this does not undo
controller/kernel state and is not an unattended hardware recovery procedure.

## Why live bring-up was deferred (2026-11-08)

Read-only observations at 22:42 UTC found the bridge enabled but inactive, no
invocation in this boot, `bluetooth.target` inactive, BlueZ active, and the legacy
Bluetooth-target symlink present. `/dev/stpbt`, `/dev/vhci` and their drivers were
present; CONNSYS was already active/exited. The accepted preceding inventory
reported zero controllers. This supports the bootstrap diagnosis, **not** a
claim that controller initialization or Bluetooth traffic works. A final
metadata-only check at 23:14 UTC still found zero controllers in both sysfs and
BlueZ, unchanged helper/unit/core hashes and service PIDs, intact modem guards,
frozen/runtime-masked clients, USB recovery routing, and the existing VPN
interface. No new traffic test was performed.

Opening `/dev/stpbt` is not a passive probe. Relevant unchanged source anchors:

- `drivers/mediatek/connectivity/bt/mt66xx/wmt/stp_chrdev_bt.c:781`, `BT_open`:
  calls `mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT)`; `BT_close` at line 860 calls off.
- `drivers/mediatek/connectivity/common/common_main/core/wmt_func.c:300`,
  `wmt_func_bt_on`: enters shared WMT function control.
- `drivers/mediatek/connectivity/common/common_main/core/wmt_core.c:446`,
  `wmt_core_func_ctrl_cmd`: missing firmware response triggers
  `wmt_lib_trigger_assert(WMTDRV_TYPE_WMT, 32)`.
- In the same file, `opfunc_func_on` at line 1289 can power the shared chip and
  calls `opfunc_try_pwr_off` on failure (line 1368);
  `wmt_core_trigger_stp_assert` at line 2796 calls `wmt_lib_trigger_reset` when
  coredumps are disabled.

A userspace service timeout cannot contain a vendor-driver hang or shared
assert/reset. Even with CONNSYS already active, unattended USB/LTE recovery cannot
be guaranteed. Starting BlueZ against a new controller also risks automatic
power-on/profile reconnection; merely avoiding explicit scan/pair commands is
insufficient. Any future trial needs separately approved policy isolation,
`Restart=no`, independent recovery, and an attended hardware recovery path.

The proposed bounded trial was **not executed**. No files were staged on the
phone; no Bluetooth service, module, CONNSYS, firmware, calibration, modem, VPN or
account settings were changed. No fresh checkpoints were needed because no live
mutation was approved. The existing read-only root/home checkpoints were checked.
Persistent no-transmit policies and the runtime-masked/frozen Calls and Chatty
clients must remain; do not reboot, thaw, unmask or relaunch those clients.

## Wi-Fi acceptance boundary

Safe checks are interface presence, NM-managed/enabled state, rfkill booleans and
existing disconnected state, without reading profiles, credentials, SSIDs or
peer identifiers. Do not request a scan, choose a profile or associate. The
previous inventory showed `wlan0` enabled/managed and disconnected; Wi-Fi
association and data transfer remain unvalidated. Ordinary system-route tests
with the existing VPN are not isolated LTE or Wi-Fi acceptance, and no VPN
bypass/change is permitted.
