# Linux for the Zinwa Q25

Mainline Linux 7.3.3 and Arch Linux ARM with Phosh for the **Zinwa Q25**
(`q20_v12_factory`): MediaTek Helio G99 (MT6789 / MT8781), 12 GB RAM, 256 GB UFS,
720x720 display and a hardware QWERTY keyboard.

This repository contains the kernel patch series, the board devicetree, the
out-of-tree drivers ported from the vendor 5.10 kernel, the Arch packages and
rootfs overlay, and the scripts that build and flash a complete phone image.

## Hardware status

Kernel `7.3.3-cachyos-q25` (build #49), checked on the device in November 2026.

**Ready**: works in daily use. **Partial**: works, with the stated limitation.
**TBD**: not working or not implemented yet.

| Component | Status | Driver / notes |
| :--- | :--- | :--- |
| CPU (2x Cortex-A76 + 6x Cortex-A55), DVFS | ✅ Ready | `mediatek-cpufreq-hw`, BORE scheduler |
| RAM (12 GB), UFS storage (256 GB) | ✅ Ready | `ufs-mediatek`, btrfs root filesystem |
| microSD | ✅ Ready | `mtk-sd` |
| Display (720x720 MIPI DSI), backlight | ✅ Ready | `mediatek-drm` + `panel-q20-hd720-lcm-dsi-vdo`, 60 Hz |
| GPU (Mali-G57 MC2) | ✅ Ready | Panfrost: OpenGL ES 3.1 / OpenGL 3.1, DVFS 390–1100 MHz. No Vulkan (PanVK does not support Valhall v9) |
| Touchscreen (Synaptics S2716B) | ✅ Ready | `synaptics_dsx_q25` |
| Keyboard, trackpad, keyboard backlight | ✅ Ready | `bbqX0kbd`, `q20_keyboard`, `keyd`; Shift/Caps/Sym indicators in Phosh |
| Power, volume and PTT keys | ✅ Ready | `mtk-pmic-keys`, `gpio-keys` |
| Speaker, earpiece | ✅ Ready | MT6358 codec + OCA7255 amplifier, 48 kHz |
| Headphone jack (output, plug detection), microphones | ✅ Ready | MT6358 ACCDET |
| Charging, fuel gauge, battery temperature | ✅ Ready | `upm6922_charger`, `upm6722`, `q25_mt6358_gauge` |
| Thermal sensors | ✅ Ready | `mt6359-auxadc`, `generic-adc-thermal` |
| Accelerometer (auto-rotate) | ✅ Ready | mainline `st_accel` (SC7A20) |
| Proximity sensor | ✅ Ready | `q25_sensors` (HX32062SE) |
| Magnetometer | ✅ Ready | `q25_sensors` (MMC5603NJ) |
| Ambient light sensor | 🟡 Partial | Raw channels only, no lux, so no auto-brightness |
| Wi-Fi (802.11ac) | ✅ Ready | MediaTek CONNSYS (`wlan_drv_gen4m_6789`) |
| Bluetooth | 🟡 Partial | Drivers and the `q25-bt-vhci` bridge exist; the controller does not come up yet |
| LTE mobile data | ✅ Ready | ECCCI / DPMAIF / CCMNI + ModemManager plugin |
| SMS (send, receive, delivery reports) | ✅ Ready | Over IMS: `q25-imsd` + ModemManager → Chatty |
| Voice calls (VoLTE), USSD | ⏳ TBD | Needs IMS voice (SIP INVITE and an RTP/AMR audio path) |
| NFC | 🟡 Partial | `st21nfc` kernel driver works; no userspace NFC stack |
| Flash LED / torch, vibration motor, notification LED | ✅ Ready | `aw36518`, `flashlight-gpio-v4l2`, `leds-gpio` |
| USB device mode (charging, NCM networking) | 🟡 Partial | The NCM link breaks if the USB interface is reset |
| Cameras | ⏳ TBD | No sensor or ISP driver |
| GNSS / GPS | ⏳ TBD | No driver |
| Suspend | ⏳ TBD | Disabled system-wide |
| Hardware video decode / encode | ⏳ TBD | Vendor-only VCU firmware protocol |
| Power management | ⏳ TBD | Boots with `clk_ignore_unused` / `pd_ignore_unused`; see [kernel.md](docs/kernel.md#known-deviations-from-a-clean-mainline-port) |

## Quick start

Building needs an Arch Linux (or Arch Linux ARM) x86_64 host; WSL2 works. See
[docs/building.md](docs/building.md) for the full list of host tools.

```sh
build/prepare-kernel.sh            # once: Linux 7.3.3 + patches/ -> ~/q25-work/linux-7.3
build/build-all.sh                 # kernel, packages, rootfs image, initramfs, boot.img -> out/
build/flash.sh                     # phone in fastboot mode; ERASES userdata
```

Kernel updates on a running phone, without reflashing userdata:

```sh
build/build-kernel.sh
build/build-boot.sh ~/q25-work/kstage/boot/Image.gz out/initramfs.cpio.gz out/boot.img
OUT=out build/deploy-kernel.sh     # boot_a + the complete module tree, over SSH
```

Flashing, recovery and the A/B slot behaviour of this bootloader are described in
[docs/flashing.md](docs/flashing.md).

## Repository layout

| Path | Contents |
| :--- | :--- |
| `patches/mt6789-soc/` | Kernel patch series: MT6789 SoC support and Q25 board fixes |
| `patches/cachyos-7.3/` | CachyOS BORE scheduler for 7.3.3 |
| `cachyos/` | Kernel config (`config-mt6789-mainline-7.3.aarch64`) |
| `devicetree/` | Board devicetree (`mt8781-zinwa-q25.dts`) and the vendor devicetree for reference |
| `drivers/` | Out-of-tree drivers built against the patched tree (see [docs/kernel.md](docs/kernel.md)) |
| `firmware/` | Wi-Fi/BT (CONNSYS) and audio amplifier firmware |
| `packages/` | Arch packages built for the image: ModemManager with the MTK CCCI plugin, keyd, Phosh keyboard indicators, kitty |
| `rootfs/` | Files installed into the image: systemd units, udev rules, ALSA UCM, `q25-imsd` |
| `initramfs/` | Boot ramdisk (mounts the btrfs root) |
| `src/` | Small native helpers (`q25-wmt-init`, `q25-bt-vhci`, `libprop_preload`) |
| `build/` | Build, image and flashing scripts and their tests |
| `tests/` | Host-side tests for drivers, helpers and packages |
| `tools/` | Audio tuning generator, Windows fastboot driver installer |
| `docs/` | Documentation |

## Documentation

- [Building](docs/building.md): host setup, every build stage, image contents
- [Flashing and recovery](docs/flashing.md): fastboot, kernel deploys, A/B slots
- [Kernel](docs/kernel.md): patch series, config, boot chain, drivers
- [Telephony](docs/telephony.md): modem, mobile data, SMS over IMS
- [Phosh image](docs/phosh.md), [storage and snapshots](docs/storage.md), [package policy](docs/packages.md)
- [Keyboard](docs/keyboard.md): layout and implementation

## Credits

- [mtk_g99-mainline](https://gitlab.nura.eco/ellyq/mtk_g99-mainline): MT6789 mainline SoC support
- [postmarketOS Zinwa Q25 port](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/8365)
- [LineageOS](https://github.com/LineageOS/android_kernel_xelex_mt6789) and
  [JamiKettunen](https://github.com/JamiKettunen/android_kernel_zinwa_mt6789) vendor kernel trees
- [CachyOS kernel patches](https://github.com/CachyOS/kernel-patches)

Kernel patches and drivers are GPL-2.0, as marked by their SPDX headers.
