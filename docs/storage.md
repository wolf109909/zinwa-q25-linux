# Q25 btrfs image, snapshots and recovery

This is a **new-image format**, not an in-place ext4 migration. Flashing userdata
still destroys its previous contents. Vendor nvdata/nvcfg/protect/persist remain
ext4; only the Linux userdata filesystem changes. The kernel has BTRFS_FS built
in. Its config disables BTRFS_FS_RUN_SANITY_TESTS and BTRFS_ASSERT; building and
integrating that kernel is a separate step.

## Layout and build

| Top-level subvolume | Mounted at |
| --- | --- |
| `@` | initially `/` (filesystem **default**) |
| `@home` | `/home` |
| `@root` | `/root` |
| `@srv` | `/srv` |
| `@cache` | `/var/cache` |
| `@log` | `/var/log` |
| `@tmp` | `/var/tmp` |
| `@snapshots` | `/.snapshots` |

All use `noatime,compress=zstd:1`. **Neither initramfs nor fstab pins `/` to `@`.**
The other seven mounts keep explicit subvolume names across root rollback.
`/var/lib`, including pacman's database, is part of root; home, root's home,
service data in `/srv`, cache and logs are not rolled back. Back those up
separately. Snapshots are not off-device backups.

`build-rootfs.sh image` requires host btrfs-progs **7.x** with
`--rootdir`, `--subvol` and `--compress`. It copies staging to a temporary layout,
never rearranges staging, creates the subvolumes and default in mkfs, compresses
initial data with zstd:1, and runs a read-only btrfs check. mkfs may enlarge the
sparse raw file beyond the initial size estimate. Android sparse chunks carry
only nonzero data (zero ranges use FILL to erase stale userdata), not the holes.
The configured 48 MiB data payload per chunk stays below LK's 64 MiB download
limit. No ext4-specific tooling is needed to split or flash them.

For this offline build lane, do **not** use the normal rootfs/out/kernel tree:

```sh
sudo cp -a ~/q25-work/rootfs ~/q25-work/rootfs-btrfs  # once, destination must not exist
sudo env ROOT="$HOME/q25-work/rootfs-btrfs" OUT="$HOME/q25-work/out-btrfs" \
  KSTAGE="$HOME/q25-work/kstage" build/build-rootfs.sh overlay configure image
build/build-initramfs.sh ~/q25-work/rootfs-btrfs ~/q25-work/out-btrfs/initramfs.cpio.gz
```

Install the updated package set into the scratch tree before overlay/configure.
No kernel rebuild or device access is required. The initramfs contains static
BusyBox and btrfs plus its shared libraries, **not e2fsck**, and remains padded to
exactly 8 MiB for the DTB's fixed initrd window. Firstboot runs
`btrfs filesystem resize max /`; repeating it after a rollback is harmless.

## Snapper policy

The image installs snapper, snap-pac and btrfs-assistant and provisions the `root`
config. **Do not run `snapper create-config /`**: `/.snapshots` already mounts its
separate subvolume. Timeline/cleanup timers are enabled. Limits are 6 hourly,
7 daily, 4 weekly, 1 monthly, no yearly snapshots; package-transaction number
cleanup keeps 8 normal / 4 important snapshots. These are count limits, not a
hard space cap. Qgroups are off to avoid accounting overhead on this phone;
monitor `btrfs filesystem usage /` and free space after large upgrades.

To restore a root snapshot, use the upstream classic mode explicitly:

```sh
sudo snapper -c root list
sudo snapper --ambit classic -c root rollback NUMBER
sudo reboot
```

The initial `@` is not a numbered Snapper snapshot, so Snapper cannot always
auto-detect its ambit: unqualified `snapper rollback` can report "default
subvolume is unknown". There is no shipped default-ambit config override; use
`--ambit classic`. This creates a writable snapshot and makes it the btrfs
default; the next boot follows it without GRUB or another bootloader.
Btrfs-assistant is installed for inspection/snapshot management; its restore UI
is not claimed to implement this phone's boot-slot policy. Use the CLI above
for boot rollback.

## Good checkpoints and automatic fallback

A **good** checkpoint means the system reached `multi-user.target`. It does
**not** attest that the screen, modem, network, login or all services work.

* Initramfs mounts subvolid=5 privately at `/btrfs`. The top-level ordinary
  directory `q25-boot/` holds `boot-count`, `good/NUMBER` records and recovery
  state. This is outside every root snapshot and outside Snapper's directory;
  rolling back cannot rewind the counter. State is root-only.
* Each boot increments and syncs the counter before switching root.
  `q25-boot-ok.service` clears it at multi-user and creates a read-only Snapper
  checkpoint tagged `q25-good=yes`, unless the root ID/generation is unchanged
  since the last checkpoint. Runtime writes in `/var/lib` can advance the
  generation even without a package change. Only the latest **three** good
  checkpoints are retained; they have no automatic cleanup algorithm.
* On the **fourth attempt after three failed boots**, initramfs chooses the
  newest recorded good checkpoint that still exists and has modules for the
  running kernel. Ordinary timeline or snap-pac snapshots are not assumed good.
  It creates a **writable clone** under top-level
  `q25-boot/recovery-NUMBER-TIMESTAMP`, sets that clone default, and boots it.
  This is the same default-subvolume mechanism as classic rollback, implemented
  with btrfs directly so Snapper/DBus is not needed in the small initramfs.
* Recovery is recorded and synced **before** changing the default. The recovery
  root gets up to three boot attempts. If it also fails, there is **no endless
  fallback loop**: enter USB rescue. Success clears the recovery marker; later
  successful boots prune unused recovery clones, never the running/default one.
  Missing compatible checkpoints, mount errors, and explicit `q25.rescue` also
  enter rescue. A power loss during recovery setup can intentionally leave the
  system in rescue rather than retry a partially completed operation.
* If checkpoint creation fails (for example, ENOSPC), the successful boot still
  clears the counter, but no new good record is published. Inspect the service
  journal; old good checkpoints remain the fallback candidates.

In rescue, `/btrfs` exposes top-level state. To continue after a deliberate
repair, check the default and matching modules, clear the persistent attempt
state, then request a retry:

```sh
btrfs subvolume get-default /btrfs
# If necessary: btrfs subvolume set-default ID /btrfs
rm -f /btrfs/q25-boot/boot-count /btrfs/q25-boot/recovery-attempt
sync
echo > /run/continue
```

The existing rescue USB NCM/telnet console is unauthenticated; use it only on a
trusted directly attached host. These instructions do not initiate networking
on the development host.

### Kernel/root safeguard and A/B plan

`boot.img` is **outside btrfs**. Snapshot rollback cannot roll back the kernel or
ramdisk. Before *every* switch_root (including a manual default change), the
initramfs requires the init executable, a nonempty
`/usr/lib/modules/$(uname -r)/modules.dep`, and module files in that release's
directory. Automatic recovery skips incompatible checkpoints. A default root
without matching modules is **refused**, with a clear rescue message, rather
than booting a system with no modules. This is a fail-closed safeguard, not
transparent cross-kernel rollback: restore the matching boot.img or a compatible
root from rescue. Do not remove modules still needed by either boot slot.

Future A/B update transaction (not implemented in flash.sh):

1. Give each ABI-changing kernel build a unique `uname -r`; stage its complete
   modules alongside the active release, run depmod, and checkpoint that root.
2. Write and verify new boot.img in the **inactive** boot slot, leaving the
   active slot and its matching root checkpoint untouched. Record slot ->
   kernel release -> root default/checkpoint mapping outside root snapshots.
3. Select the new slot for a trial boot. Commit the slot/root pair only after
   the chosen health criterion succeeds. On failure restore **both** the old
   slot and its compatible root default, not just one of them.
4. Retire old kernels/modules only when neither a boot slot nor a retained
   recovery checkpoint references them. Keep DTB/vendor-boot compatibility in
   that transaction too.

The current guard checks presence for the exact release, not module signatures,
vermagic of every file, or corruption. Reusing the same release string for an
ABI-incompatible rebuild defeats that check; do not do it.

### Volume-Down picker: deferred

A reliable "held during early boot" picker is **not implemented**. BusyBox can
read evdev events with dd/hexdump, but cannot query `EVIOCGKEY`: a key already
held before opening `/dev/input/event*` need not emit a new press. Device event
numbers are unstable, and the built-in screen may not be usable before its
modules load. A future tiny evdev ioctl/poll helper must identify gpio-keys,
query initial state, debounce, time out, and present a tested console/display
chooser. Do not claim an event-stream-only approximation supports holding the
key. Today use classic rollback or the explicit rescue/default-selection path.

## Reproducible offline checks

```sh
for script in build/*.sh initramfs/init initramfs/boot-root.sh rootfs/usr/lib/q25/q25-{firstboot,boot-ok}; do
  bash -n "$script" || exit
done
python3 build/test-btrfs-config.py -v
python3 build/test-sparse-chunks.py
sudo build/test-btrfs-boot.sh ~/q25-work/rootfs-btrfs
sudo build/test-btrfs-checkpoint.sh  # host snapper + btrfs-progs, private mount namespace
python3 build/test-sparse-chunks.py ~/q25-work/out-btrfs/q25-rootfs.img ~/q25-work/out-btrfs/userdata
# Mount image read-only twice: subvolid=5 at TOP, default at ROOT.
# Use -o loop,ro,rescue=nologreplay[,subvolid=5]. Then:
sudo env Q25_BTRFS_TOP=TOP Q25_BTRFS_ROOT=ROOT python3 build/test-btrfs-config.py -v
# Unmount both before rebuilding.
```

The boot-policy test runs the real shell helper under ARM64 BusyBox/qemu against
a disposable loop filesystem (native host btrfs/mount utilities). Qemu-user
cannot translate all btrfs ioctls: the target btrfs `subvolume show` currently
fails with ENOTTY on this WSL host. The initramfs's target binary/library closure
is smoke-tested with `btrfs version`; native Snapper integration verifies
checkpoint deduplication/retention, classic rollback and that the **same
initramfs mount function follows the newly selected default**. This does not
emulate the full PID 1/MT6789 hardware boot or exercise USB rescue on a phone.
