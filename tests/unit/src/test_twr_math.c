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

ZTEST_SUITE(twr_math, NULL, NULL, NULL, NULL, NULL);
