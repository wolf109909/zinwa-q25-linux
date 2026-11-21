// SPDX-License-Identifier: GPL-2.0
/*
 * Zinwa Q25: MT6358/MT6366 PMIC accessory detection (ACCDET) driver.
 *
 * Headset plug detection through the PMIC ACCDET EINT0 block, 3-pole vs
 * 4-pole classification through the ACCDET A/B comparators, and three
 * headset buttons through the AUXADC ACCDET channel.
 *
 * Ported from the MediaTek vendor 5.10 driver that ships on this phone
 * (sound/soc/codecs/mt6358-accdet.c, "vendor:NNNN" below refers to its line
 * numbers). Only the configuration used by the stock Q25 devicetree is
 * ported: PMIC EINT0 (no "headset-eint-num"), PMIC EINT IRQ (no
 * "headset-use-ap-eint"), three-key mode (no "headset-key-mode"), no
 * moisture detection (no "moisture-water-r").
 *
 * Register addresses and bit values are the vendor's MT6358 ones (vendor
 * sound/soc/codecs/mt6358.h). Mainline include/linux/mfd/mt6358/registers.h
 * and sound/soc/codecs/mt6358.h do not define the ACCDET block; the names
 * below follow the vendor field names.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iio/consumer.h>
#include <linux/irq.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "q25_mt6358_accdet.h"

/* ---- PMIC registers (vendor mt6358.h) ---- */
#define AUD_TOP_CKPDN_CON0		0x220c	/* bit0 RG_ACCDET_CK_PDN */
#define AUD_TOP_RST_CON0		0x2220
#define  RG_ACCDET_RST			BIT(1)
#define AUD_TOP_INT_STATUS0		0x2234	/* W1C */
#define  RG_INT_STATUS_ACCDET		BIT(5)
#define  RG_INT_STATUS_ACCDET_EINT0	BIT(6)
#define AUDENC_ANA_CON6			0x2394	/* RG_CLKSQ_EN bit0, RG_AUDSPARE */
#define  RG_CLKSQ_EN			BIT(0)
#define  RG_AUDSPARE_FSTDSCHRG_ANALOG_DIR_EN BIT(5)
#define  RG_AUDSPARE_FSTDSCHRG_IMPR_EN	BIT(6)
#define AUDENC_ANA_CON10		0x239c	/* MICBIAS1 */
#define  RG_AUDMICBIAS1LOWPEN		BIT(2)
#define  RG_AUDMICBIAS1VREF_SFT		4
#define  RG_AUDMICBIAS1DCSW1PEN		BIT(8)
#define AUDENC_ANA_CON11		0x239e
#define  RG_EINTCOMPVTH			BIT(10)
#define  RG_ACCDET_MODE_ANA11_MODE1	0x0807
#define  RG_ACCDET_MODE_ANA11_MODE2	0x0887
#define  RG_ACCDET_MODE_ANA11_MODE6	0x0887

#define ACCDET_CON1			0x250a
#define  ACCDET_EN			BIT(0)
#define  ACCDET_SEQ_INIT		BIT(1)
#define  ACCDET_EINT0_EN		BIT(2)
#define  ACCDET_EINT0_SEQ_INIT		BIT(3)
#define ACCDET_CON2			0x250c
#define  ACCDET_CMP_PWM_EN_ALL		GENMASK(2, 0)	/* CMP/VTH/MBIAS */
#define  ACCDET_EINT0_PWM_EN		BIT(3)
#define  ACCDET_CMP_PWM_IDLE_ALL	GENMASK(10, 8)	/* CMP/VTH/MBIAS */
#define  ACCDET_EINT0_PWM_IDLE		BIT(11)
#define ACCDET_PWM_WIDTH		0x250e	/* CON3 */
#define ACCDET_PWM_THRESH		0x2510	/* CON4 */
#define ACCDET_RISE_DELAY		0x2512	/* CON5, fall delay at bit15 */
#define ACCDET_DEBOUNCE0		0x2514	/* CON6 */
#define ACCDET_DEBOUNCE1		0x2516	/* CON7 */
#define ACCDET_DEBOUNCE3		0x251a	/* CON9 */
#define ACCDET_DEBOUNCE4		0x251c	/* CON10 */
#define ACCDET_CON11			0x251e
#define  ACCDET_EINT0_IVAL_SEL		BIT(14)
#define  ACCDET_EINT0_IVAL_B2_6_10	0x0444
#define ACCDET_CON12			0x2520	/* ACCDET_IRQ */
#define  ACCDET_IRQ			BIT(0)
#define  ACCDET_EINT0_IRQ		BIT(2)
#define  ACCDET_EINT_IRQ_B2_B3		(0x3 << 2)
#define  ACCDET_IRQ_CLR			BIT(8)
#define  ACCDET_EINT0_IRQ_CLR		BIT(10)
#define  ACCDET_EINT0_IRQ_POLARITY	BIT(14)
#define ACCDET_CON14			0x2524	/* ACCDET_MEM_IN */
#define  ACCDET_STATE_MEM_IN_SFT	6
#define  ACCDET_STATE_AB_MASK		0x3
#define ACCDET_CON15			0x2526
#define  ACCDET_EINT0_DEBOUNCE_SFT	3
#define  ACCDET_EINT0_DEBOUNCE_MASK	(0xf << 3)
#define ACCDET_CON24			0x2538	/* ACCDET_HWEN_SEL */
#define  ACCDET_HWMODE_SEL_BIT		BIT(2)
#define  ACCDET_FAST_DISCAHRGE		BIT(4)

#define ACCDET_STATE_AB_00		0x0
#define ACCDET_STATE_AB_01		0x1
#define ACCDET_STATE_AB_11		0x3

#define ACCDET_EINT0_DEB_IN_256		0x0e
#define ACCDET_EINT0_DEB_OUT_012	0x01
#define ACCDET_BUTTON_PRESS_DEBOUNCE	0x400	/* vendor:166 */
#define ACCDET_IRQ_CLR_TIMEOUT_US	400	/* vendor ACCDET_TIME_OUT */
#define ACCDET_MICBIAS_DISABLE_DELAY	(6 * HZ)	/* vendor:166 */

#define HEADSET_MODE_1			1
#define HEADSET_MODE_2			2
#define HEADSET_MODE_6			6

enum { PLUG_OUT = 0, MIC_BIAS, HOOK_SWITCH };			/* accdet FSM */
enum { NO_DEVICE = 0, HEADSET_MIC, HEADSET_NO_MIC };		/* cable */
enum { EINT_PLUG_OUT = 0, EINT_PLUG_IN };
enum { NO_KEY = 0, UP_KEY = 0x01, MD_KEY = 0x02, DW_KEY = 0x04 };

/* "headset-mode-setting", vendor struct pwm_deb_settings */
struct q25_accdet_pwm_deb {
	u32 pwm_width;
	u32 pwm_thresh;
	u32 fall_delay;
	u32 rise_delay;
	u32 debounce0;
	u32 debounce1;
	u32 debounce3;
	u32 debounce4;
	u32 eint_pwm_width;
	u32 eint_pwm_thresh;
	u32 eint_debounce0;
	u32 eint_debounce1;
	u32 eint_debounce2;
	u32 eint_debounce3;
	u32 eint_inverter_debounce;
};

struct q25_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *auxadc;
	int irq_accdet;
	int irq_eint0;

	struct workqueue_struct *wq;
	struct work_struct eint_work;
	struct work_struct accdet_work;
	struct delayed_work dis_micbias_work;

	/* DT */
	u32 mic_vol;
	u32 mic_mode;
	u32 eint_pol;
	bool eint_type_high;
	struct q25_accdet_pwm_deb deb;
	u32 key_mid, key_up, key_down;

	/* protects everything below (vendor res_lock) and the jack pointer */
	struct mutex lock;
	bool hw_inited;
	bool eint_sync_flag;
	u32 cur_eint_state;	/* written by the IRQ thread only */
	u32 accdet_status;
	u32 cable_type;
	u32 cur_key;
	int cali_voltage;
	int jack_status;	/* last SND_JACK_HEADSET bits reported */
	struct snd_soc_jack *jack;
};

static DEFINE_MUTEX(q25_accdet_glock);
static struct q25_accdet *q25_accdet_bound;
static struct snd_soc_jack *q25_accdet_pending_jack;

/* ---------------------------------------------------------------------- */

static inline u32 acc_read(struct q25_accdet *acc, u32 reg)
{
	u32 val = 0;

	regmap_read(acc->regmap, reg, &val);
	return val;
}

static void acc_set_eint0_debounce(struct q25_accdet *acc, u32 deb)
{
	regmap_update_bits(acc->regmap, ACCDET_CON15, ACCDET_EINT0_DEBOUNCE_MASK,
			   deb << ACCDET_EINT0_DEBOUNCE_SFT);
}

static u32 acc_cur_ab(struct q25_accdet *acc)
{
	return (acc_read(acc, ACCDET_CON14) >> ACCDET_STATE_MEM_IN_SFT) &
		ACCDET_STATE_AB_MASK;
}

static void acc_dump(struct q25_accdet *acc, const char *tag)
{
	dev_dbg(acc->dev,
		"%s: CON1=%#x CON2=%#x CON12=%#x CON14=%#x CON15=%#x ANA10=%#x ANA11=%#x\n",
		tag, acc_read(acc, ACCDET_CON1), acc_read(acc, ACCDET_CON2),
		acc_read(acc, ACCDET_CON12), acc_read(acc, ACCDET_CON14),
		acc_read(acc, ACCDET_CON15), acc_read(acc, AUDENC_ANA_CON10),
		acc_read(acc, AUDENC_ANA_CON11));
}

/* ---- jack reporting (vendor send_status_event/send_key_event) ---- */

static void acc_report_status(struct q25_accdet *acc, int status)
{
	lockdep_assert_held(&acc->lock);

	acc->jack_status = status;
	if (acc->jack)
		snd_soc_jack_report(acc->jack, status, SND_JACK_HEADSET);
	dev_info(acc->dev, "jack: %s\n",
		 status == SND_JACK_HEADSET ? "headset (4-pole)" :
		 status == SND_JACK_HEADPHONE ? "headphone (3-pole)" : "unplugged");
}

static void acc_send_status_event(struct q25_accdet *acc)
{
	switch (acc->cable_type) {
	case HEADSET_NO_MIC:
		acc_report_status(acc, SND_JACK_HEADPHONE);
		break;
	case HEADSET_MIC:
		acc_report_status(acc, SND_JACK_HEADSET);
		/* vendor:860: stop the 6 s micbias-off timer for 4-pole */
		cancel_delayed_work(&acc->dis_micbias_work);
		break;
	default:
		/* vendor: "Invalid cableType", nothing reported */
		break;
	}
}

static void acc_send_key_event(struct q25_accdet *acc, u32 key, bool pressed)
{
	int btn;

	lockdep_assert_held(&acc->lock);

	switch (key) {
	case MD_KEY:
		btn = SND_JACK_BTN_0;	/* play/pause */
		break;
	case UP_KEY:
		btn = SND_JACK_BTN_1;	/* volume up */
		break;
	case DW_KEY:
		btn = SND_JACK_BTN_2;	/* volume down */
		break;
	default:
		return;
	}
	dev_dbg(acc->dev, "key %#x %s\n", key, pressed ? "down" : "up");
	if (acc->jack)
		snd_soc_jack_report(acc->jack, pressed ? btn : 0, btn);
}

/* ---- AUXADC key voltage (vendor:601 accdet_get_auxadc, vendor:748 key_check) ---- */

static int acc_get_auxadc(struct q25_accdet *acc)
{
	int vol = 0, ret;

	if (!acc->auxadc)
		return -ENODEV;

	ret = iio_read_channel_processed(acc->auxadc, &vol);
	if (ret < 0) {
		dev_warn(acc->dev, "auxadc read failed: %d\n", ret);
		return ret;
	}
	/*
	 * The vendor subtracts an efuse calibration offset (efuse word 115
	 * for MT6366, low byte, signed, /2, vendor:625). Mainline has no
	 * MT6358 PMIC efuse nvmem provider, so the offset is taken as 0.
	 */
	dev_dbg(acc->dev, "key voltage %d mV\n", vol);
	return vol;
}

static u32 acc_key_check(struct q25_accdet *acc, int v)
{
	if (v < 0)
		return NO_KEY;
	if (v < acc->key_down && v >= acc->key_up)
		return DW_KEY;
	if (v < acc->key_up && v >= acc->key_mid)
		return UP_KEY;
	if (v < acc->key_mid)
		return MD_KEY;
	return NO_KEY;
}

/* vendor:874 multi_key_detection */
static void acc_multi_key_detection(struct q25_accdet *acc, u32 cur_ab)
{
	bool irq_bit;

	if (cur_ab == ACCDET_STATE_AB_00)
		acc->cur_key = acc_key_check(acc, acc->cali_voltage);

	/* let a racing plug-out EINT get flagged before sending the key */
	usleep_range(10000, 12000);

	irq_bit = !(acc_read(acc, ACCDET_CON12) & ACCDET_EINT_IRQ_B2_B3);
	if (irq_bit && READ_ONCE(acc->cur_eint_state) == EINT_PLUG_IN)
		acc_send_key_event(acc, acc->cur_key, !cur_ab);
	else
		acc->cur_key = NO_KEY;

	if (cur_ab)
		acc->cur_key = NO_KEY;
}

/* ---- IRQ clear helpers (vendor:908-962) ---- */

static void acc_clear_int(struct q25_accdet *acc)
{
	regmap_set_bits(acc->regmap, ACCDET_CON12, ACCDET_IRQ_CLR);
}

static void acc_clear_int_check(struct q25_accdet *acc)
{
	u32 val;

	regmap_read_poll_timeout_atomic(acc->regmap, ACCDET_CON12, val,
					!(val & ACCDET_IRQ), 0,
					ACCDET_IRQ_CLR_TIMEOUT_US);
	regmap_clear_bits(acc->regmap, ACCDET_CON12, ACCDET_IRQ_CLR);
	/*
	 * Vendor does a read-modify-write set of the bit; the status register
	 * is write-1-to-clear, so write only our bit to avoid acking other
	 * pending AUD_TOP interrupts.
	 */
	regmap_write(acc->regmap, AUD_TOP_INT_STATUS0, RG_INT_STATUS_ACCDET);
}

static void acc_clear_eint0(struct q25_accdet *acc)
{
	u32 val;

	regmap_set_bits(acc->regmap, ACCDET_CON12, ACCDET_EINT0_IRQ_CLR);
	regmap_read_poll_timeout_atomic(acc->regmap, ACCDET_CON12, val,
					!(val & ACCDET_EINT0_IRQ), 0,
					ACCDET_IRQ_CLR_TIMEOUT_US);
	regmap_clear_bits(acc->regmap, ACCDET_CON12, ACCDET_EINT0_IRQ_CLR);
	regmap_write(acc->regmap, AUD_TOP_INT_STATUS0, RG_INT_STATUS_ACCDET_EINT0);
}

/* vendor:991 eint_polarity_reverse, EINT0 branch */
static void acc_eint0_polarity_reverse(struct q25_accdet *acc)
{
	bool set;

	if (acc->cur_eint_state == EINT_PLUG_OUT)
		set = !acc->eint_type_high;
	else
		set = acc->eint_type_high;

	regmap_update_bits(acc->regmap, ACCDET_CON12, ACCDET_EINT0_IRQ_POLARITY,
			   set ? ACCDET_EINT0_IRQ_POLARITY : 0);
}

/* vendor:1038 disable_accdet */
static void acc_disable_accdet(struct q25_accdet *acc)
{
	acc_clear_int(acc);
	udelay(200);
	mutex_lock(&acc->lock);
	acc_clear_int_check(acc);
	mutex_unlock(&acc->lock);

	/* recover accdet debounce0,3 */
	regmap_write(acc->regmap, ACCDET_DEBOUNCE0, acc->deb.debounce0);
	regmap_write(acc->regmap, ACCDET_DEBOUNCE3, acc->deb.debounce3);

	regmap_clear_bits(acc->regmap, ACCDET_CON1, ACCDET_EN);
	/* clear ACCDET PWM enable to avoid power leakage */
	regmap_clear_bits(acc->regmap, ACCDET_CON2, ACCDET_CMP_PWM_EN_ALL);
}

/* ---- init sequences ---- */

/* vendor:2006 accdet_init: run at late init and on every plug-in */
static void acc_init(struct q25_accdet *acc)
{
	regmap_set_bits(acc->regmap, ACCDET_CON1, ACCDET_SEQ_INIT);
	usleep_range(2000, 3000);
	regmap_clear_bits(acc->regmap, ACCDET_CON1, ACCDET_SEQ_INIT);
	usleep_range(1000, 1500);

	/* debounce time = value / 32768 s */
	regmap_write(acc->regmap, ACCDET_DEBOUNCE0, acc->deb.debounce0);
	regmap_write(acc->regmap, ACCDET_DEBOUNCE1, acc->deb.debounce1);
	regmap_write(acc->regmap, ACCDET_DEBOUNCE3, acc->deb.debounce3);
	regmap_write(acc->regmap, ACCDET_DEBOUNCE4, acc->deb.debounce4);
	/*
	 * vendor also calls accdet_set_debounce(eint_inverter_state000, ..),
	 * which hits the default (no-op) case of accdet_set_debounce().
	 */
}

/* vendor:1823 accdet_eint_high_level_support, EINT0 branch */
static void acc_eint_high_level_support(struct q25_accdet *acc)
{
	u32 reg;

	if (!acc->eint_type_high || acc->cur_eint_state != EINT_PLUG_OUT)
		return;

	regmap_set_bits(acc->regmap, ACCDET_CON12, ACCDET_EINT0_IRQ_POLARITY);

	/* set EINT0_SEQ_INIT to load the default EINT init status */
	regmap_set_bits(acc->regmap, ACCDET_CON1, ACCDET_EINT0_SEQ_INIT);
	usleep_range(2000, 3000);
	reg = acc_read(acc, ACCDET_CON11);
	regmap_write(acc->regmap, ACCDET_CON11,
		     (reg | ACCDET_EINT0_IVAL_SEL) & ~ACCDET_EINT0_IVAL_B2_6_10);
	usleep_range(2000, 3000);
	regmap_clear_bits(acc->regmap, ACCDET_CON1, ACCDET_EINT0_SEQ_INIT);

	reg = acc_read(acc, ACCDET_CON15) & ~ACCDET_EINT0_DEBOUNCE_MASK;
	regmap_write(acc->regmap, ACCDET_CON15, reg);
	/*
	 * vendor:1870 writes ACCDET_EINT0_DEB_IN_256 (0x0e) unshifted, i.e.
	 * bits [3:1] of CON15 (EINT0 debounce = 1, plus two undocumented
	 * bits). Kept as-is because that is what runs on the stock firmware;
	 * the debounce field is rewritten on the first EINT anyway.
	 */
	regmap_write(acc->regmap, ACCDET_CON15, reg | ACCDET_EINT0_DEB_IN_256);
}

/* vendor:1924 accdet_init_once */
static void acc_init_once(struct q25_accdet *acc)
{
	u32 reg;

	/* reset the accdet unit */
	regmap_set_bits(acc->regmap, AUD_TOP_RST_CON0, RG_ACCDET_RST);
	regmap_clear_bits(acc->regmap, AUD_TOP_RST_CON0, RG_ACCDET_RST);

	/* PWM frequency, duty & rise/falling delay (REGISTER_VAL = x - 1) */
	regmap_write(acc->regmap, ACCDET_PWM_WIDTH, acc->deb.pwm_width - 1);
	regmap_write(acc->regmap, ACCDET_PWM_THRESH, acc->deb.pwm_thresh - 1);
	regmap_write(acc->regmap, ACCDET_RISE_DELAY,
		     acc->deb.fall_delay << 15 | acc->deb.rise_delay);

	/* micbias1 voltage (vendor ORs the field in) + low power enable */
	if (acc->mic_vol <= 7) {
		reg = (acc->mic_vol << RG_AUDMICBIAS1VREF_SFT) | RG_AUDMICBIAS1LOWPEN;
		regmap_update_bits(acc->regmap, AUDENC_ANA_CON10, reg, reg);
	}

	/* mic mode */
	if (acc->mic_mode == HEADSET_MODE_1) {
		/* ACC mode */
		regmap_set_bits(acc->regmap, AUDENC_ANA_CON11,
				RG_ACCDET_MODE_ANA11_MODE1);
	} else if (acc->mic_mode == HEADSET_MODE_2) {
		/* DCC mode, low cost mode without internal bias */
		regmap_set_bits(acc->regmap, AUDENC_ANA_CON11,
				RG_ACCDET_MODE_ANA11_MODE2);
	} else if (acc->mic_mode == HEADSET_MODE_6) {
		/* DCC mode with internal bias */
		regmap_set_bits(acc->regmap, AUDENC_ANA_CON11,
				RG_ACCDET_MODE_ANA11_MODE6);
		regmap_set_bits(acc->regmap, AUDENC_ANA_CON10,
				RG_AUDMICBIAS1DCSW1PEN);
	}

	/*
	 * "Set HP_EINT trigger level to 2.0v": done by the vendor codec driver
	 * (vendor mt6358.c:6614, mt6358_codec_init_reg) right before it hands
	 * over to the accdet late init; mainline mt6358.c does not do it.
	 */
	regmap_set_bits(acc->regmap, AUDENC_ANA_CON11, RG_EINTCOMPVTH);

	/* analog fast discharge */
	if (acc->eint_pol == IRQ_TYPE_LEVEL_LOW) {
		regmap_set_bits(acc->regmap, AUDENC_ANA_CON6,
				RG_AUDSPARE_FSTDSCHRG_IMPR_EN |
				RG_AUDSPARE_FSTDSCHRG_ANALOG_DIR_EN);
	} else {
		/*
		 * vendor: reg = reg & 0xE0. That would also clear RG_CLKSQ_EN
		 * (bit0), which mainline mt6358.c owns as the "CLKSQ Audio"
		 * DAPM supply; keep that bit and clear the rest as the vendor.
		 */
		regmap_update_bits(acc->regmap, AUDENC_ANA_CON6,
				   (u32)~(0xE0 | RG_CLKSQ_EN) & 0xffff, 0);
	}

	/* vendor:1887 config_eint_init_by_mode, EINT0 */
	regmap_set_bits(acc->regmap, ACCDET_CON15, 0x6 << 8);	/* EINT0 PWM thresh */
	regmap_set_bits(acc->regmap, ACCDET_CON15, 0x2 << 12);	/* EINT0 PWM width */
	regmap_set_bits(acc->regmap, ACCDET_CON2, ACCDET_EINT0_PWM_EN);
	regmap_set_bits(acc->regmap, ACCDET_CON2, ACCDET_EINT0_PWM_IDLE);

	/* vendor:1877 config_digital_init_by_mode: sw mode + fast discharge */
	reg = acc_read(acc, ACCDET_CON24);
	regmap_write(acc->regmap, ACCDET_CON24,
		     (reg & ~ACCDET_HWMODE_SEL_BIT) | ACCDET_FAST_DISCAHRGE);

	acc_eint_high_level_support(acc);

	/* accdet eint enable (vendor writes the whole register) */
	regmap_write(acc->regmap, ACCDET_CON1, ACCDET_EINT0_EN);
}

/* vendor:2027 mt6358_accdet_late_init: init, init_debounce, init_once */
static void acc_late_init(struct q25_accdet *acc)
{
	u32 ckpdn = acc_read(acc, AUD_TOP_CKPDN_CON0);

	if (ckpdn & BIT(0))
		dev_warn(acc->dev, "ACCDET clock gated (AUD_TOP_CKPDN_CON0=%#x)\n",
			 ckpdn);

	acc_init(acc);
	/* vendor:1996 accdet_init_debounce: EINT0 debounce (eint_state011 is a no-op) */
	acc_set_eint0_debounce(acc, acc->deb.eint_debounce0);
	acc_init_once(acc);
	acc_dump(acc, "late_init");
}

/* ---- deferred work ---- */

/* vendor:1215 check_cable_type */
static void acc_check_cable_type(struct q25_accdet *acc)
{
	u32 cur_ab = acc_cur_ab(acc);

	lockdep_assert_held(&acc->lock);

	dev_dbg(acc->dev, "AB=%u status=%u cable=%u sync=%d\n", cur_ab,
		acc->accdet_status, acc->cable_type, acc->eint_sync_flag);

	switch (acc->accdet_status) {
	case PLUG_OUT:
		if (cur_ab == ACCDET_STATE_AB_00) {
			if (acc->eint_sync_flag) {
				acc->cable_type = HEADSET_NO_MIC;
				acc->accdet_status = HOOK_SWITCH;
			}
		} else if (cur_ab == ACCDET_STATE_AB_01) {
			if (acc->eint_sync_flag) {
				acc->accdet_status = MIC_BIAS;
				acc->cable_type = HEADSET_MIC;
				regmap_write(acc->regmap, ACCDET_DEBOUNCE3,
					     acc->deb.debounce3 * 30);
			}
			/* faster hook switch debounce so quick presses are seen */
			regmap_write(acc->regmap, ACCDET_DEBOUNCE0,
				     ACCDET_BUTTON_PRESS_DEBOUNCE);
		} else if (cur_ab == ACCDET_STATE_AB_11) {
			if (acc->eint_sync_flag) {
				acc->accdet_status = PLUG_OUT;
				acc->cable_type = NO_DEVICE;
			}
		}
		break;
	case MIC_BIAS:
		if (cur_ab == ACCDET_STATE_AB_00) {
			if (acc->eint_sync_flag) {
				acc->accdet_status = HOOK_SWITCH;
				acc_multi_key_detection(acc, cur_ab);
			}
		} else if (cur_ab == ACCDET_STATE_AB_01) {
			if (acc->eint_sync_flag) {
				acc->accdet_status = MIC_BIAS;
				acc->cable_type = HEADSET_MIC;
			}
		} else if (cur_ab == ACCDET_STATE_AB_11) {
			if (acc->eint_sync_flag)
				acc->accdet_status = PLUG_OUT;
		}
		break;
	case HOOK_SWITCH:
		if (cur_ab == ACCDET_STATE_AB_01) {
			if (acc->eint_sync_flag) {
				acc_multi_key_detection(acc, cur_ab);
				acc->accdet_status = MIC_BIAS;
				acc->cable_type = HEADSET_MIC;
			}
		} else if (cur_ab == ACCDET_STATE_AB_11) {
			if (acc->eint_sync_flag)
				acc->accdet_status = PLUG_OUT;
		}
		break;
	default:
		break;
	}
}

/* vendor:1331 accdet_work_callback */
static void acc_accdet_work(struct work_struct *work)
{
	struct q25_accdet *acc = container_of(work, struct q25_accdet, accdet_work);
	u32 pre_cable_type;

	pm_stay_awake(acc->dev);
	mutex_lock(&acc->lock);
	pre_cable_type = acc->cable_type;
	acc_check_cable_type(acc);
	if (acc->eint_sync_flag && pre_cable_type != acc->cable_type)
		acc_send_status_event(acc);
	mutex_unlock(&acc->lock);
	pm_relax(acc->dev);
}

/* vendor:1059 headset_plug_out */
static void acc_headset_plug_out(struct q25_accdet *acc)
{
	mutex_lock(&acc->lock);
	acc_report_status(acc, 0);
	acc->accdet_status = PLUG_OUT;
	acc->cable_type = NO_DEVICE;
	if (acc->cur_key != NO_KEY) {
		acc_send_key_event(acc, acc->cur_key, false);
		acc->cur_key = NO_KEY;
	}
	mutex_unlock(&acc->lock);
}

/* vendor:1122 eint_work_callback */
static void acc_eint_work(struct work_struct *work)
{
	struct q25_accdet *acc = container_of(work, struct q25_accdet, eint_work);

	if (READ_ONCE(acc->cur_eint_state) == EINT_PLUG_IN) {
		mutex_lock(&acc->lock);
		acc->eint_sync_flag = true;
		mutex_unlock(&acc->lock);
		pm_wakeup_event(acc->dev, 7000);

		acc_init(acc);

		/* PWM idle on */
		regmap_set_bits(acc->regmap, ACCDET_CON2, ACCDET_CMP_PWM_IDLE_ALL);
		regmap_set_bits(acc->regmap, ACCDET_CON2, ACCDET_EINT0_PWM_IDLE);
		regmap_set_bits(acc->regmap, ACCDET_CON2, ACCDET_CMP_PWM_EN_ALL);
		/* enable ACCDET unit */
		regmap_set_bits(acc->regmap, ACCDET_CON1, ACCDET_EN);
		dev_dbg(acc->dev, "EINT plug-in, ACCDET enabled\n");
	} else {
		mutex_lock(&acc->lock);
		acc->eint_sync_flag = false;
		mutex_unlock(&acc->lock);
		cancel_delayed_work(&acc->dis_micbias_work);

		regmap_clear_bits(acc->regmap, ACCDET_CON2, ACCDET_CMP_PWM_IDLE_ALL);
		acc_disable_accdet(acc);
		acc_headset_plug_out(acc);
	}
	acc_dump(acc, "eint_work");
}

/* vendor:1083 dis_micbias_work_callback */
static void acc_dis_micbias_work(struct work_struct *work)
{
	struct q25_accdet *acc = container_of(to_delayed_work(work),
					      struct q25_accdet, dis_micbias_work);
	u32 cur_ab;
	bool off;

	/*
	 * vendor also bails out if ACCDET_EINT0_MEM_IN == M_PLUG_OUT (3) or
	 * if dis_micbias_done is set; neither can happen on this
	 * configuration (1-bit field, no modify_vref_volt caller).
	 */
	mutex_lock(&acc->lock);
	cur_ab = acc_cur_ab(acc);
	/*
	 * 3-pole, or AB=00/11 after 6 s: turn the ACCDET comparators and
	 * micbias PWM off to save power; EINT still catches the plug-out.
	 */
	off = acc->eint_sync_flag &&
	      (acc->cable_type == HEADSET_NO_MIC ||
	       cur_ab == ACCDET_STATE_AB_00 || cur_ab == ACCDET_STATE_AB_11);
	mutex_unlock(&acc->lock);

	if (off) {
		regmap_clear_bits(acc->regmap, ACCDET_CON2, ACCDET_CMP_PWM_IDLE_ALL);
		acc_disable_accdet(acc);
		dev_dbg(acc->dev, "micbias/ACCDET off (AB=%u)\n", cur_ab);
	}
}

/* vendor:1359 pmic_eint_queue_work, EINT0 branch */
static void acc_eint0_queue_work(struct q25_accdet *acc)
{
	if (acc->cur_eint_state == EINT_PLUG_IN) {
		acc_set_eint0_debounce(acc, ACCDET_EINT0_DEB_IN_256);
		regmap_write(acc->regmap, ACCDET_DEBOUNCE3, acc->deb.debounce3);
		WRITE_ONCE(acc->cur_eint_state, EINT_PLUG_OUT);
	} else {
		acc_set_eint0_debounce(acc, ACCDET_EINT0_DEB_OUT_012);
		WRITE_ONCE(acc->cur_eint_state, EINT_PLUG_IN);
		mod_delayed_work(acc->wq, &acc->dis_micbias_work,
				 ACCDET_MICBIAS_DISABLE_DELAY);
	}
	queue_work(acc->wq, &acc->eint_work);
}

/* vendor:1507 accdet_irq_handle; shared by ACCDET and EINT0 interrupts */
static irqreturn_t acc_irq_thread(int irq, void *data)
{
	struct q25_accdet *acc = data;
	u32 irq_status = acc_read(acc, ACCDET_CON12);
	bool eint0 = irq_status & ACCDET_EINT0_IRQ;

	if ((irq_status & ACCDET_IRQ) && !eint0) {
		acc_clear_int(acc);
		/* sample the key voltage while the button is still held */
		if (acc->accdet_status == MIC_BIAS)
			acc->cali_voltage = acc_get_auxadc(acc);
		queue_work(acc->wq, &acc->accdet_work);
		acc_clear_int_check(acc);
	} else if (eint0) {
		dev_dbg(acc->dev, "EINT0, IRQ=%#x cur_eint_state=%u\n",
			irq_status, acc->cur_eint_state);
		acc_eint0_polarity_reverse(acc);
		acc_clear_eint0(acc);
		acc_eint0_queue_work(acc);
	} else {
		dev_dbg(acc->dev, "no accdet interrupt pending (%#x)\n",
			irq_status);
	}

	return IRQ_HANDLED;
}

/* ---- machine driver interface ---- */

static void acc_attach_jack(struct q25_accdet *acc, struct snd_soc_jack *jack)
{
	bool start;

	mutex_lock(&acc->lock);
	acc->jack = jack;
	/* the headset may already be in: report the state we know */
	if (jack) {
		snd_soc_jack_report(jack, acc->jack_status, SND_JACK_HEADSET);
		if (acc->cur_key != NO_KEY)
			acc_send_key_event(acc, acc->cur_key, true);
	}
	start = jack && !acc->hw_inited;
	if (start)
		acc->hw_inited = true;
	mutex_unlock(&acc->lock);

	if (!start)
		return;

	/*
	 * Like the vendor (codec calls mt6358_accdet_late_init() after its
	 * own register init), program the block only once the mt6358 codec
	 * component has probed: the ACCDET reset below must come after the
	 * codec's ACCDET_CON13 write. With EINT level-triggered, an already
	 * inserted plug raises EINT0 as soon as it is enabled.
	 */
	acc_late_init(acc);
	enable_irq(acc->irq_accdet);
	enable_irq(acc->irq_eint0);
}

/**
 * q25_mt6358_accdet_set_jack - attach (or detach with NULL) the card jack
 * @jack: jack with SND_JACK_HEADSET | SND_JACK_BTN_0..2
 *
 * Returns 0 if the ACCDET device is bound, -ENODEV otherwise (the jack is
 * remembered and attached when the device binds later).
 */
int q25_mt6358_accdet_set_jack(struct snd_soc_jack *jack)
{
	struct q25_accdet *acc;

	mutex_lock(&q25_accdet_glock);
	q25_accdet_pending_jack = jack;
	acc = q25_accdet_bound;
	if (acc)
		acc_attach_jack(acc, jack);
	mutex_unlock(&q25_accdet_glock);

	return acc ? 0 : -ENODEV;
}
EXPORT_SYMBOL_GPL(q25_mt6358_accdet_set_jack);

/* ---- platform driver ---- */

static int acc_parse_dt(struct q25_accdet *acc)
{
	struct device_node *np = acc->dev->of_node;
	u32 three_key[4];
	u32 tmp;

	/* defaults from vendor:1671 accdet_get_dts_data */
	if (of_property_read_u32(np, "accdet-mic-vol", &acc->mic_vol))
		acc->mic_vol = 6;
	if (of_property_read_u32(np, "accdet-mic-mode", &acc->mic_mode))
		acc->mic_mode = 2;
	if (of_property_read_u32(np, "headset-eint-level-pol", &acc->eint_pol))
		acc->eint_pol = IRQ_TYPE_LEVEL_LOW;
	acc->eint_type_high = acc->eint_pol == IRQ_TYPE_LEVEL_HIGH;

	/* 16 cells in DT, the vendor reads the first 15 */
	if (of_property_read_u32_array(np, "headset-mode-setting", (u32 *)&acc->deb,
				       sizeof(acc->deb) / sizeof(u32))) {
		dev_err(acc->dev, "missing headset-mode-setting\n");
		return -EINVAL;
	}

	if (!of_property_read_u32(np, "headset-use-ap-eint", &tmp) && tmp) {
		dev_err(acc->dev, "AP GPIO EINT mode not supported\n");
		return -EINVAL;
	}
	if (!of_property_read_u32(np, "headset-eint-num", &tmp) && tmp) {
		dev_err(acc->dev, "only PMIC EINT0 is supported\n");
		return -EINVAL;
	}
	if (!of_property_read_u32(np, "headset-key-mode", &tmp) && tmp) {
		dev_err(acc->dev, "only three-key mode is supported\n");
		return -EINVAL;
	}
	if (of_property_present(np, "moisture-water-r"))
		dev_warn(acc->dev, "moisture detection not supported, ignored\n");

	if (!of_property_read_u32_array(np, "headset-three-key-threshold",
					three_key, ARRAY_SIZE(three_key))) {
		acc->key_mid = three_key[1];
		acc->key_up = three_key[2];
		acc->key_down = three_key[3];
	} else {
		dev_warn(acc->dev, "no headset-three-key-threshold, buttons disabled\n");
		acc->auxadc = NULL;
	}

	dev_info(acc->dev,
		 "mic_vol=%u mic_mode=%u eint_pol=%u keys=%u/%u/%u mV\n",
		 acc->mic_vol, acc->mic_mode, acc->eint_pol,
		 acc->key_mid, acc->key_up, acc->key_down);
	return 0;
}

static void acc_destroy_wq(void *data)
{
	destroy_workqueue(data);
}

static int q25_accdet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q25_accdet *acc;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	acc = devm_kzalloc(dev, sizeof(*acc), GFP_KERNEL);
	if (!acc)
		return -ENOMEM;

	acc->dev = dev;
	mutex_init(&acc->lock);

	/* mt6397-core keeps the pwrap regmap in its chip data (cf. mt6359-accdet) */
	acc->regmap = ((struct mt6397_chip *)dev_get_drvdata(pdev->dev.parent))->regmap;
	if (!acc->regmap)
		return dev_err_probe(dev, -ENODEV, "no PMIC regmap\n");

	acc->auxadc = devm_iio_channel_get(dev, "pmic_accdet");
	if (IS_ERR(acc->auxadc)) {
		ret = PTR_ERR(acc->auxadc);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "no AUXADC channel (%d), buttons disabled\n", ret);
		acc->auxadc = NULL;
	}

	ret = acc_parse_dt(acc);
	if (ret)
		return ret;

	acc->irq_accdet = platform_get_irq_byname(pdev, "accdet_irq");
	if (acc->irq_accdet < 0)
		return acc->irq_accdet;
	acc->irq_eint0 = platform_get_irq_byname(pdev, "accdet_eint0");
	if (acc->irq_eint0 < 0)
		return acc->irq_eint0;

	acc->wq = alloc_ordered_workqueue("q25-accdet", 0);
	if (!acc->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, acc_destroy_wq, acc->wq);
	if (ret)
		return ret;

	INIT_WORK(&acc->eint_work, acc_eint_work);
	INIT_WORK(&acc->accdet_work, acc_accdet_work);
	INIT_DELAYED_WORK(&acc->dis_micbias_work, acc_dis_micbias_work);

	/* enabled once the hardware is programmed (acc_attach_jack) */
	ret = devm_request_threaded_irq(dev, acc->irq_accdet, NULL, acc_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"accdet", acc);
	if (ret)
		return dev_err_probe(dev, ret, "request accdet irq\n");
	ret = devm_request_threaded_irq(dev, acc->irq_eint0, NULL, acc_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"accdet_eint0", acc);
	if (ret)
		return dev_err_probe(dev, ret, "request accdet eint0 irq\n");

	device_init_wakeup(dev, true);
	platform_set_drvdata(pdev, acc);

	mutex_lock(&q25_accdet_glock);
	q25_accdet_bound = acc;
	if (q25_accdet_pending_jack)
		acc_attach_jack(acc, q25_accdet_pending_jack);
	mutex_unlock(&q25_accdet_glock);

	dev_info(dev, "MT6358 ACCDET ready (EINT0, mode %u)\n", acc->mic_mode);
	return 0;
}

static void q25_accdet_remove(struct platform_device *pdev)
{
	struct q25_accdet *acc = platform_get_drvdata(pdev);

	mutex_lock(&q25_accdet_glock);
	q25_accdet_bound = NULL;
	mutex_unlock(&q25_accdet_glock);

	if (acc->hw_inited) {
		disable_irq(acc->irq_eint0);
		disable_irq(acc->irq_accdet);
	}
	cancel_delayed_work_sync(&acc->dis_micbias_work);
	cancel_work_sync(&acc->eint_work);
	cancel_work_sync(&acc->accdet_work);
	device_init_wakeup(&pdev->dev, false);
}

static const struct of_device_id q25_accdet_of_match[] = {
	{ .compatible = "mediatek,mt6358-accdet" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, q25_accdet_of_match);
MODULE_IMPORT_NS("IIO_CONSUMER");

static struct platform_driver q25_accdet_driver = {
	.driver = {
		.name = "mt6358-accdet",
		.of_match_table = q25_accdet_of_match,
	},
	.probe = q25_accdet_probe,
	.remove = q25_accdet_remove,
};

static bool q25_accdet_registered;

int q25_mt6358_accdet_register(void)
{
	int ret = platform_driver_register(&q25_accdet_driver);

	q25_accdet_registered = !ret;
	return ret;
}

void q25_mt6358_accdet_unregister(void)
{
	if (q25_accdet_registered)
		platform_driver_unregister(&q25_accdet_driver);
	q25_accdet_registered = false;
}
