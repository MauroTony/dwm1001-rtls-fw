/*
 * Testes da localização (docs/04-algoritmos.md, seções 4 e 5): matrizes,
 * multilateração e EKF com trajetórias sintéticas. IDs em docs/08, seção 7.
 */

#include <errno.h>
#include <math.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <rtls/frames.h>
#include <rtls/loc.h>
#include <rtls/loc_mat.h>

#define TAG_Z_M  1.0f
#define CYCLE_MS 100

/* Sala de 8 x 6 m, âncoras no teto (coplanares): o caso 2D do sistema. */
static const int32_t anchors_2d[4][3] = {
	{0, 0, 2500}, {8000, 0, 2500}, {8000, 6000, 2500}, {0, 6000, 2500},
};

/* Alturas alternadas: geometria 3D bem condicionada. */
static const int32_t anchors_3d[4][3] = {
	{0, 0, 500}, {8000, 0, 2500}, {8000, 6000, 500}, {0, 6000, 2500},
};

static struct rtls_loc_ctx ctx;

/* Gerador determinístico (xorshift32) + Box-Muller: mesmo resultado em toda plataforma. */
static uint32_t rng_state;

static void rng_seed(uint32_t seed)
{
	rng_state = seed ? seed : 1U;
}

static float rng_uniform(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return ((float)(rng_state >> 8) + 0.5f) / 16777216.0f;
}

static float rng_gauss(float std)
{
	const float u1 = rng_uniform();
	const float u2 = rng_uniform();

	return std * sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static struct rtls_loc_cfg cfg_2d(float range_std_m)
{
	struct rtls_loc_cfg cfg = {
		.dim = RTLS_LOC_2D,
		.tag_height_m = TAG_Z_M,
		.range_std_m = range_std_m,
		.accel_std_mps2 = 1.0f,
		.gate_chi2 = 6.63f,
	};

	return cfg;
}

/*
 * Monta uma rodada com as âncoras de @p mask (bit i = âncora i). A distância é
 * a verdadeira (tag em @p p, z = p[2]) + ruído gaussiano; @p outlier_idx >= 0
 * soma 1 m nessa âncora.
 */
static void make_set(struct rtls_range_set *set, const int32_t anchors[4][3], const float p[3],
		     int64_t t_ms, uint8_t mask, float noise_std, int outlier_idx)
{
	set->round = (uint8_t)(t_ms / CYCLE_MS);
	set->t_ms = t_ms;
	set->count = 0;

	for (int i = 0; i < 4; i++) {
		struct rtls_range *r;
		float d2 = 0.0f;

		if (!(mask & BIT(i))) {
			continue;
		}

		r = &set->ranges[set->count++];
		r->anchor_id = 0xA001 + i;
		for (int j = 0; j < 3; j++) {
			const float dj = p[j] - (float)anchors[i][j] / 1000.0f;

			r->anchor_pos_mm[j] = anchors[i][j];
			d2 += dj * dj;
		}
		r->distance_m = sqrtf(d2) + rng_gauss(noise_std);
		if (i == outlier_idx) {
			r->distance_m += 1.0f;
		}
		r->distance_raw_m = r->distance_m;
		r->rx_power_cdbm = (int16_t)(-6300 - (int)(200.0f * sqrtf(d2)));
		r->resp_fp_power_cdbm = r->rx_power_cdbm; /* indicador de NLOS = 0 dB */
		r->fp_power_cdbm = r->rx_power_cdbm;
	}
}

static float err_2d(const struct rtls_position *pos, const float p[3])
{
	const float dx = pos->pos_m[0] - p[0];
	const float dy = pos->pos_m[1] - p[1];

	return sqrtf(dx * dx + dy * dy);
}

/* ---------------------------------------------------------------- matrizes */

/* T4.14 */
ZTEST(loc_mat, test_mul_and_mul_bt)
{
	struct rtls_mat a, b, c;

	rtls_mat_zero(&a, 2, 3);
	rtls_mat_zero(&b, 3, 2);
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 3; j++) {
			a.a[i][j] = (float)(1 + 3 * i + j); /* [1 2 3; 4 5 6] */
			b.a[j][i] = (float)(1 + 3 * i + j); /* a^T */
		}
	}

	zassert_ok(rtls_mat_mul(&c, &a, &b));
	zassert_equal(c.rows, 2);
	zassert_equal(c.cols, 2);
	zassert_within(c.a[0][0], 14.0f, 1e-6f);
	zassert_within(c.a[0][1], 32.0f, 1e-6f);
	zassert_within(c.a[1][1], 77.0f, 1e-6f);

	/* a a^T pelo segundo fator transposto dá o mesmo resultado */
	zassert_ok(rtls_mat_mul_bt(&c, &a, &a));
	zassert_within(c.a[1][0], 32.0f, 1e-6f);

	zassert_equal(rtls_mat_mul(&c, &a, &a), -EINVAL);
}

/* T4.14 */
ZTEST(loc_mat, test_cholesky_inverse)
{
	struct rtls_mat a, inv, prod;
	const float m[3][3] = {{4.0f, 2.0f, 0.4f}, {2.0f, 5.0f, 1.0f}, {0.4f, 1.0f, 3.0f}};

	rtls_mat_zero(&a, 3, 3);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			a.a[i][j] = m[i][j];
		}
	}

	zassert_ok(rtls_mat_inv_spd(&inv, &a));
	zassert_ok(rtls_mat_mul(&prod, &a, &inv));
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			zassert_within(prod.a[i][j], (i == j) ? 1.0f : 0.0f, 1e-5f);
		}
	}
	zassert_within(rtls_mat_trace(&a), 12.0f, 1e-6f);
}

/* T4.14 */
ZTEST(loc_mat, test_cholesky_rejects_singular)
{
	struct rtls_mat a, l;

	/* posto 1: u u^T */
	rtls_mat_zero(&a, 2, 2);
	a.a[0][0] = 1.0f;
	a.a[0][1] = 2.0f;
	a.a[1][0] = 2.0f;
	a.a[1][1] = 4.0f;
	zassert_equal(rtls_mat_cholesky(&l, &a), -EDOM);

	/* indefinida */
	a.a[1][1] = -1.0f;
	zassert_equal(rtls_mat_cholesky(&l, &a), -EDOM);

	rtls_mat_zero(&a, 2, 2);
	zassert_equal(rtls_mat_cholesky(&l, &a), -EDOM);
}

ZTEST_SUITE(loc_mat, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------- multilateração */

/* T4.1 */
ZTEST(loc, test_multilat_exact_2d)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {2.7f, 4.1f, TAG_Z_M};
	struct rtls_range_set set;
	float pos[3], gdop;

	make_set(&set, anchors_2d, truth, 0, 0xF, 0.0f, -1);
	zassert_ok(rtls_loc_multilaterate(&cfg, &set, pos, &gdop));
	zassert_within(pos[0], truth[0], 1e-3f);
	zassert_within(pos[1], truth[1], 1e-3f);
	zassert_within(pos[2], TAG_Z_M, 1e-6f);
	zassert_true(gdop > 0.5f && gdop < 3.0f, "GDOP %f", (double)gdop);

	/* 3 âncoras bastam em 2D */
	make_set(&set, anchors_2d, truth, 0, 0x7, 0.0f, -1);
	zassert_ok(rtls_loc_multilaterate(&cfg, &set, pos, NULL));
	zassert_within(pos[0], truth[0], 1e-3f);
	zassert_within(pos[1], truth[1], 1e-3f);
}

/* T4.1 */
ZTEST(loc, test_multilat_exact_3d)
{
	struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {5.2f, 1.9f, 1.3f};
	struct rtls_range_set set;
	float pos[3], gdop;

	cfg.dim = RTLS_LOC_3D;
	make_set(&set, anchors_3d, truth, 0, 0xF, 0.0f, -1);
	zassert_ok(rtls_loc_multilaterate(&cfg, &set, pos, &gdop));
	for (int i = 0; i < 3; i++) {
		zassert_within(pos[i], truth[i], 1e-3f, "eixo %d: %f", i, (double)pos[i]);
	}
	zassert_true(gdop > 0.5f && isfinite(gdop));
}

/* T4.2 */
ZTEST(loc, test_multilat_degenerate)
{
	static const int32_t collinear[4][3] = {
		{0, 0, 2500}, {3000, 0, 2500}, {6000, 0, 2500}, {9000, 0, 2500},
	};
	struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {2.0f, 3.0f, TAG_Z_M};
	struct rtls_range_set set;
	float pos[3];

	make_set(&set, collinear, truth, 0, 0xF, 0.0f, -1);
	zassert_equal(rtls_loc_multilaterate(&cfg, &set, pos, NULL), -EINVAL);

	/* poucas distâncias: 2 em 2D, 3 em 3D */
	make_set(&set, anchors_2d, truth, 0, 0x3, 0.0f, -1);
	zassert_equal(rtls_loc_multilaterate(&cfg, &set, pos, NULL), -EINVAL);
	cfg.dim = RTLS_LOC_3D;
	make_set(&set, anchors_3d, truth, 0, 0x7, 0.0f, -1);
	zassert_equal(rtls_loc_multilaterate(&cfg, &set, pos, NULL), -EINVAL);

	/* 3D com âncoras coplanares (todas no teto) */
	make_set(&set, anchors_2d, truth, 0, 0xF, 0.0f, -1);
	zassert_equal(rtls_loc_multilaterate(&cfg, &set, pos, NULL), -EINVAL);
}

/* --------------------------------------------------------------------- EKF */

/* RMSE 2D de uma tag parada, a partir da rodada 20. */
static float run_static(float noise_std, int outlier_every, uint32_t *n_injected,
			uint32_t *n_flagged)
{
	const struct rtls_loc_cfg cfg = cfg_2d(noise_std);
	const float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	float sum = 0.0f;
	int n = 0;

	*n_injected = 0;
	*n_flagged = 0;
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 300; k++) {
		const bool outlier = outlier_every > 0 && k >= 10 && k % outlier_every == 5;
		struct rtls_range_set set;
		struct rtls_position pos;
		float e;

		make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, 0xF, noise_std,
			 outlier ? 2 : -1);
		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
		zassert_true(pos.flags & RTLS_POS_FLAG_VALID);
		if (outlier) {
			(*n_injected)++;
			*n_flagged += (pos.flags & RTLS_POS_FLAG_OUTLIER) ? 1 : 0;
		}

		e = err_2d(&pos, truth);
		if (k >= 20) {
			sum += e * e;
			n++;
		}
	}

	return sqrtf(sum / n);
}

/* T4.3 */
ZTEST(loc, test_ekf_static)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	struct rtls_position pos;
	struct rtls_range_set set;
	uint32_t inj, flg;
	float rmse;

	rng_seed(1);
	rmse = run_static(0.05f, 0, &inj, &flg);
	zassert_true(rmse <= 0.05f, "RMSE %f m", (double)rmse);
	TC_PRINT("T4.3 parado, sigma 5 cm: RMSE %.1f mm\n", (double)(rmse * 1000.0f));

	/* a primeira rodada sai da multilateração */
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	make_set(&set, anchors_2d, truth, 0, 0xF, 0.05f, -1);
	zassert_ok(rtls_loc_update(&ctx, &set, &pos));
	zassert_true(pos.flags & RTLS_POS_FLAG_INIT);
	zassert_equal(pos.n_ranges, 4);
	zassert_false(pos.flags & RTLS_POS_FLAG_3D);
	zassert_within(pos.pos_m[2], TAG_Z_M, 1e-6f);
}

/* T4.4 */
ZTEST(loc, test_ekf_straight_line)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float p0[2] = {1.0f, 1.0f};
	const float u[2] = {0.8320503f, 0.5547002f}; /* direção (6, 4) normalizada */
	float sum = 0.0f;
	int n = 0;

	rng_seed(2);
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 72; k++) {
		/* 1 m/s = 0,1 m por ciclo */
		const float truth[3] = {p0[0] + 0.1f * k * u[0], p0[1] + 0.1f * k * u[1], TAG_Z_M};
		struct rtls_range_set set;
		struct rtls_position pos;
		float e;

		make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, 0xF, 0.05f, -1);
		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
		e = err_2d(&pos, truth);
		if (k >= 20) {
			sum += e * e;
			n++;
		}
	}

	zassert_true(sqrtf(sum / n) <= 0.10f, "RMSE %f m", (double)sqrtf(sum / n));
	TC_PRINT("T4.4 reta a 1 m/s: RMSE %.1f mm\n", (double)(sqrtf(sum / n) * 1000.0f));
}

/* T4.5 */
ZTEST(loc, test_ekf_rejects_outliers)
{
	struct rtls_loc_stats st;
	uint32_t inj, flg;
	float clean, dirty;

	rng_seed(3);
	clean = run_static(0.05f, 0, &inj, &flg);
	rng_seed(3);
	dirty = run_static(0.05f, 10, &inj, &flg);
	rtls_loc_get_stats(&ctx, &st, false);

	zassert_true(inj >= 25, "outliers injetados: %u", inj);
	zassert_equal(flg, inj, "flag OUTLIER em %u de %u", flg, inj);
	zassert_true(st.rejected >= inj);
	zassert_equal(st.resets, 0);
	zassert_true(dirty <= 1.1f * clean + 0.005f, "RMSE %f com, %f sem", (double)dirty,
		     (double)clean);
	TC_PRINT("T4.5 +1 m em 10%%: %u rejeitadas (%u injetadas), RMSE %.1f x %.1f mm\n",
		 st.rejected, inj, (double)(dirty * 1000.0f), (double)(clean * 1000.0f));
}

/* T4.6 */
ZTEST(loc, test_ekf_two_ranges)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	struct rtls_loc_stats st;
	struct rtls_position pos;
	float worst = 0.0f;

	rng_seed(4);
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 130; k++) {
		const bool two = k >= 30 && k < 80;
		struct rtls_range_set set;

		make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, two ? 0x3 : 0xF, 0.05f,
			 -1);
		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
		if (two) {
			worst = fmaxf(worst, err_2d(&pos, truth));
		}
	}

	rtls_loc_get_stats(&ctx, &st, false);
	zassert_equal(st.resets, 0);
	zassert_true(worst < 0.30f, "pior erro com 2 distancias %f m", (double)worst);
	zassert_true(err_2d(&pos, truth) < 0.10f);
}

/* T4.7 */
ZTEST(loc, test_ekf_nees)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float dt = CYCLE_MS / 1000.0f;
	const int runs = 100;
	const int steps = 100;
	float nees_sum = 0.0f;
	int nees_n = 0;

	rng_seed(5);
	for (int run = 0; run < runs; run++) {
		/* verdade gerada pelo próprio modelo: aceleração branca de sigma_a */
		float x[4] = {4.0f, 3.0f, 0.0f, 0.0f};

		zassert_ok(rtls_loc_init(&ctx, &cfg));
		for (int k = 0; k < steps; k++) {
			const float truth[3] = {x[0], x[1], TAG_Z_M};
			struct rtls_range_set set;
			struct rtls_position pos;
			struct rtls_mat pinv;
			float e[4], s = 0.0f;

			make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, 0xF, 0.05f, -1);
			zassert_ok(rtls_loc_update(&ctx, &set, &pos));

			if (k >= 20) {
				zassert_ok(rtls_mat_inv_spd(&pinv, &ctx.P));
				for (int i = 0; i < 4; i++) {
					e[i] = ctx.x[i] - x[i];
				}
				for (int i = 0; i < 4; i++) {
					for (int j = 0; j < 4; j++) {
						s += e[i] * pinv.a[i][j] * e[j];
					}
				}
				nees_sum += s;
				nees_n++;
			}

			for (int ax = 0; ax < 2; ax++) {
				const float w = rng_gauss(cfg.accel_std_mps2);

				x[ax] += dt * x[2 + ax] + 0.5f * dt * dt * w;
				x[2 + ax] += dt * w;
			}
		}
	}

	zassert_true(nees_sum / nees_n > 3.4f && nees_sum / nees_n < 4.8f, "NEES medio %f",
		     (double)(nees_sum / nees_n));
	TC_PRINT("T4.7 NEES medio %.2f (4 estados)\n", (double)(nees_sum / nees_n));
}

/* T4.12 */
ZTEST(loc, test_ekf_reinit_after_timeout)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	const float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	struct rtls_range_set set;
	struct rtls_position pos;
	struct rtls_loc_stats st;
	int64_t t = 0;

	rng_seed(6);
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 20; k++, t += CYCLE_MS) {
		make_set(&set, anchors_2d, truth, t, 0xF, 0.05f, -1);
		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
	}
	zassert_false(pos.flags & RTLS_POS_FLAG_INIT);

	/* 1,5 s sem rodadas */
	make_set(&set, anchors_2d, truth, t + 1500, 0xF, 0.05f, -1);
	zassert_ok(rtls_loc_update(&ctx, &set, &pos));
	rtls_loc_get_stats(&ctx, &st, true);
	zassert_true(pos.flags & RTLS_POS_FLAG_INIT);
	zassert_equal(st.resets, 1);

	/* sem filtro e com poucas distâncias: ainda não há posição */
	rtls_loc_reset(&ctx);
	make_set(&set, anchors_2d, truth, t + 1600, 0x3, 0.05f, -1);
	zassert_equal(rtls_loc_update(&ctx, &set, &pos), -EAGAIN);
}

/* T4.13 */
ZTEST(loc, test_ekf_reinit_after_jump)
{
	const struct rtls_loc_cfg cfg = cfg_2d(0.05f);
	float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	struct rtls_range_set set;
	struct rtls_position pos;
	struct rtls_loc_stats st;
	int init_round = -1;

	rng_seed(7);
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 60; k++) {
		if (k == 30) {
			/*
			 * Salto de 2,8 m: duas âncoras continuam coerentes com a
			 * solução espelhada perto da posição antiga; só o critério de
			 * "metade ou mais rejeitadas" tira o filtro dali.
			 */
			truth[0] = 5.0f;
			truth[1] = 4.0f;
		}
		make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, 0xF, 0.05f, -1);
		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
		if (k > 0 && (pos.flags & RTLS_POS_FLAG_INIT) && init_round < 0) {
			init_round = k;
		}
	}

	rtls_loc_get_stats(&ctx, &st, false);
	zassert_equal(st.resets, 1);
	zassert_true(init_round > 30 && init_round <= 37, "reiniciou na rodada %d", init_round);
	zassert_true(err_2d(&pos, truth) < 0.10f, "erro final %f m",
		     (double)err_2d(&pos, truth));
}

/* ------------------------------------------------------- NLOS (M8) */

static struct rtls_loc_cfg cfg_nlos(bool weighting)
{
	struct rtls_loc_cfg cfg = cfg_2d(0.05f);

	cfg.nlos_weighting = weighting;
	cfg.nlos_los_db = 6.0f;
	cfg.nlos_nlos_db = 10.0f;
	cfg.nlos_var_gain = 9.0f;
	return cfg;
}

/* T8.6: rampa do fator da variância pelo indicador P_RX - P_FP */
ZTEST(loc, test_nlos_var_factor)
{
	struct rtls_loc_cfg cfg = cfg_nlos(true);
	struct rtls_range r = {.rx_power_cdbm = -7900};

	r.resp_fp_power_cdbm = -7900 - 300; /* 3 dB: LOS */
	zassert_within(rtls_loc_nlos_var_factor(&cfg, &r), 1.0f, 1e-6f);
	r.resp_fp_power_cdbm = -7900 - 800; /* 8 dB: meio da rampa */
	zassert_within(rtls_loc_nlos_var_factor(&cfg, &r), 5.0f, 1e-4f);
	r.resp_fp_power_cdbm = -7900 - 1200; /* 12 dB: NLOS */
	zassert_within(rtls_loc_nlos_var_factor(&cfg, &r), 9.0f, 1e-6f);

	/* sem as potências, ou com a ponderação desligada: fator 1 */
	cfg.nlos_weighting = false;
	zassert_within(rtls_loc_nlos_var_factor(&cfg, &r), 1.0f, 1e-6f);
	cfg.nlos_weighting = true;
	r.rx_power_cdbm = 0;
	r.resp_fp_power_cdbm = 0;
	zassert_within(rtls_loc_nlos_var_factor(&cfg, &r), 1.0f, 1e-6f);

	/* limiares invertidos são configuração inválida */
	cfg.nlos_nlos_db = 5.0f;
	zassert_equal(rtls_loc_init(&ctx, &cfg), -EINVAL);
}

/*
 * RMSE com +12 cm de viés numa âncora sinalizada (indicador 12 dB), abaixo
 * do que o gating rejeita: só a ponderação reduz o peso dela.
 */
static float run_nlos_bias(bool weighting, struct rtls_loc_stats *st)
{
	const struct rtls_loc_cfg cfg = cfg_nlos(weighting);
	const float truth[3] = {3.0f, 2.0f, TAG_Z_M};
	float sum = 0.0f;
	int n = 0;

	rng_seed(8);
	zassert_ok(rtls_loc_init(&ctx, &cfg));
	for (int k = 0; k < 300; k++) {
		struct rtls_range_set set;
		struct rtls_position pos;
		float e;

		make_set(&set, anchors_2d, truth, (int64_t)k * CYCLE_MS, 0xF, 0.05f, -1);
		for (int i = 0; i < set.count; i++) {
			set.ranges[i].resp_fp_power_cdbm = set.ranges[i].rx_power_cdbm - 200;
		}
		set.ranges[2].distance_m += 0.12f;
		set.ranges[2].resp_fp_power_cdbm = set.ranges[2].rx_power_cdbm - 1200;

		zassert_ok(rtls_loc_update(&ctx, &set, &pos));
		e = err_2d(&pos, truth);
		if (k >= 20) {
			sum += e * e;
			n++;
		}
	}

	rtls_loc_get_stats(&ctx, st, false);
	return sqrtf(sum / n);
}

/* T8.6 */
ZTEST(loc, test_nlos_weighting_reduces_bias)
{
	struct rtls_loc_stats st_off, st_on;
	const float off = run_nlos_bias(false, &st_off);
	const float on = run_nlos_bias(true, &st_on);

	zassert_equal(st_off.nlos_weighted, 0);
	zassert_true(st_on.nlos_weighted >= 290, "ponderadas: %u", st_on.nlos_weighted);
	zassert_true(on < 0.8f * off, "RMSE %f com, %f sem", (double)on, (double)off);
	TC_PRINT("T8.6 vies NLOS de 12 cm numa ancora: RMSE %.1f mm sem, %.1f mm com ponderacao\n",
		 (double)(off * 1000.0f), (double)(on * 1000.0f));
}

ZTEST_SUITE(loc, NULL, NULL, NULL, NULL, NULL);
