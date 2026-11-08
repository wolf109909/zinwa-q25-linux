// SPDX-License-Identifier: GPL-2.0
/*
 * Zinwa Q25 (MT6789 + MT6358 + OCA72xxx) Native Mainline Linux 7.3.3 AFE & Machine Driver
 *
 * Drives the MT6789 AFE at 0x11210000 (DL1 + VUL12 + ADDA -> MT6358 MTKAIF),
 * manages audio_intbus / aud_afe / aud_dac / aud_adc clocks,
 * switches MTKAIF MOSI/MISO pinctrl (GPIO168-174 MODE1), and triggers OCA72xxx
 * SmartPA profile switching on playback start/stop.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

extern int oca72xxx_set_profile(int dev_index, char *profile);
extern int oca72xxx_add_codec_controls(void *codec);
extern int mt6358_set_mtkaif_protocol(struct snd_soc_component *cmpnt, int mtkaif_protocol);
int q25_register_audio_card(void);
void q25_unregister_audio_card(void);

/* MT6789 AFE Registers (base 0x11210000) */
#define AUDIO_TOP_CON0			0x0000
#define AUDIO_TOP_CON1			0x0004
#define AUDIO_TOP_CON3			0x000c
#define AFE_DAC_CON0			0x0010
#define AFE_CONN0			0x0020
#define AFE_CONN1			0x0024
#define AFE_CONN2			0x0028
#define AFE_CONN3			0x002c
#define AFE_CONN4			0x0030
#define AFE_CONN5			0x0044
#define AFE_CONN_24BIT			0x0048
#define AFE_DL1_CON0			0x004c
#define AFE_DL1_BASE_MSB		0x0050
#define AFE_DL1_BASE			0x0054
#define AFE_DL1_CUR_MSB			0x0058
#define AFE_DL1_CUR			0x005c
#define AFE_DL1_END_MSB			0x0060
#define AFE_DL1_END			0x0064
#define AFE_ADDA_DL_SRC2_CON0		0x0108
#define AFE_ADDA_DL_SRC2_CON1		0x010c
#define AFE_ADDA_UL_SRC_CON0		0x0114
#define AFE_ADDA_UL_SRC_CON1		0x0118
#define AFE_ADDA_TOP_CON0		0x0120
#define AFE_ADDA_UL_DL_CON0		0x0124
#define AFE_ADDA_PREDIS_CON0		0x0260
#define AFE_ADDA_PREDIS_CON1		0x0264
#define AFE_ADDA_IIR_COEF_02_01		0x0290
#define AFE_ADDA_IIR_COEF_04_03		0x0294
#define AFE_ADDA_IIR_COEF_06_05		0x0298
#define AFE_ADDA_IIR_COEF_08_07		0x029c
#define AFE_ADDA_IIR_COEF_10_09		0x02a0
#define AFE_IRQ_MCU_CON1		0x02e4
#define AFE_IRQ_MCU_CON2		0x02e8
#define AFE_IRQ_MCU_CNT0		0x0300
#define AFE_VUL12_CON0			0x0374
#define AFE_VUL12_BASE_MSB		0x0378
#define AFE_VUL12_BASE			0x037c
#define AFE_VUL12_CUR_MSB		0x0380
#define AFE_VUL12_CUR			0x0384
#define AFE_VUL12_END_MSB		0x0388
#define AFE_VUL12_END			0x038c
#define AFE_IRQ_MCU_CON0		0x03a0
#define AFE_IRQ_MCU_STATUS		0x03a4
#define AFE_IRQ_MCU_CLR			0x03a8
#define AFE_IRQ_MCU_EN			0x03b4
#define AFE_HD_ENGEN_ENABLE		0x0dd0
#define AFE_CONN21			0x0470
#define AFE_CONN22			0x0474
#define AFE_IRQ_MCU_CNT13		0x0608
#define AFE_MEMIF_CON0			0x08cc
#define AFE_ADDA_DL_SDM_DCCOMP_CON	0x0c50
#define AFE_ADDA_DL_SDM_DITHER_CON	0x0c70
#define AFE_ADDA_DL_SDM_AUTO_RESET_CON	0x0c74
#define AFE_ADDA_MTKAIF_CFG0		0x0e00
#define AFE_ADDA_MTKAIF_RX_CFG0		0x0e20
#define AFE_ADDA_MTKAIF_RX_CFG2		0x0e28
#define AFE_AUD_PAD_TOP			0x0e40
#define AFE_AGENT_ON			0x1b00
#define AFE_AGENT_ON_SET		0x1b04
#define AFE_AGENT_ON_CLR		0x1b08

/* Interconnections */
#define I_ADDA_UL_CH1			3
#define I_ADDA_UL_CH2			4
#define I_DL1_CH1			5
#define I_DL1_CH2			6

/* AFE_AGENT_ON shifts */
#define DL1_ON_SFT			4
#define VUL12_ON_SFT			31

/* SRAM: 0x11212000, size 0xd000 (52 KiB) */
#define MT6789_AFE_SRAM_PHYS		0x11212000ULL
#define MT6789_AFE_SRAM_SIZE		0xd000
#define MT6789_DL1_SRAM_SIZE		0x6000
#define MT6789_VUL_SRAM_OFFSET		0x6000
#define MT6789_VUL_SRAM_SIZE		0x3000

struct q25_afe_priv {
	void __iomem *base;
	void __iomem *sram_base;
	phys_addr_t sram_phys;
	void *shadow_buf[2];
	void __iomem *gpio_base;
	void __iomem *iocfg_lm_base;
	void __iomem *infracfg_base;
	void __iomem *topckgen_base;
	void __iomem *spm_base;
	int irq;

	struct snd_pcm_substream *substream[2];
	struct snd_soc_component *mt6358_cmpnt;
	int output_route; /* 0=Speaker, 1=Receiver, 2=Headphone, 3=Speaker+Headphone */
	bool spk_pa_enabled;
	spinlock_t lock;
};

static struct q25_afe_priv *g_q25_afe;

static unsigned int q25_rate_to_memif_mode(unsigned int rate)
{
	switch (rate) {
	case 8000: return 0;
	case 11025: return 1;
	case 12000: return 2;
	case 16000: return 4;
	case 22050: return 5;
	case 24000: return 6;
	case 32000: return 8;
	case 44100: return 9;
	case 48000: return 10;
	case 96000: return 11;
	case 192000: return 12;
	default: return 10;
	}
}

static unsigned int q25_rate_to_adda_dl(unsigned int rate)
{
	switch (rate) {
	case 8000: return 0;
	case 11025: return 1;
	case 12000: return 2;
	case 16000: return 3;
	case 22050: return 4;
	case 24000: return 5;
	case 32000: return 6;
	case 44100: return 7;
	case 48000: return 8;
	case 96000: return 9;
	case 192000: return 10;
	default: return 8;
	}
}

static unsigned int q25_rate_to_adda_ul(unsigned int rate)
{
	switch (rate) {
	case 8000: return 0;
	case 16000: return 1;
	case 32000: return 2;
	case 48000: return 3;
	case 96000: return 4;
	case 192000: return 5;
	default: return 3;
	}
}

static void q25_afe_enable_clocks_and_pins(struct q25_afe_priv *afe)
{
	u32 val;
	int i;

	/* 1. Ungate INFRACFG_AO audio clocks:
	 * MODULE_SW_CG_1_CLR (0x8c): bit 25 (infra_audio)
	 * MODULE_SW_CG_2_CLR (0xa8): bit 4 (infra_audio_26m_bclk)
	 */
	if (afe->infracfg_base) {
		writel(BIT(25), afe->infracfg_base + 0x008c);
		writel(BIT(4), afe->infracfg_base + 0x00a8);
	}

	/* 2. Ungate TOPCKGEN audio muxes:
	 * CLK_CFG_7_CLR (0x0088): bit 31 (pdn_audio) -> CLK_CFG_UPDATE1 (0x0008) bit 0
	 * CLK_CFG_8_CLR (0x0098): bit 7 (pdn_aud_intbus) -> CLK_CFG_UPDATE1 (0x0008) bit 1
	 * CLK_CFG_12_CLR (0x00d8): bit 7 (pdn_aud_engen1), bit 15 (pdn_aud_engen2) -> UPDATE1 bits 17, 18
	 * CLK_CFG_13_CLR (0x00e8): bit 7 (pdn_aud_1), bit 15 (pdn_aud_2) -> UPDATE1 bits 21, 22
	 * CLK_CFG_15_CLR (0x0108): bit 7 (pdn_audio_h) -> UPDATE1 bit 29
	 */
	if (afe->topckgen_base) {
		writel(BIT(31), afe->topckgen_base + 0x0088);
		writel(BIT(7), afe->topckgen_base + 0x0098);
		writel(BIT(7) | BIT(15), afe->topckgen_base + 0x00d8);
		writel(BIT(7) | BIT(15), afe->topckgen_base + 0x00e8);
		writel(BIT(7), afe->topckgen_base + 0x0108);
		writel(BIT(0) | BIT(1) | BIT(17) | BIT(18) | BIT(21) | BIT(22) | BIT(29),
		       afe->topckgen_base + 0x0008);
	}

	/* 2b. Power on SPM AUDIO MTCMOS domain (0x10006358) and release bus prot */
	if (afe->spm_base) {
		/* Unlock SPM register write access (POWERON_CONFIG_EN = 0x0b160001) */
		writel(0x0b160001U, afe->spm_base + 0x0000);
		val = readl(afe->spm_base + 0x0358);
		val |= BIT(2); /* PWR_ON */
		writel(val, afe->spm_base + 0x0358);
		val |= BIT(3); /* PWR_ON_2ND */
		writel(val, afe->spm_base + 0x0358);
		for (i = 0; i < 1000; i++) {
			if ((readl(afe->spm_base + 0x016c) & BIT(22)) &&
			    (readl(afe->spm_base + 0x0170) & BIT(22)))
				break;
			udelay(1);
		}
		val &= ~BIT(4); /* ~PWR_CLK_DIS */
		writel(val, afe->spm_base + 0x0358);
		val &= ~BIT(1); /* ~PWR_ISO */
		writel(val, afe->spm_base + 0x0358);
		val |= BIT(0); /* PWR_RST_B */
		writel(val, afe->spm_base + 0x0358);
		val &= ~BIT(8); /* ~SRAM_PDN */
		writel(val, afe->spm_base + 0x0358);
		for (i = 0; i < 1000; i++) {
			if (!(readl(afe->spm_base + 0x0358) & BIT(12)))
				break;
			udelay(1);
		}
	}
	if (afe->infracfg_base) {
		/* Clear TOP_AXI_PROT_EN_2 bit 4 (AUDIO bus protection) */
		writel(BIT(4), afe->infracfg_base + 0x0718);
	}

	/* 3. Set GPIO168..176 to MODE 1 (AUD_CLK_MOSI/MISO, AUD_SYNC_MOSI, AUD_DAT_MOSI0/1, AUD_DAT_MISO0/1/2)
	 * GPIO_MODE21 (0x0450): pins 168..175 -> nibbles 0..7 = 1 -> 0x11111111
	 * GPIO_MODE22 (0x0460): pin 176 -> nibble 0 = 1
	 */
	if (afe->gpio_base) {
		writel(0x11111111U, afe->gpio_base + 0x0450);
		val = readl(afe->gpio_base + 0x0460);
		val &= ~0xfU;
		val |= 0x1U;
		writel(val, afe->gpio_base + 0x0460);
	}
	/* Enable IES + SMT and disable pull-down on GPIO168..176 in IOCFG_LM (0x11e20000) */
	if (afe->iocfg_lm_base) {
		val = readl(afe->iocfg_lm_base + 0x00c0);
		val |= BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8);
		writel(val, afe->iocfg_lm_base + 0x00c0);
		val = readl(afe->iocfg_lm_base + 0x00e0);
		val |= BIT(26);
		writel(val, afe->iocfg_lm_base + 0x00e0);
		val = readl(afe->iocfg_lm_base + 0x00f0);
		val &= ~(BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8));
		writel(val, afe->iocfg_lm_base + 0x00f0);
		val = readl(afe->iocfg_lm_base + 0x0110);
		val &= ~BIT(15);
		writel(val, afe->iocfg_lm_base + 0x0110);
		val = readl(afe->iocfg_lm_base + 0x01e0);
		val |= BIT(23) | BIT(24);
		writel(val, afe->iocfg_lm_base + 0x01e0);
		val = readl(afe->iocfg_lm_base + 0x01f0);
		val |= BIT(19);
		writel(val, afe->iocfg_lm_base + 0x01f0);
	}

	/* 4. Ungate AUDIO_TOP_CON0 & AUDIO_TOP_CON1 clocks */
	val = readl(afe->base + AUDIO_TOP_CON0);
	val &= ~(BIT(2) | BIT(8) | BIT(9) | BIT(24) | BIT(25) | BIT(26) | BIT(27));
	writel(val, afe->base + AUDIO_TOP_CON0);

	val = readl(afe->base + AUDIO_TOP_CON1);
	val &= ~(BIT(15) | BIT(16) | BIT(17));
	writel(val, afe->base + AUDIO_TOP_CON1);

	/* 5. Set AFE_MEMIF_CON0 to normal SRAM mode & 8_24 HD align (0) */
	val = readl(afe->base + AFE_MEMIF_CON0);
	val &= ~(BIT(1) | BIT(2));
	writel(val, afe->base + AFE_MEMIF_CON0);
}

static void q25_afe_configure_adda_dl(struct q25_afe_priv *afe, unsigned int rate, bool is_24bit)
{
	unsigned int dl_src2_con0;
	unsigned int dl_src2_con1 = 0xf74f0000U;

	/* Route DL1_CH1 -> ADDA_DL_CH1 (AFE_CONN3 bit 5) and DL1_CH2 -> ADDA_DL_CH2 (AFE_CONN4 bit 6) */
	writel(BIT(I_DL1_CH1), afe->base + AFE_CONN3);
	writel(BIT(I_DL1_CH2), afe->base + AFE_CONN4);

	if (is_24bit)
		writel(readl(afe->base + AFE_CONN_24BIT) | BIT(3) | BIT(4), afe->base + AFE_CONN_24BIT);
	else
		writel(readl(afe->base + AFE_CONN_24BIT) & ~(BIT(3) | BIT(4)), afe->base + AFE_CONN_24BIT);

	dl_src2_con0 = (q25_rate_to_adda_dl(rate) << 28) |
		       (0x3 << 24) | /* UP_SAMPLING_RATE_X8 */
		       BIT(12) | BIT(11) | /* DL_2_MUTE_CH1/CH2_OFF_CTL_PRE = 1 */
		       BIT(1); /* gain on */
	if (rate == 8000 || rate == 16000)
		dl_src2_con0 |= BIT(5);

	/* Ensure ADDA CKDIV_RST (bit 30) and FIFO_AUTO_RST_EN (bit 29) are cleared */
	writel(readl(afe->base + AFE_ADDA_UL_DL_CON0) & ~(BIT(30) | BIT(29) | BIT(14) | BIT(13)),
	       afe->base + AFE_ADDA_UL_DL_CON0);

	writel(0, afe->base + AFE_ADDA_PREDIS_CON0);
	writel(0, afe->base + AFE_ADDA_PREDIS_CON1);
	writel(dl_src2_con0, afe->base + AFE_ADDA_DL_SRC2_CON0);
	writel(dl_src2_con1, afe->base + AFE_ADDA_DL_SRC2_CON1);

	/* SDM & MTKAIF protocol 2 setup: AUDIO_SDM_LEVEL_NORMAL = 0x1d, USE_NEW_2ND_SDM = BIT(28) */
	writel(0x1dU, afe->base + AFE_ADDA_DL_SDM_DCCOMP_CON);
	writel(BIT(20), afe->base + AFE_ADDA_DL_SDM_DITHER_CON);
	writel(0x190000U | BIT(28) | BIT(31), afe->base + AFE_ADDA_DL_SDM_AUTO_RESET_CON);
	writel(0x80010000U, afe->base + AFE_ADDA_MTKAIF_CFG0);
	writel(0x39, afe->base + AFE_AUD_PAD_TOP);
}

static void q25_afe_configure_adda_ul(struct q25_afe_priv *afe, unsigned int rate, bool is_24bit)
{
	unsigned int voice_mode = q25_rate_to_adda_ul(rate);
	unsigned int ul_src_con0 = (voice_mode << 17) | (1 << 10);

	/* Route ADDA_UL_CH1 -> VUL12_CH1 (AFE_CONN21 bit 3) and ADDA_UL_CH2 -> VUL12_CH2 (AFE_CONN22 bit 4) */
	writel(BIT(I_ADDA_UL_CH1), afe->base + AFE_CONN21);
	writel(BIT(I_ADDA_UL_CH2), afe->base + AFE_CONN22);

	if (is_24bit)
		writel(readl(afe->base + AFE_CONN_24BIT) | BIT(21) | BIT(22), afe->base + AFE_CONN_24BIT);
	else
		writel(readl(afe->base + AFE_CONN_24BIT) & ~(BIT(21) | BIT(22)), afe->base + AFE_CONN_24BIT);

	writel(0x00000000U, afe->base + AFE_ADDA_IIR_COEF_02_01);
	writel(0x00003FB8U, afe->base + AFE_ADDA_IIR_COEF_04_03);
	writel(0x3FB80000U, afe->base + AFE_ADDA_IIR_COEF_06_05);
	writel(0x3FB80000U, afe->base + AFE_ADDA_IIR_COEF_08_07);
	writel(0x0000C048U, afe->base + AFE_ADDA_IIR_COEF_10_09);
	writel(ul_src_con0, afe->base + AFE_ADDA_UL_SRC_CON0);
	writel(0, afe->base + AFE_ADDA_TOP_CON0);
	writel(0, afe->base + AFE_ADDA_MTKAIF_RX_CFG0);
	writel(0x80010000U, afe->base + AFE_ADDA_MTKAIF_CFG0);
	writel(0x39, afe->base + AFE_AUD_PAD_TOP);
}

static const struct snd_pcm_hardware q25_afe_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_RESUME |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_8000_192000,
	.rate_min = 8000,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = MT6789_DL1_SRAM_SIZE,
	.period_bytes_min = 256,
	.period_bytes_max = MT6789_DL1_SRAM_SIZE / 2,
	.periods_min = 2,
	.periods_max = 64,
};

static void q25_mt6358_lineout_amp_set(struct q25_afe_priv *afe, bool enable)
{
	struct snd_soc_component *c = afe->mt6358_cmpnt;
	unsigned int zcd_con1 = 0x0408; /* Default 0dB if unset */
	unsigned int zcd_con2 = 0x0408;
	unsigned int zcd_con3 = 0x0008;

	if (!c)
		return;

	if (enable) {
		/*
		 * Mainline Linux 7.3 mt6358.h uses 0x2408 base for AUDDEC_ANA_CON0..15
		 * (not 5.10's 0x2080!), and LOL Mux in 7.3 has no DAPM supply routes to
		 * DL Power Supply or mt_lo_event. Program the full digital + analog
		 * power sequence at the true 7.3 MT6358 offsets, and configure MT6358
		 * playback GPIO_MODE2 (0x00d8) for MTKAIF MOSI pins.
		 */
		/* 0. Configure MT6358 playback GPIO_MODE2 (MOSI pins) */
		snd_soc_component_update_bits(c, 0x00dc, 0x01f8, 0x01f8); /* GPIO_MODE2_CLR */
		snd_soc_component_update_bits(c, 0x00da, 0xffff, 0x0249); /* GPIO_MODE2_SET */
		snd_soc_component_update_bits(c, 0x00d8, 0xffff, 0x0249); /* GPIO_MODE2 */

		/* 1. Enable XO_AUDIO_EN_M (DCXO_CW14 0x07ac), AUDGLB (0x2422), CLKSQ 26MHz (0x2394), and TOPCK (0x220c) */
		snd_soc_component_update_bits(c, 0x2410, 0x1 << 6, 0x1 << 6); /* AUDDEC_ANA_CON4 HP CMFB rstb */
		snd_soc_component_update_bits(c, 0x07ac, 0x1 << 13, 0x1 << 13); /* DCXO_CW14 XO_AUDIO_EN_M */
		snd_soc_component_update_bits(c, 0x2422, 0x1 << 4, 0x0); /* AUDDEC_ANA_CON13 AUDGLB_PWRDN=0 */
		snd_soc_component_update_bits(c, 0x2394, 0x0003, 0x0001); /* AUDENC_ANA_CON6 RG_CLKSQ_EN=1 */
		snd_soc_component_update_bits(c, 0x220c, 0x0066, 0x0000); /* AUD_TOP_CKPDN_CON0 enable ZCD13M/AUD/AUDIF/AUDNCP */
		usleep_range(250, 270);

		/* 2. Release digital audio clock power-down and enable SDM/FIFO/AFE/DL_SRC2 */
		snd_soc_component_update_bits(c, 0x2292, 0x00c5, 0x0000); /* AUDIO_TOP_CON0 release PDN_AFE/DAC/PWR_CLK */
		usleep_range(250, 270);
		snd_soc_component_write(c, 0x229a, 0x0006); /* AFUNC_AUD_CON2 SDM audio FIFO clock power on */
		snd_soc_component_write(c, 0x2296, 0xcba1); /* AFUNC_AUD_CON0 scrambler clock on */
		snd_soc_component_write(c, 0x229a, 0x0003); /* AFUNC_AUD_CON2 SDM power on */
		snd_soc_component_write(c, 0x229a, 0x000b); /* AFUNC_AUD_CON2 SDM FIFO enable */
		snd_soc_component_update_bits(c, 0x2288, 0x4001, 0x0001); /* AFE_UL_DL_CON0 AFE_ON=1 */
		snd_soc_component_write(c, 0x228a, 0x0001); /* AFE_DL_SRC2_CON0_L DL_2_SRC_ON=1 */

		/* 3. Power on MT6358 NCP, cap-less LDOs, NV reg, IBIST, DAC L/R, and output buffers */
		snd_soc_component_write(c, 0x240c, 0x4033); /* AUDDEC_ANA_CON2 AU_REFN + HPP/N STB */
		snd_soc_component_write(c, 0x223c, 0x0001); /* AUDNCP_CLKDIV_CON1 */
		snd_soc_component_write(c, 0x223e, 0x002c); /* AUDNCP_CLKDIV_CON2 604kHz */
		snd_soc_component_write(c, 0x223a, 0x0001); /* AUDNCP_CLKDIV_CON0 */
		snd_soc_component_write(c, 0x2242, 0x0003); /* AUDNCP_CLKDIV_CON4 */
		snd_soc_component_write(c, 0x2240, 0x0000); /* AUDNCP_CLKDIV_CON3 enable NCP */
		usleep_range(250, 270);
		snd_soc_component_update_bits(c, 0x2424, 0x1055, 0x1055); /* AUDDEC_ANA_CON14 cap-less LDOs 1.5V */
		snd_soc_component_write(c, 0x2426, 0x0001); /* AUDDEC_ANA_CON15 NV regulator -1.2V */
		usleep_range(100, 120);
		snd_soc_component_update_bits(c, 0x2488, 0x1 << 0, 0x0); /* ZCD_CON0 disable AUD_ZCD */
		snd_soc_component_update_bits(c, 0x2416, 0x1 << 4, 0x1 << 4); /* AUDDEC_ANA_CON7 disable LO short-ckt prot */
		snd_soc_component_write(c, 0x2420, 0x0055); /* AUDDEC_ANA_CON12 enable IBIST */
		snd_soc_component_update_bits(c, 0x241e, 0xff80, 0x4900); /* AUDDEC_ANA_CON11 DR bias current */
		snd_soc_component_update_bits(c, 0x2416, 0x1 << 8, 0x1 << 8); /* AUDDEC_ANA_CON7 LO STB enh */
		snd_soc_component_write(c, 0x241a, 0x0000); /* AUDDEC_ANA_CON9 */
		snd_soc_component_write(c, 0x241c, 0x0000); /* AUDDEC_ANA_CON10 */
		snd_soc_component_update_bits(c, 0x2416, 0x3, 0x3); /* AUDDEC_ANA_CON7 LO bias + core */
		snd_soc_component_write(c, 0x2414, 0x0093); /* AUDDEC_ANA_CON6 HS STB + bias + core */
		snd_soc_component_write(c, 0x240a, 0x3f03); /* AUDDEC_ANA_CON1 HP main output stage */
		snd_soc_component_update_bits(c, 0x2422, 0x1, 0x1); /* AUDDEC_ANA_CON13 AUD_CLK */
		/* Enable Audio DAC L + DAC R + VA28_REFGEN + VCM_BUF + HP driver */
		snd_soc_component_write(c, 0x2408, 0x30ff); /* AUDDEC_ANA_CON0 */
		snd_soc_component_write(c, 0x241a, 0xf201); /* AUDDEC_ANA_CON9 low-noise + HP CMFB */
		if (afe->output_route == 2) {
			/* Headphone only: disable LOL/HS, enable HPL/HPR MUX -> DAC */
			snd_soc_component_update_bits(c, 0x2416, 0x3 << 2, 0x0);
			snd_soc_component_write(c, 0x2414, 0x0000);
			snd_soc_component_write(c, 0x2408, 0x3aff);
		} else if (afe->output_route == 3) {
			/* Speaker + Headphone: enable LOL + HPL/HPR MUX -> DAC */
			snd_soc_component_update_bits(c, 0x2416, 0x3 << 2, 0x2 << 2);
			snd_soc_component_write(c, 0x2414, 0x009b);
			snd_soc_component_write(c, 0x2408, 0x3aff);
		} else {
			/* Speaker (0) or Receiver (1): enable LOL + HS MUX -> DAC + HPL/HPR LoudSPK */
			snd_soc_component_update_bits(c, 0x2416, 0x3 << 2, 0x2 << 2);
			snd_soc_component_write(c, 0x2414, 0x009b);
			snd_soc_component_write(c, 0x2408, 0x35ff);
		}
		snd_soc_component_update_bits(c, 0x240c, 0x00c0, 0x0000); /* AUDDEC_ANA_CON2 hp_pull_down=false */
		/* Ensure ZCD_CON1 (0x248a), ZCD_CON2 (0x248c), ZCD_CON3 (0x248e) gains are unmuted */
		zcd_con1 = snd_soc_component_read(c, 0x248a);
		if ((zcd_con1 & 0x1f) >= 0x12)
			snd_soc_component_update_bits(c, 0x248a, 0x0f9f, (4 << 7) | 4); /* +4dB */
		zcd_con2 = snd_soc_component_read(c, 0x248c);
		if ((zcd_con2 & 0x1f) >= 0x12)
			snd_soc_component_update_bits(c, 0x248c, 0x0f9f, (4 << 7) | 4); /* +4dB */
		zcd_con3 = snd_soc_component_read(c, 0x248e);
		if ((zcd_con3 & 0x1f) >= 0x12)
			snd_soc_component_update_bits(c, 0x248e, 0x001f, 4); /* +4dB */
		pr_info("q25-afe: mt6358 route=%u CON0=0x%x CON6=0x%x CON7=0x%x ZCD1=0x%x ZCD2=0x%x ZCD3=0x%x DL_SRC2=0x%x PAD_MON=0x%x\n",
			afe->output_route,
			snd_soc_component_read(c, 0x2408),
			snd_soc_component_read(c, 0x2414),
			snd_soc_component_read(c, 0x2416),
			snd_soc_component_read(c, 0x248a),
			snd_soc_component_read(c, 0x248c),
			snd_soc_component_read(c, 0x248e),
			snd_soc_component_read(c, 0x228a),
			snd_soc_component_read(c, 0x22ce));
	} else {
		snd_soc_component_update_bits(c, 0x2416, 0x3 << 2, 0x0);
		snd_soc_component_update_bits(c, 0x2416, 0x3, 0x0);
		snd_soc_component_write(c, 0x2414, 0x0000);
		snd_soc_component_update_bits(c, 0x2408, 0x0fff, 0x0000);
		snd_soc_component_write(c, 0x228a, 0x0000); /* AFE_DL_SRC2_CON0_L */
	}
}

static irqreturn_t q25_afe_irq_handler(int irq, void *dev_id)
{
	struct q25_afe_priv *afe = dev_id;
	u32 status, en;

	status = readl(afe->base + AFE_IRQ_MCU_STATUS);
	en = readl(afe->base + AFE_IRQ_MCU_EN);
	writel(status, afe->base + AFE_IRQ_MCU_CLR);

	status &= en;
	if ((status & BIT(0)) && afe->substream[SNDRV_PCM_STREAM_PLAYBACK]) {
		if (afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK])
			memcpy_toio(afe->sram_base,
				    afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK],
				    MT6789_DL1_SRAM_SIZE);
		snd_pcm_period_elapsed(afe->substream[SNDRV_PCM_STREAM_PLAYBACK]);
	}

	if ((status & BIT(13)) && afe->substream[SNDRV_PCM_STREAM_CAPTURE]) {
		if (afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE])
			memcpy_fromio(afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE],
				      afe->sram_base + MT6789_VUL_SRAM_OFFSET,
				      MT6789_VUL_SRAM_SIZE);
		snd_pcm_period_elapsed(afe->substream[SNDRV_PCM_STREAM_CAPTURE]);
	}

	return IRQ_HANDLED;
}

static int q25_afe_pcm_open(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct q25_afe_priv *afe = g_q25_afe;
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (!afe)
		return -ENODEV;

	snd_soc_set_runtime_hwparams(substream, &q25_afe_pcm_hardware);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw.buffer_bytes_max = MT6789_VUL_SRAM_SIZE;
		runtime->hw.period_bytes_max = MT6789_VUL_SRAM_SIZE / 2;
	}

	q25_afe_enable_clocks_and_pins(afe);
	afe->substream[substream->stream] = substream;
	return 0;
}

static int q25_afe_pcm_close(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct q25_afe_priv *afe = g_q25_afe;

	if (!afe)
		return 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (afe->spk_pa_enabled) {
			oca72xxx_set_profile(0, "Off");
			afe->spk_pa_enabled = false;
		}
		q25_mt6358_lineout_amp_set(afe, false);
	}
	afe->substream[substream->stream] = NULL;
	return 0;
}

static int q25_afe_pcm_hw_params(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct q25_afe_priv *afe = g_q25_afe;
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t bytes = params_buffer_bytes(params);
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	snd_pcm_format_t format = params_format(params);
	bool is_24bit = (format == SNDRV_PCM_FORMAT_S24_LE || format == SNDRV_PCM_FORMAT_S32_LE);
	unsigned int hd_mode = (format == SNDRV_PCM_FORMAT_S32_LE) ? 1 : (format == SNDRV_PCM_FORMAT_S24_LE ? 3 : 0);
	unsigned int fs_mode = q25_rate_to_memif_mode(rate);
	u32 val;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (bytes > MT6789_DL1_SRAM_SIZE)
			return -ENOMEM;
		runtime->dma_area = afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK];
		runtime->dma_addr = afe->sram_phys;
		runtime->dma_bytes = bytes;
		memset(afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK], 0, bytes);
		memset_io(afe->sram_base, 0, bytes);

		writel(0, afe->base + AFE_DL1_BASE_MSB);
		writel(0, afe->base + AFE_DL1_END_MSB);
		writel((u32)runtime->dma_addr, afe->base + AFE_DL1_BASE);
		writel((u32)(runtime->dma_addr + bytes - 1), afe->base + AFE_DL1_END);

		val = ((fs_mode & 0xfU) << 24) |
		      (1U << 12) | /* DL1_PBUF_SIZE = 1 */
		      ((channels == 1 ? 1U : 0U) << 8) |
		      (1U << 5) | /* DL1_NORMAL_MODE = 1 */
		      ((is_24bit ? 1U : 0U) << 4) | /* DL1_HALIGN */
		      (hd_mode & 0x3U);
		writel(val, afe->base + AFE_DL1_CON0);

		q25_afe_configure_adda_dl(afe, rate, is_24bit);

		writel(params_period_size(params), afe->base + AFE_IRQ_MCU_CNT0);
		val = readl(afe->base + AFE_IRQ_MCU_CON1);
		val &= ~0xfU;
		val |= (fs_mode & 0xfU);
		writel(val, afe->base + AFE_IRQ_MCU_CON1);
	} else {
		if (bytes > MT6789_VUL_SRAM_SIZE)
			return -ENOMEM;
		runtime->dma_area = afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE];
		runtime->dma_addr = afe->sram_phys + MT6789_VUL_SRAM_OFFSET;
		runtime->dma_bytes = bytes;
		memset(afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE], 0, bytes);
		memset_io(afe->sram_base + MT6789_VUL_SRAM_OFFSET, 0, bytes);

		writel(0, afe->base + AFE_VUL12_BASE_MSB);
		writel(0, afe->base + AFE_VUL12_END_MSB);
		writel((u32)runtime->dma_addr, afe->base + AFE_VUL12_BASE);
		writel((u32)(runtime->dma_addr + bytes - 1), afe->base + AFE_VUL12_END);

		val = ((fs_mode & 0xfU) << 24) |
		      ((channels == 1 ? 1U : 0U) << 8) |
		      (1U << 5) | /* VUL12_NORMAL_MODE = 1 */
		      ((is_24bit ? 1U : 0U) << 4) |
		      (hd_mode & 0x3U);
		writel(val, afe->base + AFE_VUL12_CON0);

		q25_afe_configure_adda_ul(afe, rate, is_24bit);

		writel(params_period_size(params), afe->base + AFE_IRQ_MCU_CNT13);
		val = readl(afe->base + AFE_IRQ_MCU_CON2);
		val &= ~(0xfU << 20);
		val |= ((fs_mode & 0xfU) << 20);
		writel(val, afe->base + AFE_IRQ_MCU_CON2);
	}

	return 0;
}

static int q25_afe_pcm_hw_free(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;
	return 0;
}

static int q25_afe_pcm_prepare(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	struct q25_afe_priv *afe = g_q25_afe;

	if (!afe)
		return -ENODEV;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		q25_mt6358_lineout_amp_set(afe, true);
		if (afe->output_route == 0 || afe->output_route == 3) {
			oca72xxx_set_profile(0, "Music");
			afe->spk_pa_enabled = true;
		} else if (afe->output_route == 1) {
			oca72xxx_set_profile(0, "Receiver");
			afe->spk_pa_enabled = true;
		} else {
			oca72xxx_set_profile(0, "Off");
			afe->spk_pa_enabled = false;
		}
	}
	return 0;
}

static int q25_afe_pcm_trigger(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream, int cmd)
{
	struct q25_afe_priv *afe = g_q25_afe;
	unsigned long flags;
	u32 agent;

	spin_lock_irqsave(&afe->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel(readl(afe->base + AFE_DAC_CON0) | BIT(0),
		       afe->base + AFE_DAC_CON0);
		writel(readl(afe->base + AFE_ADDA_UL_DL_CON0) | BIT(0),
		       afe->base + AFE_ADDA_UL_DL_CON0);

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			if (afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK])
				memcpy_toio(afe->sram_base,
					    afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK],
					    MT6789_DL1_SRAM_SIZE);
			writel(readl(afe->base + AFE_ADDA_DL_SRC2_CON0) | BIT(0),
			       afe->base + AFE_ADDA_DL_SRC2_CON0);
			writel(BIT(DL1_ON_SFT), afe->base + AFE_AGENT_ON_SET);
			writel(readl(afe->base + AFE_IRQ_MCU_EN) | BIT(0),
			       afe->base + AFE_IRQ_MCU_EN);
			writel(readl(afe->base + AFE_IRQ_MCU_CON0) | BIT(0),
			       afe->base + AFE_IRQ_MCU_CON0);
			pr_info("q25-afe: START DL1: SPM_STA=0x%x/0x%x AUD_CON0=0x%x DAC_CON0=0x%x AGENT_ON=0x%x DL1_CON0=0x%x BASE=0x%x END=0x%x CUR=0x%x IRQ_EN=0x%x IRQ_CON0=0x%x IRQ_CNT0=0x%x IRQ_STA=0x%x\n",
				afe->spm_base ? readl(afe->spm_base + 0x016c) : 0,
				afe->spm_base ? readl(afe->spm_base + 0x0358) : 0,
				readl(afe->base + AUDIO_TOP_CON0),
				readl(afe->base + AFE_DAC_CON0),
				readl(afe->base + AFE_AGENT_ON),
				readl(afe->base + AFE_DL1_CON0),
				readl(afe->base + AFE_DL1_BASE),
				readl(afe->base + AFE_DL1_END),
				readl(afe->base + AFE_DL1_CUR),
				readl(afe->base + AFE_IRQ_MCU_EN),
				readl(afe->base + AFE_IRQ_MCU_CON0),
				readl(afe->base + AFE_IRQ_MCU_CNT0),
				readl(afe->base + AFE_IRQ_MCU_STATUS));
		} else {
			writel(readl(afe->base + AFE_ADDA_UL_SRC_CON0) | BIT(0),
			       afe->base + AFE_ADDA_UL_SRC_CON0);
			writel(BIT(VUL12_ON_SFT), afe->base + AFE_AGENT_ON_SET);
			writel(readl(afe->base + AFE_IRQ_MCU_EN) | BIT(13),
			       afe->base + AFE_IRQ_MCU_EN);
			writel(readl(afe->base + AFE_IRQ_MCU_CON0) | BIT(13),
			       afe->base + AFE_IRQ_MCU_CON0);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			writel(readl(afe->base + AFE_IRQ_MCU_CON0) & ~BIT(0),
			       afe->base + AFE_IRQ_MCU_CON0);
			writel(BIT(0), afe->base + AFE_IRQ_MCU_CLR);
			writel(BIT(DL1_ON_SFT), afe->base + AFE_AGENT_ON_CLR);
			writel(readl(afe->base + AFE_ADDA_DL_SRC2_CON0) & ~BIT(0),
			       afe->base + AFE_ADDA_DL_SRC2_CON0);
		} else {
			writel(readl(afe->base + AFE_IRQ_MCU_CON0) & ~BIT(13),
			       afe->base + AFE_IRQ_MCU_CON0);
			writel(BIT(13), afe->base + AFE_IRQ_MCU_CLR);
			writel(BIT(VUL12_ON_SFT), afe->base + AFE_AGENT_ON_CLR);
			writel(readl(afe->base + AFE_ADDA_UL_SRC_CON0) & ~BIT(0),
			       afe->base + AFE_ADDA_UL_SRC_CON0);
		}
		agent = readl(afe->base + AFE_AGENT_ON);
		if (!(agent & (BIT(DL1_ON_SFT) | BIT(VUL12_ON_SFT)))) {
			writel(readl(afe->base + AFE_ADDA_UL_DL_CON0) & ~BIT(0),
			       afe->base + AFE_ADDA_UL_DL_CON0);
			writel(readl(afe->base + AFE_DAC_CON0) & ~BIT(0),
			       afe->base + AFE_DAC_CON0);
		}
		break;
	default:
		spin_unlock_irqrestore(&afe->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&afe->lock, flags);
	return 0;
}

static snd_pcm_uframes_t q25_afe_pcm_pointer(struct snd_soc_component *component,
					     struct snd_pcm_substream *substream)
{
	struct q25_afe_priv *afe = g_q25_afe;
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 cur, base;
	ssize_t bytes;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK])
			memcpy_toio(afe->sram_base,
				    afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK],
				    runtime->dma_bytes);
		cur = readl(afe->base + AFE_DL1_CUR);
		base = readl(afe->base + AFE_DL1_BASE);
	} else {
		cur = readl(afe->base + AFE_VUL12_CUR);
		base = readl(afe->base + AFE_VUL12_BASE);
	}

	if (cur == 0 || cur < base)
		bytes = 0;
	else
		bytes = cur - base;

	if (bytes >= runtime->dma_bytes)
		bytes = 0;

	return bytes_to_frames(runtime, bytes);
}

static int q25_afe_pcm_mmap(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream,
			    struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	return remap_vmalloc_range(vma, runtime->dma_area, 0);
}

static const char * const q25_output_route_texts[] = {
	"Speaker",
	"Receiver",
	"Headphone",
	"Speaker+Headphone",
};

static SOC_ENUM_SINGLE_EXT_DECL(q25_output_route_enum, q25_output_route_texts);

static int q25_output_route_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	if (!g_q25_afe)
		return 0;
	ucontrol->value.enumerated.item[0] = g_q25_afe->output_route;
	return 0;
}

static int q25_output_route_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	unsigned int val = ucontrol->value.enumerated.item[0];

	if (!g_q25_afe || val >= ARRAY_SIZE(q25_output_route_texts))
		return -EINVAL;

	g_q25_afe->output_route = val;
	if (g_q25_afe->substream[SNDRV_PCM_STREAM_PLAYBACK]) {
		q25_mt6358_lineout_amp_set(g_q25_afe, true);
		if (val == 0 || val == 3) {
			oca72xxx_set_profile(0, "Music");
			g_q25_afe->spk_pa_enabled = true;
		} else if (val == 1) {
			oca72xxx_set_profile(0, "Receiver");
			g_q25_afe->spk_pa_enabled = true;
		} else {
			oca72xxx_set_profile(0, "Off");
			g_q25_afe->spk_pa_enabled = false;
		}
	}
	return 0;
}

static const struct snd_kcontrol_new q25_afe_controls[] = {
	SOC_ENUM_EXT("Q25 Output Route", q25_output_route_enum,
		     q25_output_route_get, q25_output_route_set),
};

static const struct snd_soc_component_driver q25_afe_component = {
	.name = "q25-mt6789-afe",
	.controls = q25_afe_controls,
	.num_controls = ARRAY_SIZE(q25_afe_controls),
	.open = q25_afe_pcm_open,
	.close = q25_afe_pcm_close,
	.hw_params = q25_afe_pcm_hw_params,
	.hw_free = q25_afe_pcm_hw_free,
	.prepare = q25_afe_pcm_prepare,
	.trigger = q25_afe_pcm_trigger,
	.pointer = q25_afe_pcm_pointer,
	.mmap = q25_afe_pcm_mmap,
	.legacy_dai_naming = 0,
};

static int q25_afe_dai_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	if (stream == SNDRV_PCM_STREAM_PLAYBACK && !mute && g_q25_afe) {
		/* Re-apply after snd_soc_dapm_stream_event(SND_SOC_DAPM_STREAM_START) */
		q25_mt6358_lineout_amp_set(g_q25_afe, true);
	}
	return 0;
}

static const struct snd_soc_dai_ops q25_afe_dai_ops = {
	.mute_stream = q25_afe_dai_mute_stream,
};

static struct snd_soc_dai_driver q25_afe_dai_driver[] = {
	{
		.name = "mt6789-afe-pcm-dai",
		.id = 0,
		.ops = &q25_afe_dai_ops,
		.playback = {
			.stream_name = "PCM Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "PCM Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
		},
	},
	{
		.name = "mt6789-afe-dummy-dai",
		.id = 1,
		.playback = {
			.stream_name = "Dummy Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
};

static int q25_afe_pdrv_probe(struct platform_device *pdev)
{
	return snd_soc_register_component(&pdev->dev, &q25_afe_component,
					  q25_afe_dai_driver, ARRAY_SIZE(q25_afe_dai_driver));
}

static void q25_afe_pdrv_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
}

static struct platform_driver q25_afe_pdrv = {
	.driver = {
		.name = "q25-mt6789-afe",
	},
	.probe = q25_afe_pdrv_probe,
	.remove = q25_afe_pdrv_remove,
};

static struct platform_device *q25_afe_pdev;
static struct platform_device *q25_snd_pdev;

SND_SOC_DAILINK_DEFS(q25_pri_dai,
	DAILINK_COMP_ARRAY(COMP_CPU("mt6789-afe-pcm-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6358-sound", "mt6358-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("q25-mt6789-afe")));

static int q25_mt6358_oca72xxx_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *cmpnt = snd_soc_rtd_to_codec(rtd, 0)->component;

	if (cmpnt) {
		if (g_q25_afe)
			g_q25_afe->mt6358_cmpnt = cmpnt;
		/* Set MTKAIF_PROTOCOL_2_CLK_P2 (2) to match AFE 0x00010000 / 0x38 */
		mt6358_set_mtkaif_protocol(cmpnt, 2);
		/* Initialize Lineout, Headphone, and Handset analog PGA gains to +4dB (valid ALSA 14/18) */
		snd_soc_component_write(cmpnt, 0x248a, (4 << 7) | 4);
		snd_soc_component_write(cmpnt, 0x248c, (4 << 7) | 4);
		snd_soc_component_write(cmpnt, 0x248e, 4);
		oca72xxx_add_codec_controls(cmpnt);
	}
	return 0;
}

static struct snd_soc_dai_link q25_dai_links[] = {
	{
		.name = "MT6789-MT6358-OCA72XXX",
		.stream_name = "Multimedia",
		.init = q25_mt6358_oca72xxx_init,
		SND_SOC_DAILINK_REG(q25_pri_dai),
	},
};

static struct snd_soc_card q25_soc_card = {
	.name = "ZinwaQ25Audio",
	.long_name = "Zinwa-Q25-MT6789-MT6358-OCA72XXX",
	.owner = THIS_MODULE,
	.dai_link = q25_dai_links,
	.num_links = ARRAY_SIZE(q25_dai_links),
};

int q25_register_audio_card(void)
{
	struct device_node *np;
	struct q25_afe_priv *afe;
	int ret;

	if (q25_snd_pdev)
		return 0;

	afe = kzalloc(sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	spin_lock_init(&afe->lock);
	afe->output_route = 0; /* Default to Speaker */
	afe->sram_phys = MT6789_AFE_SRAM_PHYS;

	afe->base = ioremap(0x11210000ULL, 0x2000);
	afe->sram_base = ioremap(MT6789_AFE_SRAM_PHYS, MT6789_AFE_SRAM_SIZE);
	afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK] = vmalloc_user(MT6789_DL1_SRAM_SIZE);
	afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE] = vmalloc_user(MT6789_VUL_SRAM_SIZE);
	afe->gpio_base = ioremap(0x10005000ULL, 0x1000);
	afe->iocfg_lm_base = ioremap(0x11e20000ULL, 0x1000);
	afe->infracfg_base = ioremap(0x10001000ULL, 0x1000);
	afe->topckgen_base = ioremap(0x10000000ULL, 0x1000);
	afe->spm_base = ioremap(0x10006000ULL, 0x1000);

	if (!afe->base || !afe->sram_base ||
	    !afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK] ||
	    !afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE]) {
		pr_err("q25-afe: Failed to ioremap AFE registers/SRAM\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	q25_afe_enable_clocks_and_pins(afe);

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6789-audio");
	if (np) {
		afe->irq = of_irq_get(np, 0);
		of_node_put(np);
	}
	if (afe->irq <= 0) {
		np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
		if (np) {
			struct irq_fwspec fwspec = {
				.fwnode = &np->fwnode,
				.param_count = 3,
				.param = { 0, 234, IRQ_TYPE_LEVEL_HIGH },
			};
			afe->irq = irq_create_fwspec_mapping(&fwspec);
			of_node_put(np);
		}
	}
	if (afe->irq > 0) {
		ret = request_irq(afe->irq, q25_afe_irq_handler,
				  IRQF_TRIGGER_NONE, "q25-afe-pcm", afe);
		if (ret)
			pr_warn("q25-afe: Failed to request IRQ %d: %d\n", afe->irq, ret);
	}

	g_q25_afe = afe;

	ret = platform_driver_register(&q25_afe_pdrv);
	if (ret) {
		pr_err("q25-afe: Failed to register platform driver: %d\n", ret);
		goto err_irq;
	}

	q25_afe_pdev = platform_device_register_simple("q25-mt6789-afe", -1, NULL, 0);
	if (IS_ERR(q25_afe_pdev)) {
		ret = PTR_ERR(q25_afe_pdev);
		q25_afe_pdev = NULL;
		goto err_pdrv;
	}

	q25_snd_pdev = platform_device_register_simple("mt6789-mt6358-snd", -1, NULL, 0);
	if (IS_ERR(q25_snd_pdev)) {
		ret = PTR_ERR(q25_snd_pdev);
		q25_snd_pdev = NULL;
		goto err_afe_pdev;
	}

	q25_soc_card.dev = &q25_snd_pdev->dev;
	ret = snd_soc_register_card(&q25_soc_card);
	if (ret) {
		pr_err("q25-afe: Failed to register ZinwaQ25Audio card: %d\n", ret);
		platform_device_unregister(q25_snd_pdev);
		q25_snd_pdev = NULL;
		goto err_afe_pdev;
	}

	pr_info("q25-afe: Registered ZinwaQ25Audio ALSA card (MT6789 AFE + MT6358 + OCA7255, irq=%d)\n",
		afe->irq);
	return 0;

err_afe_pdev:
	platform_device_unregister(q25_afe_pdev);
	q25_afe_pdev = NULL;
err_pdrv:
	platform_driver_unregister(&q25_afe_pdrv);
err_irq:
	if (afe->irq > 0)
		free_irq(afe->irq, afe);
	g_q25_afe = NULL;
err_unmap:
	if (afe->spm_base)
		iounmap(afe->spm_base);
	if (afe->topckgen_base)
		iounmap(afe->topckgen_base);
	if (afe->infracfg_base)
		iounmap(afe->infracfg_base);
	if (afe->iocfg_lm_base)
		iounmap(afe->iocfg_lm_base);
	if (afe->gpio_base)
		iounmap(afe->gpio_base);
	if (afe->sram_base)
		iounmap(afe->sram_base);
	if (afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK])
		vfree(afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK]);
	if (afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE])
		vfree(afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE]);
	if (afe->base)
		iounmap(afe->base);
	kfree(afe);
	return ret;
}

void q25_unregister_audio_card(void)
{
	struct q25_afe_priv *afe = g_q25_afe;

	if (q25_snd_pdev) {
		snd_soc_unregister_card(&q25_soc_card);
		platform_device_unregister(q25_snd_pdev);
		q25_snd_pdev = NULL;
	}
	if (q25_afe_pdev) {
		platform_device_unregister(q25_afe_pdev);
		platform_driver_unregister(&q25_afe_pdrv);
		q25_afe_pdev = NULL;
	}
	if (afe) {
		if (afe->irq > 0)
			free_irq(afe->irq, afe);
		if (afe->spm_base)
			iounmap(afe->spm_base);
		if (afe->topckgen_base)
			iounmap(afe->topckgen_base);
		if (afe->infracfg_base)
			iounmap(afe->infracfg_base);
		if (afe->iocfg_lm_base)
			iounmap(afe->iocfg_lm_base);
		if (afe->gpio_base)
			iounmap(afe->gpio_base);
		if (afe->sram_base)
			iounmap(afe->sram_base);
		if (afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK])
			vfree(afe->shadow_buf[SNDRV_PCM_STREAM_PLAYBACK]);
		if (afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE])
			vfree(afe->shadow_buf[SNDRV_PCM_STREAM_CAPTURE]);
		if (afe->base)
			iounmap(afe->base);
		kfree(afe);
		g_q25_afe = NULL;
	}
}
