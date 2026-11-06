# Zinwa Q25 — Mainline Linux 7.3 + CachyOS & Arch Linux ARM (`MT6789` / `MT8781`)

Complete hardware enablement, forward-ported **Mainline Linux 7.3 (`7.3.3`) & 7.0** out-of-tree kernel modules (DKMS/Kbuild), rebased MT6789 SoC + CachyOS BORE `7.3` patchset (`patches/`), Mainline DeviceTree (`mt8781-zinwa-q25.dts`), CachyOS BORE kernel packaging, and Arch Linux ARM userspace customizations for the **Zinwa Q25** (`Xelex` / `q20_v12_factory`, MediaTek Helio G99 `MT6789`, 12 GB LPDDR4X, 256 GB UFS).

---

## Repository Structure

```text
zinwa-q25-linux/
├── drivers/                  # Forward-ported out-of-tree Q25 drivers for Mainline Linux 7.0 – 7.3+ (Kbuild & DKMS)
│   ├── Makefile              # Top-level Kbuild Makefile (builds all 11 .ko modules)
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
├── patches/
│   ├── mt6789-soc/                   # 23 MT6789 / Helio G99 SoC patches rebased cleanly onto Linux 7.3.3
│   └── cachyos-7.3/                  # CachyOS BORE scheduler patch rebased onto Linux 7.3.3
├── devicetree/
│   ├── mt8781-zinwa-q25.dts          # Mainline Linux 7.0 / 7.3 DeviceTree for Zinwa Q25 with all peripherals wired
│   └── vendor-q20_v12_factory.dts    # Original vendor 5.10 DeviceTree overlay for hardware reference
├── cachyos/
│   ├── PKGBUILD                      # Arch Linux ARM PKGBUILD for linux-cachyos-q25 (7.3.3) + zinwa-q25-dkms
│   ├── cachyos-mt6789.config         # CachyOS BORE + Landlock + cgroups + Panfrost + ZSTD config fragment
│   ├── config-mt6789-mainline-7.3.aarch64 # Full Linux 7.3.3 MT6789 + BORE kernel configuration
│   └── config-mt6789-mainline-7.0.aarch64 # Full Linux 7.0.0 MT6789 + BORE kernel configuration
└── userspace/
    ├── keyd/q25-qwerty-en.conf       # Physical keyboard & toolbelt keyd remapping (Tab, Esc/Ctrl, Sym layer)
    ├── bin/                          # Q25 CLI utilities (arch, bright, q25-status, q25-hotspot)
    ├── sbin/arch-mount               # Zero-overhead Arch Linux ARM rootfs bind-mounter
    ├── systemd/arch-env.service      # Systemd unit mounting /opt/arch at boot
    ├── arch-chroot/                  # Arch rootfs configs (pacman.conf, Wayland wrappers, fish, starship, tmux)
    └── windows/install_fastboot_driver.ps1 # Automated Windows USB & Fastboot driver installer for Q25
```

---

## Out-of-Tree Hardware Drivers (`drivers/`) — Ported to Linux `7.3.3` & `7.0`

All 11 hardware kernel modules from the vendor `5.10` kernel (`LineageOS/android_kernel_xelex_mt6789` / `JamiKettunen/android_kernel_zinwa_mt6789`) have been decoupled from legacy Android/MTK vendor frameworks and forward-ported to compile cleanly with **0 errors and 0 warnings** against **Mainline Linux `7.3.3`** (`7.3.3-cachyos-q25`) as well as **Linux `7.0`**:

| Module (`.ko`) | Bus / Address | Compatible String | Hardware Function & Porting Highlights |
| :--- | :--- | :--- | :--- |
| `bbqX0kbd.ko` | `&i2c6` `@ 0x1f` | `wallComputer,bbqX0kbd` | Physical BlackBerry Q25 QWERTY keyboard, toolbelt keys, optical trackpad (arrow/mouse toggle), and keyboard/screen backlight control (`/proc/Guard_Event`). Ported to Linux 7.0 `i2c_driver` API; fixed vendor `irq-gpio` / `int-gpios` DT fallback bug. |
| `q20_keyboard.ko` | Platform | `wallComputer,pdrv_bbqX0kbd` | Keyboard MCU power rail sequencing (`VDD` GPIO 159, `1V8` GPIO 157, `2V8` GPIO 158) and firmware download GPIO 47 (`/proc/q20_firmware_upgrade`). |
| `synaptics_dsx_q25.ko` | `&i2c0` `@ 0x67` | `mediatek,cap_touch_synaptics` | Synaptics S2716B 720x720 capacitive touchscreen with 120Hz **One-Euro jitter filter** and commit `2a873a35` **stuck-touch release fix**. Decoupled from legacy MTK `tpd` framework to register a native Linux `input_dev` directly on Mainline. |
| `ocp2131_drv.ko` | `&i2c7` `@ 0x3e` | `mediatek,mtk-gateic-drv-ocp2131` | OCP2131 ±5.8V LCD bias IC (`AVDD`/`AVEE`, gate-power GPIO 84). Fixed double-unregister bug on removal. |
| `panel-q20-hd720-lcm-dsi-vdo.ko` | `&dsi0` `@ 0` | `mediatek,q20_panel_general,vdo` | Zinwa Q25 720x720@60Hz MIPI DSI video-mode DRM panel (`reset` GPIO 85, `AVDD` GPIO 156). Ported to Linux 7.0 `drm_panel`, `mipi_dsi_driver`, and `<linux/backlight.h>` APIs. |
| `upm6922_charger.ko` | `&i2c5` `@ 0x6b` | `up,upm6922_charger` | Unisemipower UPM6922 primary switching charger (3A charge, BC1.2 detection, `upm6922-charger` `power_supply`, and `usb-otg-vbus` 5V/1.2A boost `regulator_dev`). Ported to Linux 7.0 `power_supply_config.fwnode` & bitmask `usb_types`. |
| `upm6722.ko` | `&i2c5` `@ 0x65` | `unisemipower,upm6722-standalone` | Unisemipower UPM6722 standalone switched-capacitor fast-charge pump with internal 10-channel ADC (`VBAT`, `IBAT`, `VBUS`, `IBUS`, `TDIE`). |
| `snd-soc-oca72xxx.ko` | `&i2c3` `@ 0x58` | `ocs,oca72xxx_pa` | Orient-Chip OCA72xxx Smart Audio Amplifier (`ASoC` component & firmware loader for `ocs72xxx_cf.bin`). |
| `st21nfc.ko` | `&i2c3` `@ 0x08` | `st,st21nfc` | STMicroelectronics ST21NFC controller (`/dev/st21nfc`, IRQ GPIO 1, Reset GPIO 150, ClkReq GPIO 68). |
| `aw36518.ko` | I2C `@ 0x63` | `mediatek,aw36518` | AW36518 V4L2 camera flash / torch LED subdev driver with thermal cooling device support. |
| `flashlight-gpio-v4l2.ko` | I2C / GPIO | `mediatek,flashlightgpio` | Pinctrl/GPIO V4L2 camera flash / torch subdev driver. |

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
2. **Halium 14 + Arch Linux ARM Chroot (`userspace/`):**
   - Zero-overhead `/opt/arch` bind-mount (`arch-env.service` + `/usr/local/sbin/arch-mount`) sharing `/proc`, `/sys`, `/dev`, `/run` (Wayland `wayland-0`, PipeWire/PulseAudio, DBus), `/tmp` (X11/Xwayland), and `/home/droidian`.
   - Launching the terminal (`Ptyxis`) or connecting via `ssh q25` enters the Arch Linux ARM `fish` shell (`starship` + `tmux` + `fastfetch`).
   - GUI apps installed via `pacman`/`yay` inside Arch (`rustdesk`, `firefox`, etc.) are automatically exported to the Phosh desktop drawer by `/etc/pacman.d/hooks/99-export-desktop-apps.hook` (`/usr/local/bin/arch-sync-desktop`) and launch natively on the Q25 screenwhether started from the desktop drawer, terminal, or SSH.

---

## Custom Q25 Physical Keyboard Layout (`userspace/keyd/q25-qwerty-en.conf`)

| Physical Key / Combo | Action |
| :--- | :--- |
| **Call Answer** (Far-left toolbelt button) | **`Tab`** (Instant shell/code autocomplete) |
| **Call End** (Far-right toolbelt button) | **Tap = `Esc`** \| **Hold = `Ctrl`** (e.g. Hold `Call End` + `C` = `Ctrl+C`) |
| **Back Button** (Toolbelt) | **Tap = `Back`** \| **Hold = `Alt`** |
| **Left Alt** (`alt`, bottom-left) | **`Shift`** (Tap once = capitalize next key, double-tap = lock Shift, or hold) |
| **Right Alt** (`sym`, bottom-right) | **Numbers & Symbols Layer** (Tap once = next key, double-tap = lock, or hold) |
| **Right Shift + `T`** | **Toggle Trackpad Mode** between Arrow Keys and Mouse Cursor (`q25-togglemouse`) |
| **Right Shift + `Volume Up / Down`** | **Screen Brightness Up / Down** (+10% / -10% via `/usr/local/bin/bright`) |
| **Right Shift + `W / S / A / D`** | **`PgUp` / `PgDn` / `Home` / `End`** |
| **Right Shift + `Call End`** | **`Alt + F4`** (Close active window) |
| **Sym + `0`** | **Toggle Physical Keyboard Backlight** |

*(Run `keys` anytime in the `fish` shell to print this cheatsheet on the phone.)*

---

## Built-In Q25 Helper Commands (`userspace/bin/`)
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
