# Unplug shutdown and interrupted boot — observed evidence

The user reported complete power-off on USB removal, then one boot apparently stuck at `q25-init: switching to /dev/sdc60`. After a user-initiated long power-button reset the desktop returned. Phone access was limited to this urgent diagnosis; no power/thermal settings were changed and no protection was disabled.

## Confirmed shutdown mechanism

Previous-boot kernel journal entries at 2026-11-10 11:14:49 and 11:15:46 (phone local time) contain both:

- `thermal thermal_zone4: battery-thermal: critical temperature reached`
- `reboot: HARDWARE PROTECTION shutdown (Temperature too high)`

The earlier boot also contains an orderly systemd shutdown/unmount sequence. This establishes thermal protection as the immediate cause of those shutdowns, rather than an inference from lost USB networking.

After the successful boot, readings were approximately battery4.362V,100%,28.6°C; SoC38.75°C, CPU46.25°C, GPU45.75°C, Vcore46.75°C. The user said the phone was very cold. UPower reported no warning and critical action `Ignore`; it was not the source of the recorded kernel thermal shutdown. These are snapshots, not proof of battery capacity, sensor accuracy or battery health under load.

The battery zone critical threshold is60°C. The DTS uses `generic-adc-thermal` backed by PMIC BAT_TEMP. The working kernel's `drivers/iio/adc/mt6359-auxadc.c` computes battery temperature as:

```
25000 + ((850 - v_mv) * 1000) / 18  // millidegrees C
```

A zero-voltage reading would therefore become about72.2°C, crossing that threshold. **No unplug-time ADC trace was captured**, so a lost thermistor bias, ADC/mapping/calibration problem, wiring fault or another cause of an erroneous measurement is not yet distinguished. Do not present the hypothetical zero reading as measured evidence. A cold phone strongly suggests a false thermal indication, but does not justify disabling battery safety checks.

Charger register07 was0x4c (BATFET-disable bit0x20 clear); fault register09 was0. These plugged-in snapshots do not characterize the disconnect transition. No charger register was written.

## Separate boot crash

Saved pstore records identify a boot-time modem module fault:

```
memcpy_fromio+0xc
find_ccci_tag_inf+0x128 [ccci_util_lib]
ccci_util_fo_init
```

The faulting virtual address is `ffff80018353ffff`. The current source's tag walker dereferences offsets from boot metadata without validating them against the mapped blob. It also reads a four-byte payload value even when debug logging is disabled. This warrants defensive parser validation independently of the thermal problem. It does not establish why metadata was corrupt, or prove that every apparent boot delay has this cause.

The subsequent pstore record faults during module-load unwinding. The filesystem handoff message alone was not evidence of filesystem damage; logs show systemd/sshd started during the problematic boot.

An offline defensive fix now validates version-specific tag headers, count, chain/payload bounds and fixed-width names before any MMIO read; it removes the unconditional debug peek and checks modem-table lengths/counts/IDs. Production functions are exercised by `tests/run-ccci-util-lib-fo-tests.py` with checked MMIO mocks and sanitizers. This has **not been installed on the phone** and does not repair corrupt boot metadata; it aims to reject it without crashing the kernel.

The embedded DTS hard-codes `ccci,modem_info_v2` (little-endian structure: tag base0xbf7f0000, size0x1828, version3, count27) and reserves64KiB at that address. The working kernel substitutes its embedded DT for the bootloader DT. A stale or different bootloader handoff is therefore a hypothesis worth checking, not an established corruption cause. No DT, early-boot memory mapping or modem memory layout was changed.

## Preserved artifacts and next checks

Host-only artifacts: `~/q25-work/power-unplug/{shutdown-cause.log,dmesg-ramoops-0,dmesg-ramoops-1}`. No phone/user data was erased. No kernel or module was replaced during diagnosis.

## Linked modem-module candidate (not deployed)

The parser fix from `8e39b6f` now has a normal Kbuild-linked candidate at `~/q25-work/boot-tag-hardening/artifacts/module-8e39b6f/candidate/ccci_util_lib.ko`, SHA-256 `97ad582a75cf964c13c2ee8a932108f1c40ed227fb5565eb2f0006b6e00e01e2`. Parent independently checked identity and the artifact verifier:39 exports unchanged,66 kernel imports resolved, strict modpost passed, target build#37/vermagic matched, and shared build inputs unchanged. This is build validation, not on-device acceptance.

The archived baseline is a **host** module, not a verified copy of the currently installed phone module. Before deployment, independently back up the actual installed module and check the running kernel/config, lookup path and any initramfs copy. The driver has no cleanup/unload entry despite `mod_unload` in kernel vermagic. Do not force-unload it or attempt a hot replacement; plan a later authorized reboot with stable power and recovery access. Existing recovery points should be reused where suitable. This candidate does not change or fix the thermal sensor.

Before a later controlled unplug test, compare the stock BAT_TEMP channel/bias and conversion implementation, arrange persistent high-rate **hardware-only** readings, and retain recovery access. Do not raise critical thresholds or disable thermal protection as a substitute for fixing bad measurements. The user has departed with the phone; further access requires the hotel handoff. Offline boot-tag hardening is being tested separately.
