// SPDX-License-Identifier: GPL-2.0
/*
 * Zinwa Q25 battery fuel gauge on the MT6366 PMIC FGADC (coulomb counter).
 *
 * The MT6366 is register compatible with the MT6358 (mainline "mt6358"
 * MFD); its FGADC block continuously measures the voltage across the
 * battery sense resistor and integrates it into a coulomb counter (CAR).
 * The vendor 5.10 kernel drives it through MediaTek's GM3 algorithm
 * (drivers/power/supply/mt6358-gauge.c + mtk_battery*.c + a userspace
 * daemon).  This driver deliberately does NOT port GM3.  It reads the same
 * hardware counters and does plain, documented coulomb counting:
 *
 *  - current_now  : FGADC instantaneous current (FG_CURRENT_OUT)
 *  - current_avg  : FGADC hardware average current (FG_IAVG), when valid
 *  - charge_counter: coulomb counter (FG_CAR) since the PMIC FG was reset
 *                   (the bootloader resets it on every boot), in uAh
 *  - charge_now / capacity: remaining charge = anchor + (CAR - CAR_anchor),
 *                   relative to charge-full-design (vendor g_Q_MAX at 25 C)
 *
 * Anchors (the only places the counted charge is overwritten):
 *  1. probe: OCV estimate, OCV = VBAT - I * R_internal, looked up in the
 *     monitored-battery ocv-capacity-table (vendor 25 C profile).
 *  2. the charger reports FULL (charge termination): 100 %.
 *  3. after a long low-current period (>= 30 min with |avg I| < 30 mA,
 *     i.e. a suspend), if the OCV estimate differs from the counted SOC by
 *     >= 5 points: snap to the OCV estimate (corrects counter drift).
 *  4. empty / impossible states: OCV estimate at 0 % for two consecutive
 *     polls while discharging -> 0 %; counted <= 0 while OCV says >= 3 %
 *     -> OCV estimate; counted > full -> full.
 * No temperature compensation, no ageing (charge_full == design).
 *
 * Register map, latch protocol and LSB constants are taken from the vendor
 * driver (vendor-lineage-5.10 drivers/power/supply/mt6358-gauge.c):
 *   RG_FGADC_CON1 0xd0a, RG_FGADC_CAR_CON0/1 0xd14/0xd16,
 *   RG_FGADC_IAVG_CON0/1 0xd30/0xd32, RG_FGADC_CUR_CON0 0xd8a  (l.51-147)
 *   UNIT_FGCURRENT 381470 nA, UNIT_FG_IAVG 190735 nA,
 *   CHARGE_LSB 190.735 uAs, all for a 10 mOhm sense resistor (l.288-301)
 *   pre_gauge_update()/post_gauge_update() latch handshake (l.394-445)
 *   reg_to_current(): s16, positive = charging (l.451-489)
 *   fg_get_coulomb: CAR s32, bit 31 = discharging (l.1800-1880)
 *   instant_current()/average_current_get(): *DEFAULT_R_FG/r_fg_value,
 *   *car_tune/1000 (l.1412-1437, l.1493-1600)
 * The sense resistor and CAR tune come from the vendor DT (mtk_gauge
 * R_FG_VALUE = 1 [mOhm], CAR_TUNE_VALUE = 101 [x10 per mille]) and are DT
 * properties here, see the binding comment in the DTS.
 */

#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define RG_FGADC_CON1		0x0d0a
#define  FG_LATCHDATA_ST	BIT(15)
#define  FG_SW_CLEAR		BIT(3)
#define  FG_SW_READ_PRE		BIT(0)
#define RG_FGADC_CAR_CON0	0x0d14	/* CAR[15:0] */
#define RG_FGADC_CAR_CON1	0x0d16	/* CAR[31:16] */
#define RG_FGADC_IAVG_CON0	0x0d30	/* IAVG[15:0] */
#define RG_FGADC_IAVG_CON1	0x0d32	/* [15] VLD, [11:0] IAVG[27:16] */
#define  FG_IAVG_VLD		BIT(15)
#define RG_FGADC_CUR_CON0	0x0d8a	/* FG_CURRENT_OUT, s16 */

/* LSBs with the 10 mOhm reference resistor (vendor DEFAULT_R_FG = 100) */
#define FG_CUR_LSB_NA		381470LL	/* UNIT_FGCURRENT */
#define FG_IAVG_LSB_NA		190735LL	/* UNIT_FG_IAVG */
#define FG_CAR_LSB_NAS		190735LL	/* CHARGE_LSB 190.735 uAs */
#define FG_REF_RSENSE_UOHM	10000LL

#define POLL_ONLINE_MS		(30 * 1000)
#define POLL_OFFLINE_MS		(60 * 1000)
#define RELAX_MIN_S		(30 * 60)
#define RELAX_MAX_AVG_UA	30000
#define RELAX_MAX_NOW_UA	200000
#define RELAX_MIN_DIFF_PCT	5

struct q25_fg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct iio_channel *vbat_chan;
	struct delayed_work work;
	struct mutex lock;	/* cached state */
	struct mutex hw_lock;	/* FG latch handshake */

	u32 rsense_uohm;	/* sense resistor */
	u32 car_tune;		/* per mille */
	int rint_uohm;		/* battery + path resistance for OCV estimate */
	int qmax_uah;		/* charge-full-design */

	bool anchored;
	s64 anchor_car_uah;
	s64 anchor_charge_uah;

	s64 last_car_uah;
	u64 last_ns;
	bool last_valid;
	int empty_hits;

	/* cached results, under lock */
	int status;
	int soc;
	s64 charge_uah;
	s64 car_uah;
	int cur_ua;
	int avg_ua;
	bool avg_valid;
	int vbat_uv;
	int ocv_uv;
};

struct q25_fg_snap {
	s16 cur;
	s32 car;
	s32 iavg;
	bool iavg_valid;
};

/* Scale a value measured against the 10 mOhm reference to the real Rsense */
static s64 q25_fg_scale(struct q25_fg *fg, s64 v)
{
	v = div_s64(v * fg->car_tune, 1000);
	return div_s64(v * FG_REF_RSENSE_UOHM, fg->rsense_uohm);
}

/* Latch, read and release the FGADC data registers (vendor pre/post_gauge_update) */
static int q25_fg_read_snap(struct q25_fg *fg, struct q25_fg_snap *s)
{
	struct regmap *map = fg->regmap;
	unsigned int v, lo, hi;
	int ret, ret2;

	guard(mutex)(&fg->hw_lock);

	ret = regmap_update_bits(map, RG_FGADC_CON1,
				 FG_SW_CLEAR | FG_SW_READ_PRE, FG_SW_READ_PRE);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(map, RG_FGADC_CON1, v,
				       v & FG_LATCHDATA_ST, 100, 20000);
	if (ret) {
		dev_warn_ratelimited(fg->dev, "FG latch timeout (con1=%#x)\n", v);
		goto release;
	}

	ret = regmap_read(map, RG_FGADC_CUR_CON0, &v);
	if (ret)
		goto release;
	s->cur = (s16)v;

	ret = regmap_read(map, RG_FGADC_CAR_CON0, &lo);
	if (!ret)
		ret = regmap_read(map, RG_FGADC_CAR_CON1, &hi);
	if (ret)
		goto release;
	s->car = (s32)((hi << 16) | (lo & 0xffff));

	ret = regmap_read(map, RG_FGADC_IAVG_CON0, &lo);
	if (!ret)
		ret = regmap_read(map, RG_FGADC_IAVG_CON1, &hi);
	if (ret)
		goto release;
	s->iavg_valid = !!(hi & FG_IAVG_VLD);
	s->iavg = sign_extend32(((hi & 0xfff) << 16) | (lo & 0xffff), 27);

release:
	ret2 = regmap_update_bits(map, RG_FGADC_CON1,
				  FG_SW_CLEAR | FG_SW_READ_PRE, FG_SW_CLEAR);
	if (!ret2)
		ret2 = regmap_read_poll_timeout(map, RG_FGADC_CON1, v,
						!(v & FG_LATCHDATA_ST), 100, 20000);
	regmap_update_bits(map, RG_FGADC_CON1, FG_SW_CLEAR, 0);
	return ret ? ret : ret2;
}

static int q25_fg_get_vbat_uv(struct q25_fg *fg, int *uv)
{
	int mv, ret;

	ret = iio_read_channel_processed(fg->vbat_chan, &mv);
	if (ret < 0)
		return ret;
	if (mv <= 0)
		return -EIO;
	*uv = mv * 1000;
	return 0;
}

static int q25_fg_get_temp_dc(int *temp_dc)
{
	struct thermal_zone_device *tz;
	int ret, mc;

	tz = thermal_zone_get_zone_by_name("battery-thermal");
	if (IS_ERR(tz))
		return PTR_ERR(tz);
	ret = thermal_zone_get_temp(tz, &mc);
	if (ret)
		return ret;
	*temp_dc = mc / 100;
	return 0;
}

static int q25_fg_ocv_soc(struct q25_fg *fg, int ocv_uv)
{
	int soc;

	/* single 25 C table; the vendor 10 C (t2) profile is identical */
	soc = power_supply_batinfo_ocv2cap(fg->psy->battery_info, ocv_uv, 25);
	return clamp(soc, 0, 100);
}

static void q25_fg_anchor(struct q25_fg *fg, s64 car_uah, s64 charge_uah,
			  const char *why)
{
	charge_uah = clamp_t(s64, charge_uah, 0, fg->qmax_uah);
	if (!fg->anchored || abs(charge_uah - fg->charge_uah) > fg->qmax_uah / 100)
		dev_info(fg->dev, "anchor %s: %lld -> %lld uAh (V=%d OCV~%d I=%d uA)\n",
			 why, fg->anchored ? fg->charge_uah : -1LL, charge_uah,
			 fg->vbat_uv, fg->ocv_uv, fg->cur_ua);
	fg->anchor_car_uah = car_uah;
	fg->anchor_charge_uah = charge_uah;
	fg->charge_uah = charge_uah;
	fg->anchored = true;
}

static int q25_fg_charger_status(struct q25_fg *fg, bool *online)
{
	union power_supply_propval val;
	int ret;

	ret = power_supply_am_i_supplied(fg->psy);
	*online = ret > 0;
	if (!*online)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	ret = power_supply_get_property_from_supplier(fg->psy,
				POWER_SUPPLY_PROP_STATUS, &val);
	if (ret)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	switch (val.intval) {
	case POWER_SUPPLY_STATUS_FULL:
	case POWER_SUPPLY_STATUS_CHARGING:
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
		return val.intval;
	default:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
}

static void q25_fg_update(struct q25_fg *fg)
{
	struct q25_fg_snap s;
	bool online, changed;
	int status, soc, soc_ocv, soc_cc, vbat_uv, old_soc, old_status;
	s64 car_uah, charge;
	u64 now = ktime_get_boottime_ns();
	int ret;

	ret = q25_fg_read_snap(fg, &s);
	if (ret) {
		dev_warn_ratelimited(fg->dev, "FG read failed: %d\n", ret);
		return;
	}
	if (q25_fg_get_vbat_uv(fg, &vbat_uv))
		vbat_uv = 0;
	status = q25_fg_charger_status(fg, &online);

	mutex_lock(&fg->lock);
	old_soc = fg->soc;
	old_status = fg->status;

	/* CAR: LSB 190.735 uAs @10 mOhm; nAs / 3600 = nAh */
	car_uah = div_s64(q25_fg_scale(fg, div_s64((s64)s.car * FG_CAR_LSB_NAS,
						   3600)), 1000);
	fg->car_uah = car_uah;
	fg->cur_ua = (int)div_s64(q25_fg_scale(fg, (s64)s.cur * FG_CUR_LSB_NA), 1000);
	fg->avg_valid = s.iavg_valid;
	fg->avg_ua = (int)div_s64(q25_fg_scale(fg, (s64)s.iavg * FG_IAVG_LSB_NA), 1000);
	if (vbat_uv)
		fg->vbat_uv = vbat_uv;
	/* charging current raises the terminal voltage above OCV */
	fg->ocv_uv = fg->vbat_uv -
		(int)div_s64((s64)fg->cur_ua * fg->rint_uohm, 1000000);
	soc_ocv = q25_fg_ocv_soc(fg, fg->ocv_uv);

	if (!fg->anchored) {
		if (!vbat_uv)
			goto out_unlock;
		q25_fg_anchor(fg, car_uah, div_s64((s64)fg->qmax_uah * soc_ocv, 100),
			      "initial OCV");
	}

	charge = fg->anchor_charge_uah + (car_uah - fg->anchor_car_uah);
	soc_cc = (int)div_s64(charge * 100, fg->qmax_uah);

	if (online && status == POWER_SUPPLY_STATUS_FULL) {
		q25_fg_anchor(fg, car_uah, fg->qmax_uah, "charger full");
	} else if (fg->last_valid && !online && vbat_uv &&
		   now - fg->last_ns >= (u64)RELAX_MIN_S * NSEC_PER_SEC &&
		   abs(fg->cur_ua) < RELAX_MAX_NOW_UA &&
		   abs(div_s64((car_uah - fg->last_car_uah) * 3600,
			       div_u64(now - fg->last_ns, NSEC_PER_SEC))) <
			RELAX_MAX_AVG_UA &&
		   abs(soc_ocv - soc_cc) >= RELAX_MIN_DIFF_PCT) {
		q25_fg_anchor(fg, car_uah, div_s64((s64)fg->qmax_uah * soc_ocv, 100),
			      "relaxed OCV");
	} else if (charge > fg->qmax_uah) {
		q25_fg_anchor(fg, car_uah, fg->qmax_uah, "counted above full");
	} else if (charge <= 0 && soc_ocv >= 3 && vbat_uv) {
		q25_fg_anchor(fg, car_uah, div_s64((s64)fg->qmax_uah * soc_ocv, 100),
			      "counted empty, OCV not");
	} else {
		fg->charge_uah = max_t(s64, charge, 0);
	}

	if (!online && vbat_uv && soc_ocv == 0 && fg->charge_uah > 0) {
		if (++fg->empty_hits >= 2)
			q25_fg_anchor(fg, car_uah, 0, "OCV empty");
	} else {
		fg->empty_hits = 0;
	}

	soc = (int)div_s64(fg->charge_uah * 100 + fg->qmax_uah / 2, fg->qmax_uah);
	soc = clamp(soc, 0, 100);
	/* only the charger's termination may report 100 % while plugged in */
	if (online && status != POWER_SUPPLY_STATUS_FULL && soc > 99)
		soc = 99;
	if (online && status == POWER_SUPPLY_STATUS_FULL)
		soc = 100;

	fg->soc = soc;
	fg->status = status;
	fg->last_car_uah = car_uah;
	fg->last_ns = now;
	fg->last_valid = true;

	dev_dbg(fg->dev,
		"raw cur=%d car=%#x iavg=%d(%d) | I=%d uA Iavg=%d uA CAR=%lld uAh V=%d OCV~%d soc_ocv=%d charge=%lld soc=%d st=%d online=%d\n",
		s.cur, (u32)s.car, s.iavg, s.iavg_valid, fg->cur_ua, fg->avg_ua,
		car_uah, fg->vbat_uv, fg->ocv_uv, soc_ocv, fg->charge_uah, soc,
		status, online);

out_unlock:
	changed = fg->soc != old_soc || fg->status != old_status;
	mutex_unlock(&fg->lock);

	if (changed)
		power_supply_changed(fg->psy);
}

static void q25_fg_work(struct work_struct *work)
{
	struct q25_fg *fg = container_of(work, struct q25_fg, work.work);
	bool online;

	q25_fg_update(fg);
	online = power_supply_am_i_supplied(fg->psy) > 0;
	queue_delayed_work(system_freezable_power_efficient_wq, &fg->work,
			   msecs_to_jiffies(online ? POLL_ONLINE_MS : POLL_OFFLINE_MS));
}

static void q25_fg_external_power_changed(struct power_supply *psy)
{
	struct q25_fg *fg = power_supply_get_drvdata(psy);

	mod_delayed_work(system_freezable_power_efficient_wq, &fg->work,
			 msecs_to_jiffies(500));
}

static enum power_supply_property q25_fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	/*
	 * TECHNOLOGY, CHARGE_FULL_DESIGN and VOLTAGE_{MIN,MAX}_DESIGN are
	 * exposed by the power-supply core from the monitored-battery node.
	 */
};

static int q25_fg_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct q25_fg *fg = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Zinwa Q25 Li-ion";
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Zinwa";
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		return q25_fg_get_temp_dc(&val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = fg->qmax_uah;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* fresh reading, not the cached one */
		return q25_fg_get_vbat_uv(fg, &val->intval);
	default:
		break;
	}

	mutex_lock(&fg->lock);
	if (!fg->anchored) {
		ret = -ENODATA;
		goto out;
	}
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg->status;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = fg->soc;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = fg->ocv_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = fg->cur_ua;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		if (!fg->avg_valid)
			ret = -ENODATA;
		else
			val->intval = fg->avg_ua;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = (int)fg->charge_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = (int)fg->car_uah;
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&fg->lock);
	return ret;
}

/*
 * current_now must be instantaneous; refresh the cache (cheap: a latch and
 * five pwrap reads) when it is read from userspace.
 */
static int q25_fg_get_property_fresh(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct q25_fg *fg = power_supply_get_drvdata(psy);

	if (psp == POWER_SUPPLY_PROP_CURRENT_NOW && fg->anchored) {
		struct q25_fg_snap s;

		if (!q25_fg_read_snap(fg, &s)) {
			val->intval = (int)div_s64(q25_fg_scale(fg,
					(s64)s.cur * FG_CUR_LSB_NA), 1000);
			return 0;
		}
	}
	return q25_fg_get_property(psy, psp, val);
}

static const struct power_supply_desc q25_fg_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = q25_fg_props,
	.num_properties = ARRAY_SIZE(q25_fg_props),
	.get_property = q25_fg_get_property_fresh,
	.external_power_changed = q25_fg_external_power_changed,
};

static void q25_fg_put_device(void *data)
{
	put_device(data);
}

static void q25_fg_cancel_work(void *data)
{
	struct q25_fg *fg = data;

	cancel_delayed_work_sync(&fg->work);
}

static int q25_fg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct power_supply_battery_info *info;
	struct platform_device *pmic_pdev;
	struct device_node *pmic_np;
	struct mt6397_chip *chip;
	struct q25_fg *fg;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->dev = dev;
	mutex_init(&fg->lock);
	mutex_init(&fg->hw_lock);
	INIT_DELAYED_WORK(&fg->work, q25_fg_work);
	fg->status = POWER_SUPPLY_STATUS_UNKNOWN;

	/* The mt6397 MFD has no FG cell; reach the PMIC regmap via phandle. */
	pmic_np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!pmic_np)
		return dev_err_probe(dev, -EINVAL, "missing mediatek,pmic\n");
	pmic_pdev = of_find_device_by_node(pmic_np);
	of_node_put(pmic_np);
	if (!pmic_pdev)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, q25_fg_put_device, &pmic_pdev->dev);
	if (ret)
		return ret;
	chip = platform_get_drvdata(pmic_pdev);
	if (!chip || !chip->regmap)
		return -EPROBE_DEFER;
	fg->regmap = chip->regmap;

	if (of_property_read_u32(dev->of_node, "zinwa,fg-rsense-micro-ohms",
				 &fg->rsense_uohm) || !fg->rsense_uohm)
		return dev_err_probe(dev, -EINVAL, "missing zinwa,fg-rsense-micro-ohms\n");
	if (of_property_read_u32(dev->of_node, "zinwa,fg-car-tune-permille",
				 &fg->car_tune) || !fg->car_tune)
		fg->car_tune = 1000;

	fg->vbat_chan = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(fg->vbat_chan))
		return dev_err_probe(dev, PTR_ERR(fg->vbat_chan), "no vbat channel\n");

	cfg.drv_data = fg;
	cfg.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &q25_fg_desc, &cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy), "psy register failed\n");

	info = fg->psy->battery_info;
	if (!info || info->charge_full_design_uah <= 0 || !info->ocv_table_size[0])
		return dev_err_probe(dev, -EINVAL,
			"monitored-battery needs charge-full-design and an OCV table\n");
	fg->qmax_uah = info->charge_full_design_uah;
	fg->rint_uohm = max(info->factory_internal_resistance_uohm, 0);

	ret = devm_add_action_or_reset(dev, q25_fg_cancel_work, fg);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, fg);

	q25_fg_update(fg);
	queue_delayed_work(system_freezable_power_efficient_wq, &fg->work,
			   msecs_to_jiffies(POLL_ONLINE_MS));

	dev_info(dev, "MT6366 FG: Rsense %u uOhm, car_tune %u, Qmax %d uAh, Rint %d uOhm; I=%d uA V=%d uV OCV~%d uV soc=%d%%\n",
		 fg->rsense_uohm, fg->car_tune, fg->qmax_uah, fg->rint_uohm,
		 fg->cur_ua, fg->vbat_uv, fg->ocv_uv, fg->soc);
	return 0;
}

static int q25_fg_resume(struct device *dev)
{
	struct q25_fg *fg = dev_get_drvdata(dev);

	mod_delayed_work(system_freezable_power_efficient_wq, &fg->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(q25_fg_pm, NULL, q25_fg_resume);

static const struct of_device_id q25_fg_of_match[] = {
	{ .compatible = "zinwa,q25-mt6358-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, q25_fg_of_match);

static struct platform_driver q25_fg_driver = {
	.probe = q25_fg_probe,
	.driver = {
		.name = "q25-mt6358-gauge",
		.of_match_table = q25_fg_of_match,
		.pm = pm_sleep_ptr(&q25_fg_pm),
	},
};
module_platform_driver(q25_fg_driver);

MODULE_DESCRIPTION("Zinwa Q25 MT6366 (MT6358) FGADC coulomb-counter fuel gauge");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
