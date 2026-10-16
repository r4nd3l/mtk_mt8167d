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
 * Battery model from the vendor profile at 25 C (p98995_bsp_bat_setting.dtsi,
 * the 2787 mAh pack): open-circuit voltage against depth of discharge
 * (battery_profile_t2) and cell resistance against OCV (r_profile_t2).
 * The pack is a 4.35 V cell.
 */
static const struct { u16 mv; u8 dod; } mt6392_ocv[] = {
	{ 4379, 0 }, { 4355, 2 }, { 4334, 4 }, { 4312, 5 }, { 4291, 7 },
	{ 4270, 9 }, { 4248, 11 }, { 4227, 13 }, { 4207, 14 }, { 4186, 16 },
	{ 4166, 18 }, { 4146, 20 }, { 4126, 22 }, { 4107, 23 }, { 4088, 25 },
	{ 4072, 27 }, { 4055, 29 }, { 4034, 30 }, { 4015, 32 }, { 3997, 34 },
	{ 3982, 36 }, { 3968, 38 }, { 3955, 39 }, { 3937, 41 }, { 3915, 43 },
	{ 3896, 45 }, { 3879, 47 }, { 3867, 48 }, { 3856, 50 }, { 3847, 52 },
	{ 3837, 54 }, { 3829, 56 }, { 3822, 57 }, { 3815, 59 }, { 3808, 61 },
	{ 3802, 63 }, { 3797, 65 }, { 3791, 66 }, { 3787, 68 }, { 3782, 70 },
	{ 3777, 72 }, { 3771, 74 }, { 3765, 75 }, { 3757, 77 }, { 3751, 79 },
	{ 3745, 81 }, { 3738, 82 }, { 3728, 84 }, { 3718, 86 }, { 3707, 88 },
	{ 3695, 90 }, { 3692, 91 }, { 3690, 93 }, { 3684, 95 }, { 3645, 97 },
	{ 3569, 98 }, { 3488, 99 }, { 3400, 100 },
};

static const struct { u16 mv; u16 mohm; } mt6392_rbat[] = {
	{ 4379, 138 }, { 4355, 138 }, { 4334, 140 }, { 4312, 138 }, { 4291, 140 },
	{ 4270, 140 }, { 4248, 140 }, { 4227, 142 }, { 4207, 142 }, { 4186, 142 },
	{ 4166, 147 }, { 4146, 147 }, { 4126, 148 }, { 4107, 152 }, { 4088, 152 },
	{ 4072, 155 }, { 4055, 160 }, { 4034, 163 }, { 4015, 167 }, { 3997, 165 },
	{ 3982, 167 }, { 3968, 168 }, { 3955, 167 }, { 3937, 162 }, { 3915, 148 },
	{ 3896, 140 }, { 3879, 133 }, { 3867, 135 }, { 3856, 135 }, { 3847, 137 },
	{ 3837, 135 }, { 3829, 138 }, { 3822, 140 }, { 3815, 142 }, { 3808, 143 },
	{ 3802, 145 }, { 3797, 148 }, { 3791, 148 }, { 3787, 152 }, { 3782, 150 },
	{ 3777, 150 }, { 3771, 148 }, { 3765, 147 }, { 3757, 143 }, { 3751, 147 },
	{ 3745, 148 }, { 3738, 150 }, { 3728, 150 }, { 3718, 153 }, { 3707, 157 },
	{ 3695, 155 }, { 3692, 160 }, { 3690, 173 }, { 3684, 192 }, { 3639, 190 },
	{ 3541, 207 }, { 3480, 217 }, { 3400, 237 },
};

#define MT6392_QMAX_MAH		2787	/* vendor q_max_pos_25 */
/*
 * Board and connector resistance on top of the cell's: measured ~170 mOhm
 * total at mid charge against ~140 in r_profile_t2.
 * ponytail: one fixed value; calibrate here if the estimate drifts.
 */
#define MT6392_R_BOARD_MOHM	30
/* The shown percentage moves at most 1 % per this long, like the vendor D5. */
#define MT6392_UI_STEP_MS	60000
/* Longest gap integrated as one step (suspend, stalled work). */
#define MT6392_MAX_DT_MS	10000
/* "Full": in CV, within this of the CV setpoint, estimated current below. */
#define MT6392_FULL_MARGIN_MV	30
#define MT6392_FULL_CURRENT_MA	60
/*
 * Restoring the charge saved at shutdown: accepted only shortly after probe
 * and only if the probe-time voltage agrees with it. That reading is taken
 * under boot load, so the true OCV sits at or above it: allow the saved OCV
 * up to this far above, and only a little below (more below = it charged
 * while off, so the fresh reading wins).
 */
#define MT6392_RESTORE_WINDOW_MS	300000
#define MT6392_RESTORE_ABOVE_MV		150
#define MT6392_RESTORE_BELOW_MV		30

struct mt6392_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *bat;
	int vbat_avg_uv;	/* exponential average, for VOLTAGE_AVG */
	/* SW gauge state (vendor "OAM"): charge left, estimated current, shown % */
	int charge_uah;
	int current_ma;		/* estimated, > 0 = discharging */
	int ui_soc;
	unsigned long last_jiffies;
	unsigned int ui_ms;
	unsigned long probe_jiffies;
	int probe_mv;		/* battery voltage at probe, charge path off */
	struct delayed_work wdt_work;
	struct mutex lock;	/* enabled + charge path writes */
	u32 cv_uv;
	u32 cc_ua;
	bool enabled;		/* charge path is on right now */
	bool user_off;		/* userspace wrote STATUS=Not charging */
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

	dev_dbg(chg->dev, "CV %u uV (step %d), current %u uA (step %d)\n",
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

/*
 * Charge path on. Caller holds chg->lock. Also used on every plug-in: the
 * PMIC clears CHR_EN, CSDAC_EN, CS_EN, VBAT_CV_EN and CS_VTH by itself when
 * VBUS goes away (measured 10-15), so nothing flows after a replug unless the
 * whole sequence runs again.
 */
static int mt6392_charger_hw_enable(struct mt6392_charger *chg)
{
	int ret;

	/* Limits first, so the charge path never comes up unconfigured. */
	ret = mt6392_charger_set_limits(chg);
	if (ret)
		return ret;

	/* Current-source DAC slew settings, values from the vendor driver. */
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON21,
				 MT6392_CSDAC_DLY | MT6392_CSDAC_STP,
				 FIELD_PREP(MT6392_CSDAC_DLY, 0x4) |
				 FIELD_PREP(MT6392_CSDAC_STP, 0x1));
	if (ret)
		return ret;
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON20,
				 MT6392_CSDAC_STP_INC | MT6392_CSDAC_STP_DEC,
				 FIELD_PREP(MT6392_CSDAC_STP_INC, 0x1) |
				 FIELD_PREP(MT6392_CSDAC_STP_DEC, 0x2));
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON2,
				 MT6392_CS_EN | MT6392_VBAT_CV_EN,
				 MT6392_CS_EN | MT6392_VBAT_CV_EN);
	if (ret)
		return ret;

	/* Hardware constant-voltage loop. Must be on before the charge path. */
	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON23,
				 MT6392_HWCV_EN, MT6392_HWCV_EN);
	if (ret)
		return ret;

	ret = mt6392_charger_kick_wdt(chg);
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MT6392_CHR_CON0,
				 MT6392_VCDT_HV_EN | MT6392_CSDAC_EN | MT6392_CHR_EN,
				 MT6392_VCDT_HV_EN | MT6392_CSDAC_EN | MT6392_CHR_EN);
	if (ret)
		return ret;

	chg->enabled = true;
	return 0;
}

static int mt6392_charger_enable(struct mt6392_charger *chg, bool enable)
{
	int ret;

	if (!enable) {
		/* Never called from the watchdog work: that path uses hw_disable. */
		cancel_delayed_work_sync(&chg->wdt_work);
		mutex_lock(&chg->lock);
		chg->user_off = true;
		mt6392_charger_hw_disable(chg);
		mutex_unlock(&chg->lock);
		/* keep polling so the battery reading stays fresh */
		schedule_delayed_work(&chg->wdt_work, msecs_to_jiffies(MT6392_WDT_KICK_MS));
		return 0;
	}
	mutex_lock(&chg->lock);
	chg->user_off = false;
	ret = mt6392_charger_hw_enable(chg);
	mutex_unlock(&chg->lock);
	schedule_delayed_work(&chg->wdt_work, msecs_to_jiffies(MT6392_WDT_KICK_MS));
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
		/* the PMIC dropped the charge path itself */
		chg->enabled = false;
		chg->cc_ua = MT6392_CC_MAX_UA;
	} else if (online && !chg->was_online && !chg->user_off) {
		if (mt6392_charger_hw_enable(chg))
			dev_warn(chg->dev, "re-enabling charging after plug-in failed\n");
		else
			dev_info(chg->dev, "charger plugged in, charging\n");
	} else if (online && chg->enabled && chg->cc_ua > MT6392_CC_SAFE_UA &&
		   (val & (MT6392_VCDT_LV_DET | MT6392_CHR_LDO_DET)) !=
		   (MT6392_VCDT_LV_DET | MT6392_CHR_LDO_DET)) {
		dev_warn(chg->dev, "VBUS sags at %u uA (CHR_CON0 0x%x), dropping to %u uA\n",
			 chg->cc_ua, val, MT6392_CC_SAFE_UA);
		chg->cc_ua = MT6392_CC_SAFE_UA;
		mt6392_charger_set_limits(chg);
	}
	if (online != chg->was_online && chg->bat)
		power_supply_changed(chg->bat);
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

/* Charge left (uAh) for an open-circuit voltage, by the OCV table. */
static int mt6392_ocv_to_uah(int mv)
{
	int i, dod_x100;

	if (mv >= mt6392_ocv[0].mv)
		return MT6392_QMAX_MAH * 1000;
	for (i = 1; i < ARRAY_SIZE(mt6392_ocv); i++) {
		if (mv >= mt6392_ocv[i].mv) {
			int hi = mt6392_ocv[i - 1].mv, lo = mt6392_ocv[i].mv;
			int dhi = mt6392_ocv[i - 1].dod, dlo = mt6392_ocv[i].dod;

			dod_x100 = dlo * 100 - (mv - lo) * (dlo - dhi) * 100 / (hi - lo);
			return MT6392_QMAX_MAH * (10000 - dod_x100) / 10;
		}
	}
	return 0;
}

/* Open-circuit voltage (mV) the model expects at this charge. */
static int mt6392_uah_to_ocv(int uah)
{
	int dod_x100 = 10000 - (int)div_s64((s64)uah * 10000, MT6392_QMAX_MAH * 1000);
	int i;

	for (i = 1; i < ARRAY_SIZE(mt6392_ocv); i++) {
		if (dod_x100 <= mt6392_ocv[i].dod * 100) {
			int dhi = mt6392_ocv[i - 1].dod * 100, dlo = mt6392_ocv[i].dod * 100;
			int hi = mt6392_ocv[i - 1].mv, lo = mt6392_ocv[i].mv;

			return hi - (dod_x100 - dhi) * (hi - lo) / (dlo - dhi);
		}
	}
	return mt6392_ocv[ARRAY_SIZE(mt6392_ocv) - 1].mv;
}

/* Cell plus board resistance (mOhm) at this open-circuit voltage. */
static int mt6392_rbat_mohm(int mv)
{
	int i, r = mt6392_rbat[ARRAY_SIZE(mt6392_rbat) - 1].mohm;

	if (mv >= mt6392_rbat[0].mv) {
		r = mt6392_rbat[0].mohm;
	} else {
		for (i = 1; i < ARRAY_SIZE(mt6392_rbat); i++) {
			if (mv >= mt6392_rbat[i].mv) {
				int hi = mt6392_rbat[i - 1].mv, lo = mt6392_rbat[i].mv;
				int rhi = mt6392_rbat[i - 1].mohm, rlo = mt6392_rbat[i].mohm;

				r = rlo + (mv - lo) * (rhi - rlo) / (hi - lo);
				break;
			}
		}
	}
	return r + MT6392_R_BOARD_MOHM;
}

static int mt6392_uah_to_soc(int uah)
{
	return DIV_ROUND_CLOSEST(uah / 10, MT6392_QMAX_MAH);
}

static bool mt6392_charging(struct mt6392_charger *chg)
{
	bool online;

	return chg->enabled && !mt6392_charger_online(chg, &online) && online;
}

static bool mt6392_full(struct mt6392_charger *chg)
{
	return mt6392_charging(chg) &&
	       chg->vbat_avg_uv / 1000 >= chg->cv_uv / 1000 - MT6392_FULL_MARGIN_MV &&
	       -chg->current_ma < MT6392_FULL_CURRENT_MA;
}

/* Average of a few conversions; the vendor gauge takes 15. */
static int mt6392_read_vbat_avg(struct mt6392_charger *chg, int *uv)
{
	int i, v, sum = 0, ret;

	for (i = 0; i < 4; i++) {
		ret = mt6392_charger_read_vbat(chg, &v);
		if (ret)
			return ret;
		sum += v;
	}
	*uv = sum / 4;
	return 0;
}

/*
 * Software gauge, after the vendor SW_FG "OAM" algorithm (battery_meter.c,
 * oam_run). There is no current sense, so the current is inferred from how far
 * the terminal voltage sits from the OCV the model expects: I = (OCV - V) / R.
 * Integrating that current moves the model's charge. If the model is right,
 * the inferred current is the real one, so load sag and the charger's I*R
 * lift no longer show up as fake percent; a wrong model converges back.
 * The shown percentage then follows the model by at most 1 % a minute, and
 * only downwards while discharging, upwards while charging.
 * Runs every watchdog tick (2 s).
 */
static void mt6392_battery_sample(struct mt6392_charger *chg)
{
	unsigned long now = jiffies;
	int uv, mv, ocv, dt_ms, target, old_ui = chg->ui_soc;

	if (!chg->bat || mt6392_read_vbat_avg(chg, &uv))
		return;
	chg->vbat_avg_uv = chg->vbat_avg_uv + (uv - chg->vbat_avg_uv) / 8;

	dt_ms = min_t(unsigned long, jiffies_to_msecs(now - chg->last_jiffies), MT6392_MAX_DT_MS);
	chg->last_jiffies = now;

	mv = uv / 1000;
	ocv = mt6392_uah_to_ocv(chg->charge_uah);
	chg->current_ma = (ocv - mv) * 1000 / mt6392_rbat_mohm(ocv);
	chg->charge_uah -= (int)div_s64((s64)chg->current_ma * dt_ms, 3600);
	chg->charge_uah = clamp(chg->charge_uah, 0, MT6392_QMAX_MAH * 1000);

	chg->ui_ms += dt_ms;
	if (chg->ui_ms >= MT6392_UI_STEP_MS) {
		chg->ui_ms = 0;
		target = mt6392_uah_to_soc(chg->charge_uah);
		if (chg->current_ma > 0 && target < chg->ui_soc)
			chg->ui_soc--;
		else if (chg->current_ma < 0 && target > chg->ui_soc)
			chg->ui_soc++;
	}

	if (chg->ui_soc != old_ui)
		power_supply_changed(chg->bat);
}

/*
 * Starting point for the model: the battery voltage with the charge path off,
 * so the charger's lift is not mistaken for charge. Boot load still drags it
 * a little low; the model recovers from that on its own.
 * ponytail: no state kept across reboots (the vendor stores it in RTC).
 */
static void mt6392_battery_init(struct mt6392_charger *chg)
{
	int uv;

	regmap_update_bits(chg->regmap, MT6392_CHR_CON0,
			   MT6392_CSDAC_EN | MT6392_CHR_EN, 0);
	msleep(500);
	if (mt6392_read_vbat_avg(chg, &uv))
		uv = mt6392_ocv[ARRAY_SIZE(mt6392_ocv) / 2].mv * 1000;
	chg->vbat_avg_uv = uv;
	chg->probe_mv = uv / 1000;
	chg->probe_jiffies = jiffies;
	chg->charge_uah = mt6392_ocv_to_uah(uv / 1000);
	chg->ui_soc = mt6392_uah_to_soc(chg->charge_uah);
	chg->last_jiffies = jiffies;
	dev_info(chg->dev, "battery %d uV at probe, starting at %d %%\n", uv, chg->ui_soc);
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
		else if (mt6392_full(chg))
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
		val->intval = chg->ui_soc;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* estimated by the SW gauge; power_supply sign: < 0 discharging */
		val->intval = -chg->current_ma * 1000;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = mt6392_uah_to_ocv(chg->charge_uah) * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = chg->charge_uah;
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
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
};

/*
 * Writing CHARGE_NOW restores the gauge state saved at the last shutdown
 * (the vendor keeps it in RTC spare bits). Without it every boot restarts
 * from a voltage read under boot load, which reads several percent low.
 */
static int mt6392_battery_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct mt6392_charger *chg = power_supply_get_drvdata(psy);
	int ocv;

	if (psp != POWER_SUPPLY_PROP_CHARGE_NOW)
		return -EINVAL;
	if (val->intval < 0 || val->intval > MT6392_QMAX_MAH * 1000)
		return -EINVAL;
	if (time_after(jiffies, chg->probe_jiffies +
		       msecs_to_jiffies(MT6392_RESTORE_WINDOW_MS)))
		return -EBUSY;

	ocv = mt6392_uah_to_ocv(val->intval);
	if (ocv > chg->probe_mv + MT6392_RESTORE_ABOVE_MV ||
	    ocv < chg->probe_mv - MT6392_RESTORE_BELOW_MV) {
		dev_info(chg->dev, "saved charge (OCV %d mV) disagrees with %d mV at probe, ignored\n",
			 ocv, chg->probe_mv);
		return -ERANGE;
	}

	chg->charge_uah = val->intval;
	chg->ui_soc = mt6392_uah_to_soc(chg->charge_uah);
	dev_info(chg->dev, "restored saved charge: %d uAh, %d %%\n",
		 chg->charge_uah, chg->ui_soc);
	power_supply_changed(chg->bat);
	return 0;
}

static int mt6392_battery_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_NOW;
}

static const struct power_supply_desc mt6392_battery_desc = {
	.name		= "mt6392-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= mt6392_battery_properties,
	.num_properties	= ARRAY_SIZE(mt6392_battery_properties),
	.get_property	= mt6392_battery_get_property,
	.set_property	= mt6392_battery_set_property,
	.property_is_writeable = mt6392_battery_property_is_writeable,
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

	chg->bat = devm_power_supply_register(&pdev->dev, &mt6392_battery_desc,
					      &psy_cfg);
	if (IS_ERR(chg->bat))
		return PTR_ERR(chg->bat);
	mt6392_battery_init(chg);

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

	/* not mt6392_charger_enable(): that re-arms the work */
	cancel_delayed_work_sync(&chg->wdt_work);
	mutex_lock(&chg->lock);
	mt6392_charger_hw_disable(chg);
	mutex_unlock(&chg->lock);
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
