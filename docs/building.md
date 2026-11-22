# Building

Everything is built on an x86_64 Arch Linux host (WSL2 works) and cross-compiled
for aarch64. The only supported kernel is **Linux 7.3.3** with the patch series
in `patches/`.

## Host requirements

| Purpose | Packages / tools |
| :--- | :--- |
| Kernel | `aarch64-linux-gnu-gcc`, `bc`, `cpio`, `dtc`, `pahole`, `perl`, `python`, `openssl`, `zstd` |
| Packages and rootfs | `arch-install-scripts` (`pacstrap`, `arch-chroot`), `qemu-user-static` + binfmt (`qemu-aarch64-static`), `pacman` (`vercmp`), `bsdtar`, `python3` |
| Image | `btrfs-progs` 7.x (`mkfs.btrfs --rootdir --subvol --compress`) |
| boot.img | `mkbootimg`, `avbtool` (Android tools) |
| Flashing | `fastboot` (on WSL: Windows `fastboot.exe`, see [flashing.md](flashing.md)) |

Root is needed for the package and rootfs stages (`sudo`).

## One-shot build

```sh
build/prepare-kernel.sh      # first time only
build/build-all.sh
```

`build-all.sh` runs the stages below in dependency order and leaves everything
needed by `build/flash.sh` in `out/`:

| Output | Produced by |
| :--- | :--- |
| `~/q25-work/kstage/` (Image.gz, DTB, modules) | `build-kernel.sh` |
| `out/packages/*.pkg.tar.*` | `build-packages.sh` |
| `out/q25-rootfs.img`, `out/userdata/userdata_*.simg` | `build-rootfs.sh` |
| `out/initramfs.cpio.gz` | `build-initramfs.sh` |
| `out/boot.img` | `build-boot.sh` |

## Stages

### 1. Kernel source tree: `build/prepare-kernel.sh`

Downloads the kernel.org 7.3.3 tarball and applies, in order:
`patches/mt6789-soc/0001-0023` (SoC support), `patches/cachyos-7.3/` (BORE), then
`patches/mt6789-soc/0024` and later (Q25 board fixes). Copies the devicetree and
`cachyos/config-mt6789-mainline-7.3.aarch64`. Default location
`~/q25-work/linux-7.3` (`KSRC`).

### 2. Kernel and modules: `build/build-kernel.sh`

Syncs the repository's config and devicetree into the tree, builds `Image.gz`,
the DTB, the in-tree modules and every out-of-tree driver under `drivers/`, and
stages them in `~/q25-work/kstage` (`KSTAGE`). The DTB is linked into the kernel
image (see [kernel.md](kernel.md#boot-chain)), so a devicetree change always
needs a new boot.img.

### 3. Arch packages: `sudo build/build-packages.sh [dir...]`

Builds `packages/*` with `makepkg` in a clean Arch Linux ARM chroot
(`BUILDROOT`, default `~/q25-work/pkgbuild-root`). Output goes to `out/packages/`.

| Package | Purpose |
| :--- | :--- |
| `modemmanager-q25` | ModemManager with the MTK CCCI plugin and the IMS bridge used for SMS |
| `keyd-q25` | keyd with a read-only state socket for the keyboard indicators |
| `phosh-q25-keyboard` | Phosh status-icon plugin for Shift/Caps/Sym; built for one exact Phosh release |
| `kitty-upstream-bin` | kitty terminal |

### 4. Root filesystem and image: `sudo build/build-rootfs.sh [stage...]`

Stages: `bootstrap` (pacstrap `build/packages.txt`), `packages` (newest build of
each package in `out/packages`), `overlay` (`rootfs/`, firmware, kernel modules,
helpers), `configure` (users, locale, services, keyring), `image` (btrfs image
and sparse userdata chunks).

| Variable | Meaning | Default |
| :--- | :--- | :--- |
| `ROOT` | rootfs staging directory | `~/q25-work/rootfs` |
| `OUT` | output directory | `<repo>/out` |
| `KSTAGE` | kernel staging directory | `~/q25-work/kstage` |
| `Q25_USER` | user name | `alarm` |
| `Q25_PASSWORD` | initial password / lock-screen PIN | same as `Q25_USER` |
| `SSH_KEY` | public key for root and the user; also makes SSH key-only | unset |
| `Q25_APN` | carrier APN; ships a NetworkManager mobile profile with the attach APN | unset |

Set `Q25_PASSWORD` (numeric suits the Phosh keypad) and `SSH_KEY` for any image
that will be on a network.

### 5. Boot image: `build/build-initramfs.sh`, `build/build-boot.sh`

`build-boot.sh <Image.gz> <initramfs.cpio.gz> <out boot.img>` creates an Android
boot image (header v4) with an unsigned AVB hash footer for the 64 MiB boot
partition. The bootloader is unlocked and rejects images without a footer.

## Tests

```sh
python3 -m unittest discover -s tests -v
for t in build/test-*.py; do python3 "$t"; done
```

Some tests need sources: `PHOSH_SOURCE=<phosh-0.59.0>` for the keyboard plugin,
`KEYD_SOURCE=<keyd-2.6.0>` for the keyd patch.
