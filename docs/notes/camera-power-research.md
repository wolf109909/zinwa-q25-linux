# Q25 camera power: offline datasheet follow-up

This is source research, **not a camera driver or permission to power hardware**.
No phone access, I²C operation, GPIO/clock/regulator change, kernel modification,
reboot or flash was used for this follow-up.

## Document provenance

The manufacturer [WL2868C product page](https://www.ovt.com/products/wl2868c/)
identifies a seven-channel camera PMIC. A publicly accessible
[mirrored Will Semiconductor datasheet](https://cdn.semikey.com/upload/pdfs/00/13/001390848b75f4837b6062426ee2eb6e.pdf)
is marked **December 2020, Rev 1.1** throughout. Its SHA-256 is:

```text
94ec3528ae0c5ff02bc291023a1cdaab39fa9df06dccef953fbb35beddad0d34
```

Page references below are the document's printed page numbers. This mirror is
not a manufacturer-authenticated download. Its model/register descriptions
corroborate the preserved vendor source, but the exact part revision fitted to
this Q25 has **not** been identified. Do not treat an online listing for Rev 1.4
as the revision of this PDF.

## What this resolves

Earlier offline camera research left the meaning of enable-register bit 7
unknown. The document explicitly specifies:

| Subject | Document evidence | Consequence for a future port |
| --- | --- | --- |
| `0x0e[7]` | `SYS_EN`, pp. 24, 35 | Master PMIC enable; **not** an eighth output, fixed 3.3 V rail or LDO7 enable. |
| `0x0e[6:0]` | LDO7 through LDO1, p. 35 | Per-output enables apply when the corresponding sequence field is zero. Preserve unrelated outputs. |
| `SYS_EN=0`, `RST_N=1` | Register configuration and I²C retained, p. 24 | Software disable differs from hardware reset; neither is a harmless identification operation. |
| UVLO latch | Software shutdown sets `UVLO_LATCHED_STS`; clear before re-enable, pp. 24, 42 | Reset/enable sequencing needs explicit fault handling, not a copied vendor probe sequence. |
| Output discharge | `0x02[6:0]`: zero enables discharge, pp. 25, 33 | A disable can actively discharge a rail; it is not merely relinquishing ownership. |
| LDO1/2 selectors | `496 mV + n × 8 mV`, normal `n=0..127`, p. 33 | 128 ordinary selectors. Extended eight-bit range requires trim; do not enable it by inference. |
| LDO3–7 selectors | `1504 mV + n × 8 mV`, `n=0..255`, p. 34 | 256 selectors, including the maximum; avoid the vendor driver's endpoint-count error. |
| Revision IDs | Read-only `0x00`/`0x01`, listed resets `0x82`/`0x33`, pp. 32–33 | Document values only, **not** measured device identity or safe-probe evidence. |
| I²C address | Default `0x2f`; writable remap at `0x1e`, p. 41 | Consistent with stock DT. Do not write remap or scan the shared bus. |

The selector arithmetic also confirms that the vendor initialization value
`0x45` for LDO1 denotes **1.048 V**, not its source comment's 1.2 V. The later
stock rear-camera power request is 1.0 V (`0x3f`); the front's 1.2 V is `0x58`.
These are software requests, not measured voltages or proof of sensor tolerance.

## Upstream work exists, but is not an accepted implementation

A [November 2026 regulator-binding review](https://lore.jethome.com/lkml/20261106-lucrative-sadden-741ffe6c43e9@spud/1-a.txt)
describes the same chip-wide `SYS_EN` and seven outputs. The reviewer explicitly
marks that submission **changes-requested**. This is a lead for a future
upstream-compatible port, not evidence that the driver/binding is merged,
correct for this board, or ready to deploy. Input-supply topology and reset
handling need review against the exact document revision and actual board.

## What remains blocked

This narrows one documentation gap; it does not close the electrical contract:

- Q25 supply wiring, exact PMIC revision, all shared consumers, and safe takeover
  of GPIO15 and existing PMIC state remain unverified. I²C5 also serves charging.
- Front-camera PDN/AFVDD purpose and the actual MCLK TG route/ownership are not
  established. Stock `SensorMCLK` selects pinctrl; it does not prove a running
  24 MHz clock. No guessed GPIO polarity or shared-PLL programming is justified.
- Native IMX111 probe writes initialization registers; it is not an ID-only
  read. A genuinely bounded identification path still needs reviewed power,
  clock, reset and failure-unwind handling.
- CAM bus-protection data, CSI/SENINF routing, CAMSV capture and safe DMA/IOMMU
  integration remain prerequisites for image capture. A regulator document
  supplies none of those pieces.

Keep production camera nodes disabled. Do not import the vendor regulator's
probe-time output reset, selector rewrites or `0x80` write as a shortcut. Preserve
working display, modem and charging while designing an inactive implementation.
