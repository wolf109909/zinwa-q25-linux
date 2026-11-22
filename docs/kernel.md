# Kernel

Linux **7.3.3** with the patch series below, built as `7.3.3-cachyos-q25`.
Older kernel versions are not maintained.

## Patch series

Applied by `build/prepare-kernel.sh` in this order.

**SoC support** (`patches/mt6789-soc/0001-0023`, based on
[mtk_g99-mainline](https://gitlab.nura.eco/ellyq/mtk_g99-mainline)):

| Patch | Subject |
| :--- | :--- |
| 0001, 0007, 0020 | MT6789 clocks, FHCTL, build fixes |
| 0002 | mailbox: mtk-cmdq: MT6789 |
| 0003 | cpufreq: mediatek-cpufreq-hw: MT6789 |
| 0004, 0012, 0016, 0022 | MT6789 SoC devicetree |
| 0005, 0017 | DVFSRC regulator, regulator coupler |
| 0006 | interconnect: MT6789 EMI |
| 0008, 0011 | panfrost: MT6789 GPU and DVFS |
| 0009 | i2c: mt65xx: MT6789 |
| 0010, 0013 | dt-bindings: IOMMU, pin controller |
| 0014 | soc: mtk-pmic-pwrap: MT6789 |
| 0015 | pwm: mtk-disp: MT6789 |
| 0018 | memory: mtk-smi: MT6789 |
| 0019 | phy: mtk-tphy: device/host mode on mobile |
| 0021 | pinctrl: MT6789 driver |
| 0023 | Initial Zinwa Q25 devicetree |

**Scheduler:** `patches/cachyos-7.3/0001` CachyOS BORE.

**Q25 board fixes** (`patches/mt6789-soc/0024` and later):

| Patch | Subject |
| :--- | :--- |
| 0024 | 7.3 bring-up: clock parents, display/DSI, power domains, Panfrost, I2C, boot chain |
| 0025 | iio: mt6359-auxadc: BAT_TEMP in millivolts (NTC lookup in the DT) |
| 0026 | mfd: mt6397-core: MT6358 ACCDET cell (headset detection) |
| 0027 | mfd: mt6358: AUD/MISC top interrupt status bits |
| 0029 | iio: mt6359-auxadc: efuse-calibrated PMIC die temperatures |
| 0030 | pinctrl: mt6789: pull-type table covers all 222 pins |
| 0031 | drm/mediatek: dsi: program video timing on bootloader handoff (real 60 Hz) |
| 0032 | arm64: remove debug hooks from generic code (printk copy, early watchdog poke) |

The board devicetree itself is `devicetree/mt8781-zinwa-q25.dts`; the build copies
it into the tree, so it is maintained there rather than as patches.

## Configuration

`cachyos/config-mt6789-mainline-7.3.aarch64` is the full `olddefconfig` output.
Notable choices: BORE, `NR_CPUS=8`, btrfs built in, XFRM/ESP for IMS IPsec,
soft/hard lockup, hung-task and workqueue watchdogs (report only), KFENCE,
freelist and list hardening, `MEDIATEK_WATCHDOG` with
`WATCHDOG_HANDLE_BOOT_ENABLED`, ramoops for crash logs.

There is no `MODVERSIONS`: always install the kernel together with its complete
module tree (`build/deploy-kernel.sh`).

## Boot chain

The stock bootloader (LK) loads `boot_a`: an Android boot image v4 containing
`Image.gz` and the Q25 ramdisk, concatenated with the stock `vendor_boot`
ramdisk.

- **Devicetree:** LK passes the vendor 5.10 devicetree, which mainline cannot
  use. The kernel instead links `mt8781-zinwa-q25.dtb` into its image and
  `head.S` substitutes it for the one LK passed. The built-in DTB provides the
  command line (`chosen/bootargs`), the memory map and the initrd location.
  LK's command line is ignored.
- **DBGC flag:** `head.S` also sets one flag in LK's debug-control block
  (`0x48081000`, magic `DBGC`) that LK reads after a reset.
- **Display handoff:** LK leaves the DSI link running. `mtk_dsi` keeps the PHY
  and reprograms the video timing from the panel mode (patch 0031).
- **Watchdog:** LK leaves TOPRGU armed; the `mtk_wdt` driver takes it over.
- **Crash logs:** ramoops at `0x48090000`, read back from `/sys/fs/pstore`.

## Out-of-tree drivers

Built by `build/build-kernel.sh` from `drivers/` against the patched tree and
installed to `/lib/modules/<release>/updates/q25/`.

| Module | Hardware | Bus |
| :--- | :--- | :--- |
| `panel-q20-hd720-lcm-dsi-vdo` | 720x720 DSI panel | DSI0 |
| `ocp2131_drv` | OCP2131 LCD bias supply | I2C7 `0x3e` |
| `synaptics_dsx_q25` | Synaptics S2716B touchscreen | I2C0 `0x67` |
| `bbqX0kbd`, `q20_keyboard` | Keyboard, trackpad, backlight; keyboard MCU power and firmware | I2C6 `0x1f` |
| `snd-soc-oca72xxx` | MT6789 AFE PCM, MT6358 machine driver, ACCDET, OCA7255 amplifier | I2C3 `0x58` |
| `upm6922_charger` | UPM6922 charger and OTG boost | I2C5 `0x6b` |
| `upm6722` | UPM6722 charge pump | I2C5 `0x65` |
| `q25_mt6358_gauge` | MT6366 fuel gauge (coulomb counter + vendor OCV table) | PMIC |
| `q25_sensors` | MMC5603NJ magnetometer, HX32062SE light/proximity | I2C1 `0x30`, I2C9 `0x2b` |
| `st21nfc` | ST21NFC NFC controller | I2C3 `0x08` |
| `aw36518`, `flashlight-gpio-v4l2` | Flash LED / torch | I2C, GPIO |
| `wmt_drv`, `wlan_drv_gen4m_6789`, `wmt_chrdev_wifi`, `bt_drv_connac1x`, `btif_drv`, `connadp`, `connfem` | MediaTek CONNSYS Wi-Fi and Bluetooth | CONNSYS |
| `ccci_*`, `ccmni`, `conn_md_drv` | MediaTek ECCCI modem interface, DPMAIF data path, network devices | CCIF / DPMAIF |

The SC7A20 accelerometer uses the mainline `st_accel` driver.

The vendor MediaTek drivers under `drivers/mediatek/` are ports of the 5.10
vendor kernel, with fixes for 7.3 APIs and memory-safety issues found in review
in review.

## Known deviations from a clean mainline port

- `clk_ignore_unused` and `pd_ignore_unused` are on the command line: 234 of 365
  clocks are left on without a consumer. Removing them is planned subsystem by
  subsystem.
- The display power domain is kept always-on, and the DSI PHY's power-on count
  is unbalanced on the handoff path.
- The genpd core special-cases the CONNSYS and modem nodes so their domains are
  not powered on at attach.
- The embedded DTB replaces LK's, so LK's runtime updates (memory size, KASLR
  seed) are not used.
