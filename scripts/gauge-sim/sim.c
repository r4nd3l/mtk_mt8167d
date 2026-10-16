/* Host check of the MT6392 SW gauge math (model.inc is cut from the driver). */
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
typedef uint8_t u8; typedef uint16_t u16; typedef int64_t s64;
#define ARRAY_SIZE(a) (int)(sizeof(a)/sizeof((a)[0]))
#define DIV_ROUND_CLOSEST(x,d) (((x)+(d)/2)/(d))
static s64 div_s64(s64 a, int b) { return a / b; }
#include "model.inc"
#define clamp(v,lo,hi) ((v)<(lo)?(lo):(v)>(hi)?(hi):(v))
/* one 2 s tick of the driver's sample(): returns ui */
static int charge, ui, cur, ui_ms;
static void tick(int mv) {
	int ocv = mt6392_uah_to_ocv(charge);
	cur = (ocv - mv) * 1000 / mt6392_rbat_mohm(ocv);
	charge -= (int)div_s64((s64)cur * 2000, 3600);
	charge = clamp(charge, 0, MT6392_QMAX_MAH * 1000);
	if ((ui_ms += 2000) >= 60000) { int t = mt6392_uah_to_soc(charge); ui_ms = 0;
		if (cur > 0 && t < ui) ui--; else if (cur < 0 && t > ui) ui++; }
}
static double real_uah; /* true battery */
static int vterm(int i_ma) { /* terminal V of the true battery at current i (+discharge), R=170 */
	int ocv = mt6392_uah_to_ocv((int)real_uah); return ocv - i_ma * 170 / 1000; }
static void run(const char *name, int start_real_pct, int start_model_mv, int i_ma, int minutes) {
	real_uah = start_real_pct * MT6392_QMAX_MAH * 10.0;
	charge = mt6392_ocv_to_uah(start_model_mv); ui = mt6392_uah_to_soc(charge); ui_ms = 0;
	printf("%s: real %d%%, model starts %d%% (from %d mV)\n", name, start_real_pct, ui, start_model_mv);
	for (int m = 1; m <= minutes; m++) {
		for (int t = 0; t < 30; t++) { real_uah -= i_ma * 2.0 / 3.6; tick(vterm(i_ma)); }
		if (m % (minutes / 6) == 0)
			printf("  %4d min: real %5.1f%%  model %3d%%  ui %3d%%  est %4d mA\n", m,
			       real_uah / MT6392_QMAX_MAH / 10, mt6392_uah_to_soc(charge), ui, cur);
	}
}
int main(void) {
	/* table round trip */
	for (int mv = 3400; mv <= 4379; mv += 7) {
		int back = mt6392_uah_to_ocv(mt6392_ocv_to_uah(mv));
		assert(back >= mv - 2 && back <= mv + 2);
	}
	assert(mt6392_uah_to_soc(MT6392_QMAX_MAH * 1000) == 100 && mt6392_uah_to_soc(0) == 0);
	/* today's case: real ~6 %, boot reading 3.61 V under load, then charging 400 mA net */
	run("boot low, charging", 6, 3610, -400, 120);
	/* last night's case: model started high (paused reading after charge 3.926 V), real 30 %, light use */
	run("start high, discharging 300 mA", 30, 3926, 300, 180);
	/* steady office use from a correct start */
	run("correct start, 450 mA load", 60, 3856 + 0, 450, 120);
	puts("OK");
	return 0;
}
