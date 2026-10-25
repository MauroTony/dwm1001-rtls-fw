#include <math.h>
#include <stddef.h>

#include <rtls/twr_math.h>

double rtls_twr_ds_tof(const struct rtls_twr_intervals *iv)
{
	/*
	 * Os produtos chegam a ~1e17 (intervalos de ms em ticks de 15,65 ps):
	 * estouram facilmente o int64 se os intervalos crescerem, e em double
	 * o erro de arredondamento fica na ordem de 1e-8 tick.
	 */
	const double ra = (double)iv->round_a;
	const double da = (double)iv->reply_a;
	const double rb = (double)iv->round_b;
	const double db = (double)iv->reply_b;
	const double den = ra + rb + da + db;

	if (den <= 0.0) {
		return 0.0;
	}

	return (ra * rb - da * db) / den;
}

double rtls_twr_tof_to_m(double tof_ticks)
{
	return tof_ticks * RTLS_DW_TICK_S * RTLS_SPEED_OF_LIGHT_AIR;
}

/* Potência transmitida: -41,3 dBm/MHz em 500 MHz (APS011, seção 3.4). */
#define BIAS_TX_POWER_DBM (-14.3f)

/* 20 log10(c) - 20 log10(4 pi f_c) para f_c = 6489,6 MHz (canal 5), c no vácuo. */
#define BIAS_FRIIS_CH5_DB (169.5362f - 218.2284f)

/* Menor distância usada no cálculo do RSL (evita log de zero ou negativo). */
#define BIAS_MIN_DISTANCE_M 0.01f

/*
 * APS011 v1.2, tabela 2: correção (cm) para PRF 64 MHz e banda de 500 MHz, com
 * RSL de -61 dBm a -93 dBm em passos de 2 dB. O zero em -77 dBm corresponde à
 * calibração de antena a 5,0 m no canal 5 (tabela 3).
 */
#define BIAS_RSL_FIRST_DBM (-61.0f)
#define BIAS_RSL_STEP_DB   2.0f

static const float bias_prf64_cm[] = {
	-11.0f, -10.5f, -10.0f, -9.3f, -8.2f, -6.9f, -5.1f, -2.7f, 0.0f,
	2.1f,   3.5f,   4.2f,   4.9f,  6.2f,  7.1f,  7.6f,  8.1f,
};

#define BIAS_TABLE_LEN (sizeof(bias_prf64_cm) / sizeof(bias_prf64_cm[0]))

float rtls_twr_rsl_dbm(float distance_m, float gain_db)
{
	const float d = (distance_m < BIAS_MIN_DISTANCE_M) ? BIAS_MIN_DISTANCE_M : distance_m;

	return BIAS_TX_POWER_DBM + gain_db + BIAS_FRIIS_CH5_DB - 20.0f * log10f(d);
}

float rtls_twr_range_bias_m(float rsl_dbm)
{
	/* Posição fracionária na tabela: 0 em -61 dBm, BIAS_TABLE_LEN - 1 em -93 dBm. */
	const float pos = (BIAS_RSL_FIRST_DBM - rsl_dbm) / BIAS_RSL_STEP_DB;
	size_t i;
	float frac;

	if (pos <= 0.0f) {
		return bias_prf64_cm[0] / 100.0f;
	}
	if (pos >= (float)(BIAS_TABLE_LEN - 1)) {
		return bias_prf64_cm[BIAS_TABLE_LEN - 1] / 100.0f;
	}

	i = (size_t)pos;
	frac = pos - (float)i;
	return (bias_prf64_cm[i] + frac * (bias_prf64_cm[i + 1] - bias_prf64_cm[i])) / 100.0f;
}

float rtls_twr_correct_range(float distance_m, float gain_db)
{
	return distance_m - rtls_twr_range_bias_m(rtls_twr_rsl_dbm(distance_m, gain_db));
}
