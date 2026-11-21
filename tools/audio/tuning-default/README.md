# Zinwa Q25 MediaTek AudioParam XML Package (`default`)

100% native MediaTek `BesLoudness` (`SWIPRev 1312`) configuration files for **Zinwa / Xelex Q25 (`MT6789` / `q20_v12_factory`)**.
No external app, daemon, or service is required once these two XML files are placed in the firmware image.

---

## Firmware Integration Paths (`/vendor/etc/audio_param/`)

- **`PlaybackACF_AudioParam.xml`** → `/vendor/etc/audio_param/PlaybackACF_AudioParam.xml`
  - `Speaker` (`param_id="0"`): High-Pass Filter (`bes_loudness_L_hpf_fc` / `order`), Low-Pass Filter (`bes_loudness_L_lpf_fc` / `order`), and 8-band BPF Parametric EQ (`bes_loudness_L_bpf_fc`, `bes_loudness_L_bpf_bw`, `bes_loudness_L_bpf_gain` in Q24.8 `1/256 dB`).
- **`PlaybackDRC_AudioParam.xml`** → `/vendor/etc/audio_param/PlaybackDRC_AudioParam.xml`
  - `Ring` (`param_id="0"`) & `Music` (`param_id="1"`): Multi-Band DRC crossover frequencies (`bes_loudness_Cross_Freq`), 5-point Q24.8 transfer curves (`DRC_Th` / `DRC_Gn`), sub-band gains (`SB_Gn`), and 16-bit linear Limiter (`Lim_Th` / `Lim_Gn`).

---

## Exact Tuned Parameter Summary (`default`)

```text
=== Q25 Speaker DSP Tuner (100% MediaTek PlaybackACF / PlaybackDRC Native Mode) ===
  Mode          : ENABLED (effective=ACTIVE, SWIPRev=1312)
  Speaker-Only  : true (speakerActive=true, route=AUDIO_DEVICE_OUT_SPEAKER, AUDIO_DEVICE_OUT_SPEAKER[ACTIVE])
  HPF (MTK ACF) : ON (fc=159 Hz [0x9F], order=4 [24 dB/oct, 0x4], Butterworth Q=0.71)
  LPF (MTK ACF) : ON (fc=6500 Hz [0x1964], order=1 [6 dB/oct, 0x1], Butterworth Q=0.71)
  Attached Audio Sessions (2):
    - session 105  [com.netease.cloudmusic      ] -> dp=OK (enabled=true, control=true)
    - session 113  [com.netease.cloudmusic      ] -> dp=OK (enabled=true, control=true)
=== MediaTek PlaybackACF 8-Band BPF EQ (bes_loudness_L_bpf) (ENABLED, 8 MTK BPF bands) ===
  Band 0 (ON ) [fc=  107 Hz (0x006B) | bw=   98 Hz (0x0062) | Q=1.09]: +10.84 dB (Q24.8=2774)
  Band 1 (ON ) [fc=  284 Hz (0x011C) | bw=  181 Hz (0x00B5) | Q=1.57]:  -1.63 dB (Q24.8=-416)
  Band 2 (ON ) [fc=  501 Hz (0x01F5) | bw=  196 Hz (0x00C4) | Q=2.56]: -14.53 dB (Q24.8=-3719)
  Band 3 (ON ) [fc=  985 Hz (0x03D9) | bw=  263 Hz (0x0107) | Q=3.75]: -11.80 dB (Q24.8=-3020)
  Band 4 (ON ) [fc= 1969 Hz (0x07B1) | bw=  307 Hz (0x0133) | Q=6.41]:  -5.04 dB (Q24.8=-1289)
  Band 5 (ON ) [fc= 3821 Hz (0x0EED) | bw= 6704 Hz (0x1A30) | Q=0.57]: -18.00 dB (Q24.8=-4608)
  Band 6 (ON ) [fc= 4950 Hz (0x1356) | bw=  991 Hz (0x03DF) | Q=4.99]:  -4.11 dB (Q24.8=-1053)
  Band 7 (ON ) [fc=10177 Hz (0x27C1) | bw= 5685 Hz (0x1635) | Q=1.79]:  -7.00 dB (Q24.8=-1792)
=== Multi-Band DRC / Compressor (ENABLED, 4 bands) ===
  Band 0 [   <=   200 Hz] (ON ): thresh= -12.5 dB, ratio= 6.2:1, att= 10.0 ms, rel= 80.0 ms, knee= 6.0 dB, gate= -90.0 dB, exp=1.0:1, pre= +0.0 dB, post= +0.0 dB
  Band 1 [  200..  500 Hz] (ON ): thresh=  +0.0 dB, ratio= 3.0:1, att= 10.0 ms, rel= 80.0 ms, knee= 6.0 dB, gate= -75.0 dB, exp=1.0:1, pre= +0.0 dB, post= +0.0 dB
  Band 2 [  500.. 8000 Hz] (ON ): thresh= -22.0 dB, ratio= 3.0:1, att= 10.0 ms, rel= 80.0 ms, knee= 6.0 dB, gate= -75.0 dB, exp=1.0:1, pre= +0.0 dB, post= +0.0 dB
  Band 3 [ 8000..16000 Hz] (ON ): thresh= -36.0 dB, ratio= 3.0:1, att= 10.0 ms, rel= 80.0 ms, knee= 9.0 dB, gate= -75.0 dB, exp=1.0:1, pre= +0.0 dB, post= +0.0 dB
=== Brickwall Limiter (ENABLED) ===
  thresh=-3.00 dB, ratio=10.0:1, attack=2.0 ms, release=60.0 ms, postGain=+0.00 dB
```
