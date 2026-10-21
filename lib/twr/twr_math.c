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
