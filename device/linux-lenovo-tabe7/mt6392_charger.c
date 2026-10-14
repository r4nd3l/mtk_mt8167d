// SPDX-License-Identifier: GPL-2.0
/*
 * Battery charger driver for the MediaTek MT6392 PMIC.
 *
 * The MT6392 has an integrated charger rather than a separate charger IC. The
 * PMIC runs the constant-voltage loop in hardware (RG_HWCV_EN), so this driver
 * only has to program the CV threshold and the charge current, enable the
 * charge path, and keep the charger watchdog fed.
 *
 * The watchdog matters: it defaults to a 4 second timeout, and if nothing kicks
 * it the hardware stops charging. That is why a kernel without this driver
 * drains even while plugged in.
 *
 * Register fields were taken from the MT8167 vendor kernel
 * (drivers/misc/mediatek/power/mt8167/charging_hw_pmic.c and
 * include/mt-plat/mt8167/include/mach/upmu_hw.h).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6392/registers.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/* CHR_CON0 */
#define MT6392_VCDT_HV_EN	BIT(0)
#define MT6392_CSDAC_EN		BIT(3)
#define MT6392_CHR_EN		BIT(4)
#define MT6392_CHRDET		BIT(5)	/* read-only: charger present */
/* CHR_CON2 */
#define MT6392_VBAT_CV_EN	BIT(1)
#define MT6392_CS_EN		BIT(3)
/* CHR_CON3 */
#define MT6392_VBAT_CV_VTH	GENMASK(4, 0)
/* CHR_CON4 */
#define MT6392_CS_VTH		GENMASK(3, 0)
/* CHR_CON13 */
#define MT6392_CHRWDT_TD	GENMASK(3, 0)
#define MT6392_CHRWDT_EN	BIT(4)
#define MT6392_CHRWDT_WR	BIT(8)
/* CHR_CON15 */
#define MT6392_CHRWDT_INT_EN	BIT(0)
#define MT6392_CHRWDT_FLAG_WR	BIT(1)
/* CHR_CON20 */
#define MT6392_CSDAC_STP_INC	GENMASK(2, 0)
#define MT6392_CSDAC_STP_DEC	GENMASK(6, 4)
/* CHR_CON21 */
#define MT6392_CSDAC_DLY	GENMASK(2, 0)
#define MT6392_CSDAC_STP	GENMASK(6, 4)
/* CHR_CON23 */
#define MT6392_HWCV_EN		BIT(6)

#define MT6392_CHRWDT_TD_4S	0x0

/*
 * The vendor uses 900 mA for a real AC adapter, but the bundled charger is
 * 5 V / 550 mA and collapses above ~500 mA, which the PMIC answers by not
 * charging at all. So every plug-in starts at 900 mA and the watchdog work
 * drops to the USB value as soon as VBUS sags.
 * ponytail: VBUS-sag fallback instead of BC1.1 charger-type detection.
 */
#define MT6392_CC_MAX_UA	900000
#define MT6392_CC_SAFE_UA	450000
#define MT6392_CHR_LDO_DET	BIT(1)	/* read-only: charger LDO up */
#define MT6392_VCDT_LV_DET	BIT(6)	/* read-only: VBUS above the low threshold */

/*
 * Bits the vendor charging_hw_init() programs once at boot. Addresses, masks
 * and values are from MediaTek's upmu_hw.h / charging_hw_pmic.c for MT8167.
 */
/* CHR_CON6 */
#define MT6392_VBAT_OV_EN	BIT(0)
#define MT6392_VBAT_OV_VTH	GENMASK(3, 1)
#define MT6392_VBAT_OV_VTH_4V3	0x1
/* CHR_CON7 */
#define MT6392_BATON_EN		BIT(0)
#define MT6392_BATON_HT_EN	BIT(1)
/* CHR_CON10 */
#define MT6392_PCHR_FLAG_EN	BIT(4)
/* CHR_CON16 */
#define MT6392_USBDL_RST	BIT(2)
#define MT6392_USBDL_SET	BIT(3)
/* CHR_CON18 */
#define MT6392_BC11_BB_CTRL	BIT(0)
#define MT6392_BC11_RST		BIT(1)
/* CHR_CON22 */
#define MT6392_LOW_ICH_DB	GENMASK(5, 0)
/* CHR_CON23, more bits */
#define MT6392_VCDT_MODE	BIT(1)
#define MT6392_CSDAC_MODE	BIT(2)
#define MT6392_ULC_DET_EN	BIT(7)

/* AUXADC channel 0 is BATSNS. Sequence from the vendor pmic_auxadc.c. */
#define MT6392_AUXADC_RQST_CH0	BIT(0)
#define MT6392_AUXADC_RDY	BIT(15)
#define MT6392_AUXADC_DATA	GENMASK(14, 0)
#define MT6392_WDT_KICK_MS	2000	/* half the 4 s timeout */

/*
 * Constant-voltage thresholds, microvolts, indexed by RG_VBAT_CV_VTH.
 * Verbatim from VBAT_CV_VTH[] in the vendor driver. Getting this table wrong
 * overcharges the cell, so it is spelled out rather than computed.
 */
static const u32 mt6392_cv_table[] = {
	3775000, 3800000, 3850000, 3900000,
	4000000, 4050000, 4100000, 4125000,
	4137500, 4150000, 4162500, 4175000,
	4187500, 4200000, 4212500, 4225000,
	4237500, 4250000, 4262500, 4275000,
	4287500, 4300000, 4312500, 4325000,
	4337500, 4350000, 4362500, 4375000,
	4387500, 4400000, 4412500, 4425000,
};

/*
 * Charge current, microamps, indexed by RG_CS_VTH. Note this table is
 * DESCENDING: index 0 is the highest current. From CS_VTH[] in the vendor
 * driver.
 */
static const u32 mt6392_cc_table[] = {
	1600000, 1500000, 1400000, 1300000,
	1200000, 1100000, 1000000,  900000,
	 800000,  700000,  650000,  550000,
	 450000,  300000,  200000,   70000,
};

/*
 * Open-circuit voltage (mV) against state of charge (%), from the vendor
 * battery profile at 25 C (p98995_bsp_bat_setting.dtsi, battery_profile_t2,
 * capacity = 100 - depth of discharge). The pack is a 4.35 V cell.
 */
static const struct { u16 mv; u8 pct; } mt6392_ocv[] = {
	{ 4379, 100 }, { 4334, 96 }, { 4291, 93 }, { 4248, 89 }, { 4207, 86 },
	{ 4166, 82 }, { 4126, 78 }, { 4088, 75 }, { 4055, 71 }, { 4015, 68 },
	{ 3982, 64 }, { 3955, 61 }, { 3915, 57 }, { 3879, 53 }, { 3856, 50 },
	{ 3837, 46 }, { 3822, 43 }, { 3808, 39 }, { 3797, 35 }, { 3787, 32 },
	{ 3777, 28 }, { 3765, 25 }, { 3751, 21 }, { 3738, 18 }, { 3718, 14 },
	{ 3695, 10 }, { 3690, 7 }, { 3645, 3 }, { 3569, 2 }, { 3488, 1 },
	{ 3400, 0 },
};

/*
 * Charging lifts the terminal voltage by I*R: measured +70 mV at 450 mA and
 * +155 mV at 900 mA, so about 170 mOhm. Scale with the programmed current.
 * ponytail: programmed, not measured current; overcorrects in the CV taper.
 */
#define MT6392_CHG_IR_UV_PER_MA	170
#define MT6392_CHG_IR_COMP_UV(chg)	((chg)->cc_ua / 1000 * MT6392_CHG_IR_UV_PER_MA)
/* "Full" once the smoothed reading sits this close to CV while charging. */
#define MT6392_FULL_MARGIN_UV	20000

struct mt6392_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *bat;
	int vbat_avg_uv;	/* exponential average, 0 = no sample yet */
	int last_capacity;
	struct delayed_work wdt_work;
	struct mutex lock;	/* enabled + charge path writes */
	u32 cv_uv;
	u32 cc_ua;
	bool enabled;
	bool was_online;
};

static void mt6392_battery_sample(struct mt6392_charger *chg);

/* Largest table entry that does not exceed the target. Never rounds up. */
static int mt6392_lookup_floor(const u32 *tbl, size_t n, u32 target)
{
	int best = -EINVAL;
	u32 best_val = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		if (tbl[i] <= target && (best < 0 || tbl[i] > best_val)) {
			best = i;
			best_val = tbl[i];
		}
	}
	return best;
}

static int mt6392_charger_online(struct mt6392_charger *chg, bool *online)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MT6392_CHR_CON0, &val);
	if (ret)
		return ret;
	*online = val & MT6392_CHRDET;
	return 0;
}

/*
 * One-time charger block setup, mirroring the vendor charging_hw_init() step
 * for step. Without these the charge path stays armed but no current flows:
 * BATON_EN tells the PMIC a battery is actually present, BC11_RST/USBDL_RST
 * release the USB detection state the bootloader may leave behind, and the
 * over-voltage protection has to be enabled with a threshold above CV.
 */
static int mt6392_charger_hw_init(struct mt6392_charger *chg)
{
	static const struct { unsigned int reg, mask, val; } seq[] = {
		{ MT6392_CHR_CON23, MT6392_VCDT_MODE, 0 },
		{ MT6392_CHR_CON0,  MT6392_VCDT_HV_EN, MT6392_VCDT_HV_EN },
		{ MT6392_CHR_CON16, MT6392_USBDL_SET, 0 },
		{ MT6392_CHR_CON16, MT6392_USBDL_RST, MT6392_USBDL_RST },
		{ MT6392_CHR_CON18, MT6392_BC11_BB_CTRL, MT6392_BC11_BB_CTRL },
		{ MT6392_CHR_CON18, MT6392_BC11_RST, MT6392_BC11_RST },
		{ MT6392_CHR_CON23, MT6392_CSDAC_MODE, MT6392_CSDAC_MODE },
		{ MT6392_CHR_CON6,  MT6392_VBAT_OV_EN, MT6392_VBAT_OV_EN },
		{ MT6392_CHR_CON6,  MT6392_VBAT_OV_VTH,
		  FIELD_PREP_CONST(MT6392_VBAT_OV_VTH, MT6392_VBAT_OV_VTH_4V3) },
		{ MT6392_CHR_CON7,  MT6392_BATON_EN, MT6392_BATON_EN },
		{ MT6392_CHR_CON7,  MT6392_BATON_HT_EN, 0 },
		{ MT6392_CHR_CON23, MT6392_ULC_DET_EN, MT6392_ULC_DET_EN },
		{ MT6392_CHR_CON22, MT6392_LOW_ICH_DB, FIELD_PREP_CONST(MT6392_LOW_ICH_DB, 1) },
		{ MT6392_CHR_CON10, MT6392_PCHR_FLAG_EN, MT6392_PCHR_FLAG_EN },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = regmap_update_bits(chg->regmap, seq[i].reg, seq[i].mask, seq[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6392_charger_kick_wdt(struct mt6392_charger *chg)
{
	int ret;

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON13,
				 MT6392_CHRWDT_TD, MT6392_CHRWDT_TD_4S);
	if (ret)
		return ret;
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON13,
				 MT6392_CHRWDT_WR, MT6392_CHRWDT_WR);
	if (ret)
		return ret;
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON15,
				 MT6392_CHRWDT_INT_EN, MT6392_CHRWDT_INT_EN);
	if (ret)
		return ret;
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON13,
				 MT6392_CHRWDT_EN, MT6392_CHRWDT_EN);
	if (ret)
		return ret;
	return regmap_update_bits(chg->regmap, MT6392_CHR_CON15,
				  MT6392_CHRWDT_FLAG_WR, MT6392_CHRWDT_FLAG_WR);
}

static int mt6392_charger_set_limits(struct mt6392_charger *chg)
{
	int cv, cc, ret;

	cv = mt6392_lookup_floor(mt6392_cv_table, ARRAY_SIZE(mt6392_cv_table),
				 chg->cv_uv);
	if (cv < 0) {
		dev_err(chg->dev, "no CV step at or below %u uV\n", chg->cv_uv);
		return cv;
	}
	cc = mt6392_lookup_floor(mt6392_cc_table, ARRAY_SIZE(mt6392_cc_table),
				 chg->cc_ua);
	if (cc < 0) {
		dev_err(chg->dev, "no current step at or below %u uA\n", chg->cc_ua);
		return cc;
	}

	dev_info(chg->dev, "CV %u uV (step %d), current %u uA (step %d)\n",
		 mt6392_cv_table[cv], cv, mt6392_cc_table[cc], cc);

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON3,
				 MT6392_VBAT_CV_VTH, cv);
	if (ret)
		return ret;
	return regmap_update_bits(chg->regmap, MT6392_CHR_CON4,
				  MT6392_CS_VTH, cc);
}

/* Charge path off. Caller holds chg->lock. Does not touch the work item. */
static void mt6392_charger_hw_disable(struct mt6392_charger *chg)
{
	regmap_update_bits(chg->regmap, MT6392_CHR_CON15,
		   MT6392_CHRWDT_INT_EN | MT6392_CHRWDT_FLAG_WR, 0);
	regmap_update_bits(chg->regmap, MT6392_CHR_CON13,
		   MT6392_CHRWDT_EN, 0);
	regmap_update_bits(chg->regmap, MT6392_CHR_CON0,
		   MT6392_CSDAC_EN | MT6392_CHR_EN, 0);
	regmap_update_bits(chg->regmap, MT6392_CHR_CON23,
		   MT6392_HWCV_EN, 0);
	chg->enabled = false;
}

static int mt6392_charger_enable(struct mt6392_charger *chg, bool enable)
{
	int ret;

	if (!enable) {
		/* Never called from the watchdog work: that path uses hw_disable. */
		cancel_delayed_work_sync(&chg->wdt_work);
		mutex_lock(&chg->lock);
		mt6392_charger_hw_disable(chg);
		mutex_unlock(&chg->lock);
		return 0;
	}
	mutex_lock(&chg->lock);

	/* Limits first, so the charge path never comes up unconfigured. */
	ret = mt6392_charger_set_limits(chg);
	if (ret)
		goto unlock;

	/* Current-source DAC slew settings, values from the vendor driver. */
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON21,
				 MT6392_CSDAC_DLY | MT6392_CSDAC_STP,
				 FIELD_PREP(MT6392_CSDAC_DLY, 0x4) |
				 FIELD_PREP(MT6392_CSDAC_STP, 0x1));
	if (ret)
		goto unlock;
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON20,
				 MT6392_CSDAC_STP_INC | MT6392_CSDAC_STP_DEC,
				 FIELD_PREP(MT6392_CSDAC_STP_INC, 0x1) |
				 FIELD_PREP(MT6392_CSDAC_STP_DEC, 0x2));
	if (ret)
		goto unlock;

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON2,
				 MT6392_CS_EN | MT6392_VBAT_CV_EN,
				 MT6392_CS_EN | MT6392_VBAT_CV_EN);
	if (ret)
		goto unlock;

	/* Hardware constant-voltage loop. Must be on before the charge path. */
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON23,
				 MT6392_HWCV_EN, MT6392_HWCV_EN);
	if (ret)
		goto unlock;

	ret = mt6392_charger_kick_wdt(chg);
	if (ret)
		goto unlock;

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON0,
				 MT6392_VCDT_HV_EN | MT6392_CSDAC_EN | MT6392_CHR_EN,
				 MT6392_VCDT_HV_EN | MT6392_CSDAC_EN | MT6392_CHR_EN);
	if (ret)
		goto unlock;

	chg->enabled = true;
	schedule_delayed_work(&chg->wdt_work, msecs_to_jiffies(MT6392_WDT_KICK_MS));
	mutex_unlock(&chg->lock);
	return 0;
unlock:
	mutex_unlock(&chg->lock);
	return ret;
}

/*
 * Weak adapters fold under 900 mA: VBUS sags below the VCDT low threshold
 * (or the charger LDO drops) and nothing flows. Step down to the USB value
 * then, and go back to 900 mA on the next plug-in.
 */
static void mt6392_charger_check_vbus(struct mt6392_charger *chg)
{
	unsigned int val;
	bool online;

	if (regmap_read(chg->regmap, MT6392_CHR_CON0, &val))
		return;
	online = val & MT6392_CHRDET;

	mutex_lock(&chg->lock);
	if (!online && chg->was_online) {
		chg->cc_ua = MT6392_CC_MAX_UA;
		mt6392_charger_set_limits(chg);
	} else if (online && chg->enabled && chg->cc_ua > MT6392_CC_SAFE_UA &&
		   (val & (MT6392_VCDT_LV_DET | MT6392_CHR_LDO_DET)) !=
		   (MT6392_VCDT_LV_DET | MT6392_CHR_LDO_DET)) {
		dev_warn(chg->dev, "VBUS sags at %u uA (CHR_CON0 0x%x), dropping to %u uA\n",
			 chg->cc_ua, val, MT6392_CC_SAFE_UA);
		chg->cc_ua = MT6392_CC_SAFE_UA;
		mt6392_charger_set_limits(chg);
	}
	chg->was_online = online;
	mutex_unlock(&chg->lock);
}

static void mt6392_charger_wdt_work(struct work_struct *work)
{
	struct mt6392_charger *chg = container_of(to_delayed_work(work),
						  struct mt6392_charger, wdt_work);

	mt6392_charger_check_vbus(chg);

	/*
	 * Keep kicking even with no charger plugged in: the PMIC does not
	 * charge without VBUS anyway, and this way charging resumes by itself
	 * when the cable comes back. Stopping here would need a VBUS interrupt
	 * to restart, and must never cancel this very work synchronously.
	 */
	if (mt6392_charger_kick_wdt(chg))
		dev_warn_ratelimited(chg->dev, "watchdog kick failed\n");

	mt6392_battery_sample(chg);

	schedule_delayed_work(&chg->wdt_work, msecs_to_jiffies(MT6392_WDT_KICK_MS));
}

/*
 * Battery voltage through the PMIC's own AUXADC. 15-bit result against a
 * 1.8 V reference, and BATSNS sits behind a 1:3 divider, exactly as the
 * vendor driver scales it. One conversion takes about 1.5 ms.
 */
static int mt6392_charger_read_vbat(struct mt6392_charger *chg, int *uv)
{
	unsigned int val;
	int ret, i;

	ret = regmap_write(chg->regmap, MT6392_AUXADC_RQST0_SET,
			   MT6392_AUXADC_RQST_CH0);
	if (ret)
		return ret;
	usleep_range(1500, 2000);

	for (i = 0; i < 50; i++) {
		ret = regmap_read(chg->regmap, MT6392_AUXADC_ADC0, &val);
		if (ret)
			return ret;
		if (val & MT6392_AUXADC_RDY) {
			*uv = ((u64)FIELD_GET(MT6392_AUXADC_DATA, val) * 3 * 1800000) >> 15;
			return 0;
		}
		usleep_range(100, 200);
	}
	return -ETIMEDOUT;
}

/* Linear interpolation in the OCV table. ponytail: no temperature or ageing
 * compensation; a coulomb counter would replace this. */
static int mt6392_ocv_to_capacity(int uv)
{
	int mv = uv / 1000, i;

	if (mv >= mt6392_ocv[0].mv)
		return 100;
	for (i = 1; i < ARRAY_SIZE(mt6392_ocv); i++) {
		if (mv >= mt6392_ocv[i].mv) {
			int hi = mt6392_ocv[i - 1].mv, lo = mt6392_ocv[i].mv;
			int phi = mt6392_ocv[i - 1].pct, plo = mt6392_ocv[i].pct;

			return plo + (mv - lo) * (phi - plo) / (hi - lo);
		}
	}
	return 0;
}

static bool mt6392_charging(struct mt6392_charger *chg)
{
	bool online;

	return chg->enabled && !mt6392_charger_online(chg, &online) && online;
}

/* Runs every watchdog tick (4 s): smooth VBAT, notify on capacity change. */
static void mt6392_battery_sample(struct mt6392_charger *chg)
{
	int uv, cap;

	if (!chg->bat || mt6392_charger_read_vbat(chg, &uv))
		return;
	if (mt6392_charging(chg))
		uv -= MT6392_CHG_IR_COMP_UV(chg);
	/* 1/8 weight: ~30 s time constant, flattens load spikes */
	chg->vbat_avg_uv = chg->vbat_avg_uv ? chg->vbat_avg_uv + (uv - chg->vbat_avg_uv) / 8 : uv;

	cap = mt6392_ocv_to_capacity(chg->vbat_avg_uv);
	if (cap != chg->last_capacity) {
		chg->last_capacity = cap;
		power_supply_changed(chg->bat);
	}
}

static int mt6392_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6392_charger *chg = power_supply_get_drvdata(psy);
	bool online;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = mt6392_charger_online(chg, &online);
		if (ret)
			return ret;
		if (!online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (!chg->enabled)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (chg->vbat_avg_uv + MT6392_CHG_IR_COMP_UV(chg) >=
			 chg->cv_uv - MT6392_FULL_MARGIN_UV)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return mt6392_charger_read_vbat(chg, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		val->intval = chg->vbat_avg_uv;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = mt6392_ocv_to_capacity(chg->vbat_avg_uv);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = 2787000;	/* vendor q_max_pos_25, mAh -> uAh */
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = 3400000;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = 4350000;
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property mt6392_battery_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
};

static const struct power_supply_desc mt6392_battery_desc = {
	.name		= "mt6392-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= mt6392_battery_properties,
	.num_properties	= ARRAY_SIZE(mt6392_battery_properties),
	.get_property	= mt6392_battery_get_property,
};

static int mt6392_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6392_charger *chg = power_supply_get_drvdata(psy);
	bool online;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = mt6392_charger_online(chg, &online);
		if (ret)
			return ret;
		val->intval = online;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = mt6392_charger_online(chg, &online);
		if (ret)
			return ret;
		if (!online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (chg->enabled)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = chg->cv_uv;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = chg->cc_ua;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return mt6392_charger_read_vbat(chg, &val->intval);
	default:
		return -EINVAL;
	}
}

/*
 * Writing STATUS lets userspace switch the charge path off and on without
 * unplugging, which is the only clean way to measure what the charger adds:
 * same load, charger toggled, watch the battery voltage.
 */
static int mt6392_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct mt6392_charger *chg = power_supply_get_drvdata(psy);

	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return mt6392_charger_enable(chg, val->intval == POWER_SUPPLY_STATUS_CHARGING);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		/*
		 * Lets userspace pick the vendor's 900 mA when it knows a wall
		 * adapter is attached (no BC1.1 detection yet). Capped at what the
		 * vendor ever configures for this board.
		 */
		if (val->intval < 0 || val->intval > MT6392_CC_MAX_UA)
			return -EINVAL;
		mutex_lock(&chg->lock);
		chg->cc_ua = val->intval;
		ret = mt6392_charger_set_limits(chg);
		mutex_unlock(&chg->lock);
		return ret;
	default:
		return -EINVAL;
	}
}

static int mt6392_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_STATUS ||
	       psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT;
}

static enum power_supply_property mt6392_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static const struct power_supply_desc mt6392_charger_desc = {
	.name		= "mt6392-charger",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= mt6392_charger_properties,
	.num_properties	= ARRAY_SIZE(mt6392_charger_properties),
	.get_property	= mt6392_charger_get_property,
	.set_property	= mt6392_charger_set_property,
	.property_is_writeable = mt6392_charger_property_is_writeable,
};

static int mt6392_charger_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6392 = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct mt6392_charger *chg;
	bool online;
	int ret;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	chg->regmap = mt6392->regmap;

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);

	/*
	 * Conservative defaults. 4.2 V suits a standard cell; boards with a
	 * 4.35 V pack must say so explicitly in DT rather than inheriting it.
	 */
	mutex_init(&chg->lock);
	chg->cv_uv = 4200000;
	chg->cc_ua = MT6392_CC_MAX_UA;

	ret = mt6392_charger_hw_init(chg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "charger block init failed\n");
	device_property_read_u32(&pdev->dev, "constant-charge-voltage-max-microvolt",
				 &chg->cv_uv);
	device_property_read_u32(&pdev->dev, "constant-charge-current-max-microamp",
				 &chg->cc_ua);

	if (chg->cv_uv > 4350000) {
		dev_err(&pdev->dev, "refusing CV above 4.35 V (%u uV)\n", chg->cv_uv);
		return -EINVAL;
	}

	INIT_DELAYED_WORK(&chg->wdt_work, mt6392_charger_wdt_work);
	platform_set_drvdata(pdev, chg);

	psy_cfg.drv_data = chg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);
	chg->psy = devm_power_supply_register(&pdev->dev, &mt6392_charger_desc,
					      &psy_cfg);
	if (IS_ERR(chg->psy))
		return PTR_ERR(chg->psy);

	chg->last_capacity = -1;
	chg->bat = devm_power_supply_register(&pdev->dev, &mt6392_battery_desc,
					      &psy_cfg);
	if (IS_ERR(chg->bat))
		return PTR_ERR(chg->bat);
	mt6392_battery_sample(chg);

	ret = mt6392_charger_online(chg, &online);
	if (ret)
		return ret;

	if (online) {
		ret = mt6392_charger_enable(chg, true);
		if (ret) {
			dev_err(&pdev->dev, "failed to enable charging: %d\n", ret);
			return ret;
		}
	} else {
		dev_info(&pdev->dev, "no charger detected at probe\n");
		/* Poll so we notice a later plug-in even without the CHRDET irq. */
		schedule_delayed_work(&chg->wdt_work,
				      msecs_to_jiffies(MT6392_WDT_KICK_MS));
	}

	return 0;
}

static void mt6392_charger_remove(struct platform_device *pdev)
{
	struct mt6392_charger *chg = platform_get_drvdata(pdev);

	mt6392_charger_enable(chg, false);
}

static const struct platform_device_id mt6392_charger_id[] = {
	{ "mt6392-charger", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6392_charger_id);

static struct platform_driver mt6392_charger_driver = {
	.driver = {
		.name = "mt6392-charger",
	},
	.probe = mt6392_charger_probe,
	.remove = mt6392_charger_remove,
	.id_table = mt6392_charger_id,
};
module_platform_driver(mt6392_charger_driver);

MODULE_DESCRIPTION("MediaTek MT6392 PMIC battery charger");
MODULE_LICENSE("GPL");
