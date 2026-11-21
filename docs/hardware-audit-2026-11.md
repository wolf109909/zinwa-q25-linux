# Q25 port audit against the stock vendor kernel — 2026-11-21

The earlier port was checked driver-by-driver and node-by-node against the
vendor 5.10 kernel (LineageOS `android_kernel_xelex_mt6789`), the **stock DT
extracted from the phone** (`vendor_boot` + `dtbo`, saved under
`~/q25-work/vendor-dtb/`) and the stock vendor partition (audio params, SCP
firmware). Many "working" subsystems only probed; their values were invented.
Rule of thumb that came out of this: **a driver that probes and prints numbers
is not verified — compare the numbers with the vendor and with physics.**

## Fixed (root causes)

| Area | Symptom | Root cause | Fix |
|---|---|---|---|
| Battery thermal | Phone powered off on USB unplug / ~31 s after battery boot | `vbif28` (NTC pull-up) had no DT consumer → regulator late cleanup turned it off; BAT_TEMP then read ~0 V and an **invented linear formula** mapped that to ~72 °C | `vbif28` always-on; BAT_TEMP reports mV (patch 0025) and DT `temperature-lookup-table` from the vendor 10 k NTC / 24 k / 2.8 V |
| SIM | Flaky network / SMS "No network" | `vsim1` cut by late cleanup at ~31 s under the running modem; `vsim2`/`vpa` same class | Modem-owned rails have no DT node (as in the vendor DT) |
| PMIC IRQ | ~20 000 IRQ/s storm once ACCDET enabled | Mainline `MT6358_AUD_TOP`/`MISC_TOP` bits off by one (vendor: 7/8) | Patch 0027 |
| Speaker | Loud hiss | LOL mux value 2 + HS/HP drivers on; vendor `mt_lo_event` uses mux 1 (= DAC R; the enum label "Mute" is misleading) | Vendor LO sequence |
| Speaker | Overdrive, hot smell | LO gain up to +8 dB; stock `PlaybackVolAna` uses -2 dB fixed, digital volume | UCM fixed gains, `snd_soc_limit_volume` caps (SPK -2 dB, HP -1 dB) |
| Speaker/earpiece | Left channel missing; "earpiece" was the speaker | Mono outputs fed from one DAC channel; receiver route used the PA | (L+R)/2 via `AFE_CONN_RS`; earpiece = MT6358 HS from DAC L (`mt_rcv_event`), PA off |
| Headphones | No detection; hiss/pops | No ACCDET driver; HP sequence skipped all ramps | `q25_mt6358_accdet.c` + mfd cell (patch 0026); vendor `mtk_hp_enable/disable` |
| Audio | Clicks | SRAM refreshed only on period IRQ; PipeWire writes late in the period | `.ack` + `SNDRV_PCM_INFO_SYNC_APPLPTR` |
| Audio | Wrong pitch at 96/192 k, garbled S24 | memif rate codes off by one; HD bits wrong | Vendor `MTK_AFE_RATE_*`, `mtk_memif_set_format` |
| microSD | Never detected | cd polarity inverted; vqmmc min 2.9 V vs UHS; vendor-enum `drive-strength = <3>` aborted the whole pinctrl state | ACTIVE_LOW, 1.8–3.3 V, 8 mA |
| Charger | Always offline, no uevents | Attach came from the vendor TCPC, which mainline lacks | VBUS IRQ emulates the TCPC trigger |
| Charger | CV read 3864 mV; precharge 960 mA | Register **address** used as value; inverted clamp | Fixed |
| Fuel gauge | Fake %, -250 mA constant | Charger driver faked the battery psy | `q25_mt6358_gauge.c`: MT6366 FG coulomb counter + vendor OCV table (Rsense 1 mΩ verified: -300 mA idle) |
| PMIC temps | Invented formula | Vendor uses efuse-calibrated conversion | Patch 0029 |
| Magnetometer | Y/Z pinned at full scale | "ODR" written to Internal Control 1 → self-test current + inhibit | MMC5603 register map; mount matrix from SCP firmware |
| Keyboard | Every keystroke in the persistent journal | Vendor debug level left on | `DEBUG_LEVEL_OFF` |

## Open / not done

- Calls/SMS: need the host IMS stack (see `telephony-progress.md`); no live tests without explicit permission.
- ALS lux: computed inside the SCP firmware with per-part efuse offsets; only raw channels are exposed (no auto-brightness).
- MTKAIF: port uses protocol 2 without calibration (vendor: protocol 1); capture measures clean, left as is.
- Possible ICM-4N607 accel/gyro listed in the stock SCP config; not investigated.
- Speaker DSP: user tuning in `tools/audio/` (PipeWire filter-chain generator); not installed (CPU cost).
