/*
 * Testes da matemática do DS-TWR (docs/04-algoritmos.md, seção 2).
 */

#include <zephyr/ztest.h>

#include <rtls/twr_math.h>

/* Distância de ~1,57 m em ticks de 15,65 ps. */
#define TRUE_TOF_TICKS 334.0

/*
 * Gera os intervalos de uma rodada ideal com o ToF verdadeiro e relógios com
 * erro relativo e_a (tag) e e_b (âncora). Tempos em ticks "verdadeiros".
 */
static struct rtls_twr_intervals simulate(double tof, double db_true, double da_true, double e_a,
					  double e_b)
{
	struct rtls_twr_intervals iv = {
		.round_a = (uint64_t)((2.0 * tof + db_true) * (1.0 + e_a)),
		.reply_a = (uint64_t)(da_true * (1.0 + e_a)),
		.round_b = (uint64_t)((2.0 * tof + da_true) * (1.0 + e_b)),
		.reply_b = (uint64_t)(db_true * (1.0 + e_b)),
	};

	return iv;
}

ZTEST(twr_math, test_ts_diff_wraps_at_40_bits)
{
	zassert_equal(rtls_dw_ts_diff(1500, 1000), 500);
	zassert_equal(rtls_dw_ts_diff(100, RTLS_DW_TS_MASK - 99), 200);
	zassert_equal(rtls_dw_ts_diff(0, 0), 0);
}

ZTEST(twr_math, test_symmetric_ideal_clocks)
{
	const double us = 63897.6; /* ticks por µs */
	struct rtls_twr_intervals iv =
		simulate(TRUE_TOF_TICKS, 1000.0 * us, 1000.0 * us, 0.0, 0.0);

	zassert_within(rtls_twr_ds_tof(&iv), TRUE_TOF_TICKS, 1.0);
}

ZTEST(twr_math, test_asymmetric_replies_with_clock_drift)
{
	/* Slots diferentes (âncora 3 de 4: Db = 4 ms, Da = 1 ms) e ±20 ppm. */
	const double us = 63897.6;
	struct rtls_twr_intervals iv =
		simulate(TRUE_TOF_TICKS, 4000.0 * us, 1000.0 * us, 20e-6, -15e-6);

	/* Truncamento para inteiro gera até ~1 tick; a deriva em si é desprezível. */
	zassert_within(rtls_twr_ds_tof(&iv), TRUE_TOF_TICKS, 1.0);
}

ZTEST(twr_math, test_tag_intervals_from_timestamps)
{
	struct rtls_twr_intervals iv;

	/* poll_tx próximo do wrap de 40 bits. */
	rtls_twr_tag_intervals(&iv, RTLS_DW_TS_MASK - 10, 100, 500, 7, 9);

	zassert_equal(iv.round_a, 111);
	zassert_equal(iv.reply_a, 400);
	zassert_equal(iv.reply_b, 7);
	zassert_equal(iv.round_b, 9);
}

ZTEST(twr_math, test_invalid_intervals_return_zero)
{
	const struct rtls_twr_intervals iv = {0};

	zassert_equal(rtls_twr_ds_tof(&iv), 0.0);
}

ZTEST(twr_math, test_tof_to_meters)
{
	/* 1 tick ≈ 4,69 mm. */
	zassert_within(rtls_twr_tof_to_m(1.0), 0.004691, 1e-5);
	zassert_within(rtls_twr_tof_to_m(TRUE_TOF_TICKS), 1.5668, 1e-3);
}

ZTEST(twr_math, test_time_unit_conversions)
{
	/* 1 ms = 63 897 600 ticks = 975 UWB µs. */
	zassert_equal(rtls_us_to_dw_ticks(1000), 63897600ULL);
	zassert_equal(rtls_us_to_uus(1000), 975);
	zassert_equal(rtls_us_to_uus(0), 0);
}

ZTEST(twr_math, test_slot_time_wraps_at_40_bits)
{
	/* Slot 2 com T_reply = T_slot = 1 ms, partindo de 101 ticks antes do wrap. */
	zassert_equal(rtls_twr_slot_ts(RTLS_DW_TS_MASK - 100, 1000, 1000, 2), 191692699ULL);
	zassert_equal(rtls_twr_slot_ts(0, 1000, 1000, 0), 63897600ULL);
}

/*
 * Correção de viés por RSL (APS011). Valores esperados calculados à mão:
 * RSL(d) = -14,3 + 169,5362 - 218,2284 - 20 log10(d) = -62,992 - 20 log10(d).
 */
ZTEST(twr_math, test_rsl_by_friis_channel_5)
{
	zassert_within(rtls_twr_rsl_dbm(1.0f, 0.0f), -62.99f, 0.02f);
	zassert_within(rtls_twr_rsl_dbm(5.0f, 0.0f), -76.97f, 0.02f);
	zassert_within(rtls_twr_rsl_dbm(10.0f, 0.0f), -82.99f, 0.02f);
	/* Ganho soma direto no RSL. */
	zassert_within(rtls_twr_rsl_dbm(5.0f, 3.0f), -73.97f, 0.02f);
	/* Distância nula ou negativa não gera NaN. */
	zassert_true(rtls_twr_rsl_dbm(0.0f, 0.0f) > -30.0f);
	zassert_true(rtls_twr_rsl_dbm(-1.0f, 0.0f) > -30.0f);
}

ZTEST(twr_math, test_bias_table_points_and_interpolation)
{
	/* Pontos da tabela 2 (PRF 64 MHz). */
	zassert_within(rtls_twr_range_bias_m(-61.0f), -0.110f, 1e-5f);
	zassert_within(rtls_twr_range_bias_m(-77.0f), 0.0f, 1e-5f);
	zassert_within(rtls_twr_range_bias_m(-93.0f), 0.081f, 1e-5f);
	/* Interpolação linear entre -61 (-11,0 cm) e -63 (-10,5 cm). */
	zassert_within(rtls_twr_range_bias_m(-62.0f), -0.1075f, 1e-5f);
	/* Fora da tabela: valor da extremidade. */
	zassert_within(rtls_twr_range_bias_m(-40.0f), -0.110f, 1e-5f);
	zassert_within(rtls_twr_range_bias_m(-110.0f), 0.081f, 1e-5f);
}

ZTEST(twr_math, test_corrected_range)
{
	/* 1 m: RSL -62,99 dBm, viés -10,50 cm -> a distância real é maior. */
	zassert_within(rtls_twr_correct_range(1.0f, 0.0f), 1.1050f, 0.0005f);
	/* 5 m: ponto de calibração, correção praticamente nula. */
	zassert_within(rtls_twr_correct_range(5.0f, 0.0f), 5.0004f, 0.0005f);
	/* 10 m: RSL -82,99 dBm, viés +4,20 cm -> a distância real é menor. */
	zassert_within(rtls_twr_correct_range(10.0f, 0.0f), 9.9580f, 0.0005f);
}

ZTEST_SUITE(twr_math, NULL, NULL, NULL, NULL, NULL);
