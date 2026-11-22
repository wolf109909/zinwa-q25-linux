# Zinwa Q25 — Mainline Linux 7.3 + CachyOS & Arch Linux ARM (`MT6789` / `MT8781`)

Complete hardware enablement, forward-ported **Mainline Linux 7.3 (`7.3.3`) & 7.0** out-of-tree kernel modules (DKMS/Kbuild), rebased MT6789 SoC + CachyOS BORE `7.3` patchset (`patches/`), Mainline DeviceTree (`mt8781-zinwa-q25.dts`), CachyOS BORE kernel packaging, and Arch Linux ARM userspace customizations for the **Zinwa Q25** (`Xelex` / `q20_v12_factory`, MediaTek Helio G99 `MT6789`, 12 GB LPDDR4X, 256 GB UFS).

---

## Hardware status

Status on the current kernel (`7.3.3-cachyos-q25`, build #49) and image, checked on
the phone in November 2026. **Ready** = works in daily use; **Partial** = works
with a stated limitation; **TBD** = not working / not implemented yet.

| Component | Status | Driver / notes |
| :--- | :--- | :--- |
| CPU (2x A76 + 6x A55), DVFS, BORE scheduler | ✅ Ready | `mtk-cpufreq-hw` |
| RAM (12 GB) | ✅ Ready | |
| UFS storage (256 GB) | ✅ Ready | `ufshcd-mtk`, btrfs root |
| microSD | ✅ Ready | `mtk-msdc` (card detect + UHS fixed) |
| Display 720x720 MIPI DSI, backlight | ✅ Ready | `mediatek-drm` + `panel-q20-hd720-lcm-dsi-vdo`; real 60 Hz scan-out (patch 0031) |
| GPU Mali-G57 MC2 | ✅ Ready | Panfrost, OpenGL ES 3.1 / GL 3.1, DVFS 390–1100 MHz. **No Vulkan**: PanVK does not support Valhall v9 |
| Touchscreen (Synaptics S2716B) | ✅ Ready | `synaptics_dsx_q25` |
| QWERTY keyboard, trackpad, keyboard backlight | ✅ Ready | `bbqX0kbd` + `q20_keyboard` + `keyd`; Phosh status icons for Shift/Caps/Sym |
| Power / volume / PTT buttons | ✅ Ready | `mtk-pmic-keys`, `gpio-keys` |
| Speaker, earpiece | ✅ Ready | `q25_afe_pcm` (MT6358 codec) + OCA7255 PA; 48 kHz |
| Headphone jack (output + plug detection), microphones | ✅ Ready | ACCDET driver (patch 0026); capture via VUL12/MTKAIF |
| Charging, fuel gauge, battery temperature | ✅ Ready | `upm6922_charger`, `upm6722`, `q25_mt6358_gauge` |
| Thermal zones | ✅ Ready | `mt6359-auxadc` + `generic-adc-thermal` |
| Accelerometer (auto-rotate) | ✅ Ready | mainline `st_accel` (SC7A20) on AP I2C1 |
| Proximity sensor | ✅ Ready | `q25_sensors` (HX32062SE), iio-sensor-proxy |
| Magnetometer | ✅ Ready | `q25_sensors` (MMC5603NJ), raw + mount matrix |
| Ambient light sensor | 🟡 Partial | raw channels only, no lux (vendor lux math lives in SCP firmware) → no auto-brightness |
| Wi-Fi 802.11ac | ✅ Ready | MediaTek CONNSYS `wlan_drv_gen4m_6789` |
| Bluetooth | 🟡 Partial | kernel drivers + `q25-bt-vhci` bridge exist; controller not up on the current image |
| LTE mobile data | ✅ Ready | ECCCI / DPMAIF / CCMNI + ModemManager plugin |
| SMS (send/receive, delivery reports) | ✅ Ready | over IMS: `q25-imsd` + ModemManager bridge → Chatty (tested on China Telecom) |
| Voice calls (VoLTE), USSD | ⏳ TBD | needs IMS voice (SIP INVITE + RTP/AMR audio path) |
| NFC | 🟡 Partial | `st21nfc` kernel driver works (`/dev/st21nfc`); no userspace NFC stack |
| Torch / flash LED, vibration motor, RGB notification LED | ✅ Ready | `aw36518`, `flashlight-gpio-v4l2`, `leds-gpio` |
| USB device mode (NCM networking), charging | 🟡 Partial | works; the NCM link breaks if the USB interface is taken down and up again |
| Cameras | ⏳ TBD | no sensor/ISP driver |
| GNSS / GPS | ⏳ TBD | no driver |
| Suspend (s2idle / deep sleep) | ⏳ TBD | disabled system-wide for now |
| Hardware video decode/encode | ⏳ TBD | vendor-only VCU firmware protocol |
| Power management cleanup | ⏳ TBD | runs with `clk_ignore_unused` / `pd_ignore_unused`; plan in [docs/code-review-2026-11-22.md](docs/code-review-2026-11-22.md) |

Known open items and review notes: [docs/code-review-2026-11-22.md](docs/code-review-2026-11-22.md),
[docs/hardware-audit-2026-11.md](docs/hardware-audit-2026-11.md),
[docs/telephony-progress.md](docs/telephony-progress.md).

## Current standalone phone image

`build/build-all.sh` builds an Arch Linux ARM image with **stock Phosh as its only
configured desktop**, phoc at **1.25× (576×576 logical)**, and the complete phone
app set: Calls, Chatty, Contacts, Clocks, Weather, Calculator, Text Editor, Files
(Nautilus), Loupe, Maps, Snapshot, GNOME Console, **kitty** and **Firefox**.
GNOME Settings supplies Wi-Fi/Mobile Network/Bluetooth and normal system panels;
Mobile Settings supplies Phosh preferences. Stevia suppresses automatic OSK with
a hardware keyboard. SDDM/Plasma and custom GUI launchers are not in this image.

See **[build/PHOSH.md](build/PHOSH.md)** for package availability, the upstream
session service, clean-build instructions, tests and remaining device checks.
`Q25_USER` defaults to `alarm`; optional `Q25_PASSWORD` sets the initial account
password / lock-screen PIN (otherwise the password equals the user name).
A numeric password suits Phosh's keypad; alphanumeric hardware-keyboard input
still works. Change the default before network use; with `SSH_KEY` set, SSH is
key-only.

The hardware results and hybrid `userspace/` tools below describe earlier port
milestones. They are retained as reference, **not installed as the current image's
session or control center**. See [build/BTRFS.md](build/BTRFS.md) and
[build/PACKAGES.md](build/PACKAGES.md) for storage and package policy.

## Repository Structure

```text
zinwa-q25-linux/
├── drivers/                  # Forward-ported out-of-tree Q25 drivers for Mainline Linux 7.0 – 7.3+ (Kbuild & DKMS)
│   ├── Makefile              # Top-level Kbuild Makefile (builds all 12 .ko modules)
│   ├── dkms.conf             # DKMS configuration for automatic module rebuilds on kernel upgrades
│   ├── include/linux/        # Linux 7.3+ compatibility shims (e.g. of_gpio.h after upstream removal)
│   ├── keyboard/             # BBQX0 I2C QWERTY keyboard + optical trackpad + MCU power/FW control
│   │   ├── bbqX0kbd_main.c   # -> bbqX0kbd.ko (wallComputer,bbqX0kbd on &i2c6 @ 0x1f)
│   │   └── q20_keyboard.c    # -> q20_keyboard.ko (wallComputer,pdrv_bbqX0kbd platform power/FW driver)
│   ├── touchscreen/          # Synaptics S2716B 720x720 capacitive touchscreen (One-Euro filter + stuck-touch fix)
│   │   └── synaptics_dsx_*.c # -> synaptics_dsx_q25.ko (mediatek,cap_touch_synaptics on &i2c0 @ 0x67)
│   ├── display/              # Q20 720x720@60Hz MIPI DSI panel & OCP2131 +/-5.8V LCD bias IC
│   │   ├── ocp2131_drv.c     # -> ocp2131_drv.ko (mediatek,mtk-gateic-drv-ocp2131 on &i2c7 @ 0x3e)
│   │   └── panel-q20-hd720-lcm-dsi-vdo.c # -> panel-q20-hd720-lcm-dsi-vdo.ko (&dsi0)
│   ├── power/                # Unisemipower battery chargers & USB OTG VBUS boost regulator
│   │   ├── upm6922_charger.c # -> upm6922_charger.ko (up,upm6922_charger on &i2c5 @ 0x6b)
│   │   └── upm6722.c         # -> upm6722.ko (unisemipower,upm6722-standalone charge pump on &i2c5 @ 0x65)
│   ├── audio/                # Orient-Chip OCA72xxx Smart Audio PA (I2C + ASoC codec)
│   │   └── oca72xxx*.c       # -> snd-soc-oca72xxx.ko (ocs,oca72xxx_pa on &i2c3 @ 0x58)
│   ├── nfc/                  # STMicroelectronics ST21NFC NCI controller
│   │   └── st21nfc.c         # -> st21nfc.ko (st,st21nfc on &i2c3 @ 0x08)
│   └── leds/                 # V4L2 camera flash / torch subdev drivers
│       ├── aw36518.c         # -> aw36518.ko (mediatek,aw36518)
│       └── flashlight-gpio-v4l2.c # -> flashlight-gpio-v4l2.ko (mediatek,flashlightgpio)
│   └── sensors/              # Onboard IIO sensors (SC7A20 accel, MMC5603NJ mag, HX32062SE ALS/PS)
│       └── q25_sensors.c     # -> q25_sensors.ko (I2C IIO: mmc5603nj, hx32062se; sc7a20 = mainline st_accel)
├── firmware/
│   └── ocs72xxx_cf.bin       # Extracted Orient-Chip OCA7255 Smart PA ACF firmware (Music/Receiver/Off profiles)
├── patches/
│   ├── mt6789-soc/                   # 24 MT6789 / Helio G99 SoC + Q25 hardware patches for Linux 7.3.3
│   └── cachyos-7.3/                  # CachyOS BORE scheduler patch rebased onto Linux 7.3.3
├── devicetree/
│   ├── mt8781-zinwa-q25.dts          # Mainline Linux 7.0 / 7.3 DeviceTree for Zinwa Q25 with all peripherals wired
│   └── vendor-q20_v12_factory.dts    # Original vendor 5.10 DeviceTree overlay for hardware reference
├── cachyos/
│   ├── PKGBUILD                      # Arch Linux ARM PKGBUILD for linux-cachyos-q25 (7.3.3) + zinwa-q25-dkms
│   ├── cachyos-mt6789.config         # CachyOS BORE + Landlock + cgroups + Panfrost + ZSTD config fragment
│   ├── config-mt6789-mainline-7.3.aarch64 # Full Linux 7.3.3 MT6789 + BORE kernel configuration
│   └── config-mt6789-mainline-7.0.aarch64 # Full Linux 7.0.0 MT6789 + BORE kernel configuration
├── rootfs/etc/keyd/default.conf     # Physical keyboard layers (docs/keyboard.md)
└── userspace/
    ├── bin/                          # Q25 CLI utilities (arch, bright, q25-status, q25-hotspot)
    ├── sbin/arch-mount               # Zero-overhead Arch Linux ARM rootfs bind-mounter
    ├── systemd/arch-env.service      # Systemd unit mounting /opt/arch at boot
    ├── arch-chroot/                  # Arch rootfs configs (pacman.conf, Wayland wrappers, fish, starship, tmux)
    └── windows/install_fastboot_driver.ps1 # Automated Windows USB & Fastboot driver installer for Q25
```

---

## Live On-Device Hardware Verification (`Linux 7.3.3-cachyos-q25`)

> Early bring-up log (kernel #17). Some details below (LVM rootfs, dropbear/telnetd,
> module list) predate the current image; see **Hardware status** above for the
> current state.

Booted and verified directly on the physical **Zinwa Q25** (`MT6789` / `MT8781`, 12 GB RAM, 256 GB UFS) over **USB NCM SSH (`10.15.19.82`)** with **0 kernel warnings**:

```text
Linux (none) 7.3.3-cachyos-q25-00024-ge50c42ce375d-dirty #17 SMP PREEMPT aarch64 GNU/Linux
BORE CPU Scheduler modification 7.0.0 by Masahito Suzuki (kernel.sched_bore = 1)
```

### Verified Subsystems on Physical Q25 Hardware (`7.3.3-cachyos-q25`)

| Subsystem | Kernel Driver / Module | Device Node / Bus | Hardware Verification Result |
| :--- | :--- | :--- | :--- |
| **CPU & Scheduler** | `mtk-cpufreq-hw` + **CachyOS BORE v7.0.0** | `11bc10.performance-controller` | All 8 cores (`6x Cortex-A55` + `2x Cortex-A76`) online with DVFS & `kernel.sched_bore = 1`. |
| **Memory** | Mainline arm64 MM | `memory@40000000` | `MemTotal: 11804604 kB` (12 GB LPDDR4X). |
| **SCPSYS Power Domains** | `mtk-power-controller` (`CONFIG_MTK_SCPSYS_PM_DOMAINS=y`) | `10006000.syscon:power-controller` | `disp` (`on`), `mfg0` → `mfg1` → `mfg2`/`mfg3` (GPU PM domains), and `audio` registered in `pm_genpd_summary`. |
| **Mali-G57 MC2 GPU & DVFS** | `panfrost` + `mediatek-regulator-coupler` | `13000000.gpu` (`/dev/dri/card1`, `renderD128`) | Open-source **Panfrost 1.6.0** active on `mali-g57 id 0x9093` (`Mesa 26.2.6` OpenGL 3.1 / ES 3.1) with `vgpu`↔`vsram_gpu` regulator coupling across all 36 OPPs (`390 MHz` – `1.1 GHz`). |
| **Native DRM/KMS/DSI Display** | `mediatek-drm` (`mmsys` + `ovl0` + `rdma0` + `dsi0` + `mipi_tx0` + `panel-q20-hd720-lcm-dsi-vdo` + `ocp2131_drv`) | `14000000.syscon`, `14005000.ovl`, `14007000.rdma`, `14013000.dsi`, `/dev/dri/card2` | Native `card2-DSI-1` connected & enabled at `720x720@60Hz` (`mediatekdrmfb` on `/dev/fb0`), hardware vblank IRQs firing at 60 Hz on `14005000.ovl`, PWM backlight active (`1100e000.pwm`). |
| **Arch Linux ARM Wayland / Phosh GUI** | `phoc 0.59.0` + `phosh 0.59.0` + `foot` + `seatd` + `upowerd` + `q25-logind-shim` | `wayland-0` on `DSI-1` (`720x720`) | Full graphical shell & terminal running on `wayland-0` accelerated by Panfrost (`Mali-G57 MC2`), with touchscreen, physical QWERTY keyboard (`keyd`), optical trackpad cursor, and `org.freedesktop.login1` backlight control. |
| **PMIC AuxADC & 7 Thermal Zones** | `mt6359-auxadc` + `generic-adc-thermal` | `iio:device0`, `/sys/class/thermal/thermal_zone0..6` | 14 IIO ADC channels & 7 live thermal zones: `soc-thermal` (39.5°C), `cpu-thermal` (46.8°C), `gpu-thermal` (46.5°C), `vcore-thermal` (47.3°C), `battery-thermal` (31.6°C), `upm6922-charger` (31.3°C), `battery` (31.3°C). |
| **ALSA Audio Card & Smart PA** | `mt6358-sound` + `snd_soc_oca72xxx` (`ZinwaQ25Audio`) | Card 0 (`ZinwaQ25Audio`), `3-0058` (`oca72xxx_pa`) | Registered ALSA Card 0 (`ZinwaQ25Audio`: MT6358 PMIC codec + OCA7255 Smart PA) with `firmware/ocs72xxx_cf.bin` ACF firmware loaded (`Music`, `Receiver`, `Off` profiles). |
| **MMSYS, PWM, APDMA, Systimer** | `clk-mt6789-mm`, `pwm-mediatek`, `mediatek-disp-pwm`, `mtk_uart_apdma`, `timer-mediatek` | `14000000.syscon`, `10048000.pwm`, `1100e000.pwm`, `10217800.dma-controller`, `10017000.timer` | MMSYS clocks, SoC PWM, Display PWM, UART APDMA, and GICv3 IRQ 297 `systimer@10017000` probed cleanly. |
| **UFS 256 GB Storage & LVM** | `ufshcd-mtk` + `dm-mod` | `11270000.ufs` (`/dev/mapper/q25-q25--rootfs`) | `SKhynix H9QT1GGCN6X147` 256 GB UFS with all 61 GPT partitions and 227.8 GB LVM rootfs mounted at `/rootfs` + `/opt/arch` Arch Linux ARM environment. |
| **microSD Slot** | `mtk-msdc` | `11240000.mmc` | Probed cleanly (`returned 0`). |
| **QWERTY Keyboard, Trackpad & `keyd`** | `bbqX0kbd` + `q20_keyboard` + `keyd` | `6-001f` (`Q25_keyboard`, `event2`, `event4`, `event5`) | Firmware `0x11` verified, keyboard backlight enabled (`0xFF` -> reg `0x05`), `/proc/q20_switch_key_mouse` & `keyd` virtual keyboard/pointer daemon running. |
| **Touchscreen (720x720)** | `synaptics_dsx_q25` | `0-0067` (`s2716b_ts`, `event3`) | Synaptics S2716B RMI4 touchscreen bound on I2C0 and registered as `input3`. |
| **Battery Fuel Gauge & Chargers** | `upm6922_charger` + `upm6722` | `5-006b` (`upm6922-charger`, `battery`), `5-0065` (`upm6722-standalone`) | Live `/sys/class/power_supply/battery` fuel gauge (`CAPACITY=100%`, `VOLTAGE_NOW=4352000` uV, `CURRENT_NOW=-250000` uA, `TEMP=315` [31.5°C]) + `upm6922-charger` & `usb-otg-vbus` regulator. |
| **Fast Charge Pump** | `upm6722` | `5-0065` (`upm6722-standalone`) | Standalone charge pump probed on I2C5; `/sys/class/power_supply/upm6722-standalone` registered. |
| **Smart Audio PA** | `snd_soc_oca72xxx` | `3-0058` (`oca72xxx_pa`) | Orient-Chip OCA72xxx chip ID `0x11` (`pid_76` class) verified on I2C3. |
| **NFC Controller** | `st21nfc` | `3-0008` (`st21nfc`) | ST21NFC double-pulse reset & NCI ping (34B + 15B responses) verified on I2C3 (`irq_gpio = 513`). |
| **Camera Flash / Torch** | `aw36518` + `flashlight_gpio_v4l2` | V4L2 subdevs | Both flash/torch modules loaded and initialized cleanly. |
| **Buttons & RGB LEDs** | `mtk-pmic-keys`, `gpio-keys`, `leds-gpio` | `input0`, `input1`, `/sys/class/leds/` | Power/Volume/PTT buttons and `red`/`green`/`blue` GPIO LEDs active. |
| **USB NCM SSH** | `musb-mtk` + `usb_f_ncm` (`CONFIG_USB_INVENTRA_DMA=y`) | `11200000.usb` (`usb0`) | Enumerates `usb0` (`10.15.19.82`) with `dropbear` SSH & `telnetd`. |

### Critical Mainline Linux `7.3.3` Hardware Fixes (`patches/mt6789-soc/0024-*.patch`)

1. **MT6789 I2C Wrapper Clock Parent Fix (`SError` on I2C Probe):**
   - In `drivers/clk/mediatek/clk-mt6789-imp_iic_wrap.c`, the 10 `imp_iic_wrap_*` clocks referenced a non-existent parent `"top_i2c"` instead of `"i2c_pseudo_ck"` (defined in `clk-mt6789-topckgen.c`), and `GATE_IFRAO0..5` in `clk-mt6789-infra_ao.c` lacked `CLK_OPS_PARENT_ENABLE`. Consequently, `mtk_i2c_probe()` accessed unclocked I2C wrapper APB registers and triggered a fatal hardware `SError`. Fixing the parent clock names to `"i2c_pseudo_ck"` (along with `"img1_ck"` in `clk-mt6789-img.c` and `"armpll_bl0"` in `clk-mt6789-mcu.c`) and adding `CLK_OPS_PARENT_ENABLE` brings all 5 Q25 I2C buses (`i2c-0`, `i2c-3`, `i2c-5`, `i2c-6`, `i2c-7`) online cleanly.
2. **ARMv8 Arch Timer & GICv3 PPI Partition Phandle Fix:**
   - Removed the broken `&ppi_cluster0` 4th interrupt cell from the root `timer` and `gic` nodes in `mt6789.dtsi` and set the arch timer trigger flags to `IRQ_TYPE_LEVEL_LOW` so `arch_timer_of_init()` registers the system clocksource on all 8 CPUs.
3. **MediaTek MUSB Inventra DMA (`CONFIG_USB_INVENTRA_DMA=y`):**
   - Enabled `CONFIG_USB_INVENTRA_DMA=y` so `musb-mtk` (`11200000.usb`) populates `dma_init = musbhs_dma_controller_create_noirq` and registers `/sys/class/udc/musb-hdrc.1.auto` for USB NCM (`usb0`).
4. **SimpleDRM Reserved Framebuffer (`no-map` + `memory-region`):**
   - Added `no-map;` to `reserved-memory/framebuffer@ff97c000` and `memory-region = <&framebuffer_mem>;` to `chosen/framebuffer@ff97c000` in `mt8781-zinwa-q25.dts` so `simpledrm` maps the 720x720 bootloader framebuffer via `MEMREMAP_WC` without conflicting with `System RAM`.
5. **SCPSYS Power Domains (`MTK_SCPD_ALWAYS_ON` Preservation) & Open-Source Panfrost Mali-G57 MC2 GPU:**
   - Added `drivers/pmdomain/mediatek/mt6789-pm-domains.h` and wired `mt6789_scpsys_data` in `mtk-pm-domains.c`, preserving already-on `MTK_SCPD_ALWAYS_ON` domains (`MT6789_POWER_DOMAIN_DISP`) across probe deferral so `simpledrm` is never torn down.
   - In `drivers/clk/mediatek/clk-mt6789-mfgcfg.c`, cleared the `MFG_TOP_CONFIG` (`0x13fbf000`) external clock gates (`0x950`, `0x8c0`) and AXI bridge sleep masks (`0x20`, `0x180`) before registering `clk-mt6789-mfgcfg`.
   - In `drivers/gpu/drm/panfrost/panfrost_drv.c`, placed `"mediatek,mt6789-mali"` before the generic `"arm,mali-valhall-jm"` entry in `dt_match[]` so `panfrost` binds with `mediatek_mt6789_data` (`core0`=`mfg2`, `core1`=`mfg3` PM domains) and initializes `mali-g57 id 0x9093` on `/dev/dri/card1` and `/dev/dri/renderD128`.

---

## Out-of-Tree Hardware Drivers (`drivers/`) — Ported to Linux `7.3.3` & `7.0`

All 11 hardware kernel modules from the vendor `5.10` kernel (`LineageOS/android_kernel_xelex_mt6789` / `JamiKettunen/android_kernel_zinwa_mt6789`) have been decoupled from legacy Android/MTK vendor frameworks and forward-ported to compile cleanly with **0 errors and 0 warnings** against **Mainline Linux `7.3.3`** (`7.3.3-cachyos-q25`) as well as **Linux `7.0`**:

| Module (`.ko`) | Bus / Address | Compatible String | Hardware Function & Porting Highlights |
| :--- | :--- | :--- | :--- |
| `bbqX0kbd.ko` | `&i2c6` `@ 0x1f` | `wallComputer,bbqX0kbd` | Physical BlackBerry Q25 QWERTY keyboard, toolbelt keys, optical trackpad (arrow/mouse toggle), and keyboard/screen backlight control (`/proc/Guard_Event`). Ported to Linux 7.0 `i2c_driver` API; fixed vendor `irq-gpio` / `int-gpios` DT fallback bug. |
| `q20_keyboard.ko` | Platform | `wallComputer,pdrv_bbqX0kbd` | Keyboard MCU power rail sequencing (`VDD` GPIO 159, `1V8` GPIO 157, `2V8` GPIO 158) and firmware download GPIO 47 (`/proc/q20_firmware_upgrade`). |
| `synaptics_dsx_q25.ko` | `&i2c0` `@ 0x67` | `mediatek,cap_touch_synaptics` | Synaptics S2716B 720x720 capacitive touchscreen with an optional **One-Euro filter** (`one_euro_enable`, off by default) and commit `2a873a35` **stuck-touch release fix**. Decoupled from legacy MTK `tpd` framework to register a native Linux `input_dev` directly on Mainline. |
| `ocp2131_drv.ko` | `&i2c7` `@ 0x3e` | `mediatek,mtk-gateic-drv-ocp2131` | OCP2131 ±5.8V LCD bias IC (`AVDD`/`AVEE`, gate-power GPIO 84). Fixed double-unregister bug on removal. |
| `panel-q20-hd720-lcm-dsi-vdo.ko` | `&dsi0` `@ 0` | `mediatek,q20_panel_general,vdo` | Zinwa Q25 720x720@60Hz MIPI DSI video-mode DRM panel (`reset` GPIO 85, `AVDD` GPIO 156). Ported to Linux 7.0 `drm_panel`, `mipi_dsi_driver`, and `<linux/backlight.h>` APIs. |
| `upm6922_charger.ko` | `&i2c5` `@ 0x6b` | `up,upm6922_charger` | Unisemipower UPM6922 primary switching charger (3A charge, BC1.2 detection, `upm6922-charger` `power_supply`, and `usb-otg-vbus` 5V/1.2A boost `regulator_dev`). Ported to Linux 7.0 `power_supply_config.fwnode` & bitmask `usb_types`. |
| `upm6722.ko` | `&i2c5` `@ 0x65` | `unisemipower,upm6722-standalone` | Unisemipower UPM6722 standalone switched-capacitor fast-charge pump with internal 10-channel ADC (`VBAT`, `IBAT`, `VBUS`, `IBUS`, `TDIE`). |
| `snd-soc-oca72xxx.ko` | `&i2c3` `@ 0x58` + `0x11210000` | `ocs,oca72xxx_pa` | Orient-Chip OCA72xxx Smart Audio Amplifier (`ASoC` component & firmware loader for `ocs72xxx_cf.bin`) + **Mainline MT6789 AFE PCM & MT6358 ASoC Machine Driver** (`q25_afe_pcm.c`, Card 0 `ZinwaQ25Audio`: 48kHz Playback via DL1/ADDA + Stereo Microphone Capture via VUL12/MTKAIF + Speaker/Receiver/Headphone routing). |
| `st21nfc.ko` | `&i2c3` `@ 0x08` | `st,st21nfc` | STMicroelectronics ST21NFC controller (`/dev/st21nfc`, IRQ GPIO 1, Reset GPIO 150, ClkReq GPIO 68). |
| `aw36518.ko` | I2C `@ 0x63` | `mediatek,aw36518` | AW36518 V4L2 camera flash / torch LED subdev driver with thermal cooling device support. |
| `flashlight-gpio-v4l2.ko` | I2C / GPIO | `mediatek,flashlightgpio` | Pinctrl/GPIO V4L2 camera flash / torch subdev driver; plus direct GPIO 154 (`q25:white:torch` / `/proc/q20_torch`) & MT6358 `vibr` haptics (`/proc/q20_vibrator`) integrated in `bbqX0kbd.ko`. |
| `wmt_drv.ko`, `wlan_drv_gen4m_6789.ko`, `bt_drv_connac1x.ko`, `btif_drv.ko`, `connadp.ko`, `connfem.ko`, `wmt_chrdev_wifi.ko` | `0x18000000` (`CONNSYS`) | `mediatek,mt6789-consys` | Forward-ported MediaTek CONNSYS Combo (`MT6631` A-die), Dual-Band 802.11a/b/g/n/ac Wi-Fi (`wlan0`, `wlan1`, `p2p0`, `ap0`), and Bluetooth 5.4 (`hci0` via `q25-bt-vhci` + `q25-wmt-init`). |
| `ccci_md_all.ko`, `ccci_dpmaif.ko`, `ccci_ccif.ko`, `ccci_cldma.ko`, `ccci_util_lib.ko`, `ccmni.ko`, `conn_md_drv.ko`, `ccci_auxadc.ko`, `ccci_rtc.ko` | `0x10209000` (`DPMAIF`/`CCIF`) | `mediatek,mddriver` | Forward-ported MediaTek ECCCI / DPMAIF / CCMNI 4G LTE Cellular Modem stack (`ccmni0`..`ccmni20`, `/dev/ttyC0`, non-linear SKB SMEM UL DMA fix, DL BAT ring fix, and standalone Bionic property/wakelock shim `libprop_preload.so` + `q25-modem-init`). |
| `q25_sensors.ko` + mainline `st_accel` | AP I2C1 (`GPIO132/133`) & I2C9 (`GPIO148/149`) | `q25_sensors`, `st_accel_i2c` | **Silan SC7A20** accelerometer (`0x18`, mainline st_accel), **MEMSIC MMC5603NJ** magnetometer (`0x30`) and **TYHX HX32062SE** raw ALS & proximity (`0x2b`) via `q25_sensors`. |

### Building the Kernel Modules (`drivers/`)

#### 1. Native or Cross-Compilation against any Linux 6.x / 7.0+ Kernel Tree
```bash
# Cross-compile all 11 modules for arm64 against a prepared Linux 7.0 kernel tree:
make -C /path/to/linux-7.0 M=$(pwd)/drivers ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

# Or build natively on the Zinwa Q25:
cd drivers && make KDIR=/lib/modules/$(uname -r)/build
```

#### 2. Automatic DKMS Installation on Arch Linux ARM
```bash
sudo cp -r drivers /usr/src/zinwa-q25-drivers-1.0.0
sudo dkms add -m zinwa-q25-drivers -v 1.0.0
sudo dkms build -m zinwa-q25-drivers -v 1.0.0
sudo dkms install -m zinwa-q25-drivers -v 1.0.0
```

#### 3. Building the Complete `linux-cachyos-q25` + `zinwa-q25-dkms` Packages
```bash
cd cachyos
makepkg -si
```

---

## Dual-Architecture Overview (Current Hybrid + Mainline Path)

1. **Mainline Linux 7.0 + CachyOS (`cachyos/`, `devicetree/`, `drivers/`):**
   - Based on the MT6789 / Helio G99 Mainline Linux `7.0` tree (`ellyq/mtk_g99-mainline`) with open-source **Panfrost Mali-G57 MC2 GPU**, CPUFreq, UFS, MMC, PMIC `MT6358`/`MT6366`, USB2, Display PWM, and I2C, combined with CachyOS's **BORE scheduler**, `CONFIG_SECURITY_LANDLOCK=y`, full cgroups, and the 11 forward-ported Q25 hardware modules in `drivers/`.
2. **Arch Linux ARM Wayland / Phosh Environment (`userspace/`):**
   - Zero-overhead `/opt/arch` bind-mount (`arch-env.service` + `/usr/local/sbin/arch-mount`) sharing `/proc`, `/sys`, `/dev`, `/run` (Wayland `wayland-0`, PipeWire/PulseAudio, DBus), and `/tmp` (X11/Xwayland).
   - Launching the terminal (`Foot`) or connecting via `ssh q25` enters the Arch Linux ARM `fish` shell (`starship` + `tmux` + `fastfetch`).
   - GUI apps installed via `pacman`/`yay` inside Arch (`rustdesk`, `firefox`, etc.) are automatically exported to the Phosh desktop drawer by `/etc/pacman.d/hooks/99-export-desktop-apps.hook` (`/usr/local/bin/arch-sync-desktop`) and launch natively on the Q25 screenwhether started from the desktop drawer, terminal, or SSH.

---

## Custom Q25 Physical Keyboard Layout (`rootfs/etc/keyd/default.conf`)

| Physical Key / Combo | Action |
| :--- | :--- |
| **Green Call Answer** | Tap **Tab**; hold + **W E R S D F Z X C 0** = **F1–F10**, **V/B** = **F11/F12** (no extra Tab) |
| **Red Call End** | Tap **Phosh home/overview** (Super+D, not minimize-all); hold **600 ms** = **one Alt+F4** |
| **Berry/Menu** | **Super**: stock Phosh overview; type to search applications |
| **Back** | **Alt+Left** (browser/file back) |
| **Typing Shift + Back**, or **printed left Shift + Back** | **Alt+Right** (forward), without leaking Shift/Control |
| **Printed Alt / left Alt** | **Shift**: tap = next key, double-tap = Shift lock, or hold |
| **Printed left Shift** | **Control** (existing shortcuts preserved, except the Back alias) |
| **Physical `0`** | **Alt** layer (Sym+`0` types zero) |
| **Sym / right Alt** | Numbers/symbols: tap = next key, double-tap = lock, or hold |
| **Shift + `$`** | **`|`**; unshifted `$` is unchanged |
| **Sym + Backspace / arrows / Home** | **Backspace / normal navigation**, never Delete or punctuation |
| **Trackpad center** | **Enter** in arrow mode; click in mouse mode (unchanged) |
| **Right Shift + `T`** | Toggle trackpad arrow/mouse mode in the kernel |
| **Right Shift + `Q`, `W/S/A/D`** | **Esc**, **PgUp/PgDn/Home/End** |
| **Right Shift + Call End** | Existing **Alt+F4** chord |

The stock Phosh panel shows Shift/Caps and Sym immediately before battery: no badge
for held, **1** for next-key, **•** for locked. Momentary Shift is **not** labeled Caps
Lock. See [keyboard implementation, tests and safe deployment](docs/keyboard.md)
for the pinned keyd package, read-only state protocol and remaining live checks.

---

## Built-In Q25 Helper Commands & GUI Control Center (`userspace/bin/`, `userspace/sbin/`)
- `q25-control-center` — Native **GTK4 + Libadwaita Hardware Control Center GUI** (`720x720` square display optimized) with 3 tabs:
  - **Controls:** Screen Backlight (`10–1023`), Physical Keyboard Backlight (`0–255`), Audio Volume (`PipeWire` + `MT6358` ALSA), Audio Output Route (`Speaker` / `Receiver` / `Headphone`), Optical Trackpad Mode (`Cursor` vs `D-Pad`), Rear Torch LED (`GPIO 154`), and Vibrator Haptic test (`MT6358 vibr`).
  - **Wireless:** Cellular 4G LTE (`ccmni0` packet data toggle, Data Roaming `AT+EROAMING` toggle, Carrier APN setting, live Operator/Signal/IP telemetry), Wi-Fi (`wlan0` scan & connect via NetworkManager), and Bluetooth 5.4 (`hci0` power & BLE/BR-EDR device scan via BlueZ).
  - **System:** Live Battery %, Voltage, Current, Charger & SoC Temps, IP addresses (`wlan0` / `ccmni0` / `usb0`), RGB Status LED toggles (`red` / `green` / `blue`), Kernel/Scheduler info, and one-tap `Foot` Wayland terminal launcher.
- `q25-wmt-init` & `q25-bt-vhci` — Standalone native C daemons initializing MediaTek CONNSYS (`0x6789` / `MT6631`), downloading MCU/Wi-Fi/BT firmware, powering `wlan0`, and bridging `/dev/stpbt` to Linux BlueZ `hci0` (`VHCI`).
- `q25-modem-init [start|data-up|data-down|roaming on|off|status]` — Standalone 4G LTE modem manager & NetAgent AT handshake tool using `libprop_preload.so`.
- `keys` — Print the Q25 physical keyboard shortcut cheatsheet
- `status-q25` (or `q25-status`) — Show live Battery %, charging/discharging current (mA), temperature (°C), screen brightness %, trackpad mode, and WLAN/USB IPs
- `bright [up|down|min|max|1..100%]` — View or set screen brightness from the CLI
- `hotspot-on [SSID] [PASSWORD]` (or `q25-hotspot on`) — Switch MediaTek `/dev/wmtWifi` into AP mode and launch a Wi-Fi Hotspot (defaults to SSID `Zinwa-Q25`, password `q25linux88`)
- `hotspot-off` (or `q25-hotspot off`) — Stop hotspot and restore normal Wi-Fi client mode
- `mouse-toggle` (or `q25-togglemouse`) — Toggle optical trackpad between arrow keys and mouse pointer
- `rustdesk` — Launch **RustDesk** (`aarch64`, works from both the Phosh desktop drawer and any terminal/SSH session)
- `firefox` — Launch **Firefox** on Wayland (`MOZ_ENABLE_WAYLAND=1`)

---

## Sources & Upstream Credits
- [Mainline Linux 7.0 for MediaTek Helio G99 (`ellyq/mtk_g99-mainline`)](https://gitlab.nura.eco/ellyq/mtk_g99-mainline)
- [postmarketOS Zinwa Q25 Device Port (`pmaports!8365`)](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/8365)
- [LineageOS Zinwa Q25 Kernel (`LineageOS/android_kernel_xelex_mt6789`)](https://github.com/LineageOS/android_kernel_xelex_mt6789)
- [JamiKettunen Zinwa Q25 Kernel (`JamiKettunen/android_kernel_zinwa_mt6789`)](https://github.com/JamiKettunen/android_kernel_zinwa_mt6789)
- [CachyOS Kernel Patches (`CachyOS/kernel-patches`)](https://github.com/CachyOS/kernel-patches)
