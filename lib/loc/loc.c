/*
 * Multilateração LS + Gauss-Newton e EKF de velocidade constante
 * (docs/04-algoritmos.md, seções 4 e 5; tcc/07-localizacao.md).
 *
 * Estado: 2D [px, py, vx, vy] (z fixo = tag_height_m); 3D [px, py, pz, vx, vy, vz].
 * Índices: posição em [0, dim), velocidade em [dim, 2 dim).
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <rtls/frames.h>
#include <rtls/loc.h>
#include <rtls/loc_mat.h>

/* Iterações de Gauss-Newton depois da solução linear. */
#define LOC_GN_ITERS 3
/* Passo de Gauss-Newton abaixo do qual a iteração para (m). */
#define LOC_GN_STOP_M 1e-4f
/* Distância prevista mínima para linearizar (m): evita dividir por ~0. */
#define LOC_MIN_RANGE_M 1e-3f
/* Reinicializa após este tempo sem nenhuma distância aceita (ms). */
#define LOC_TIMEOUT_MS 1000
/*
 * Rodadas seguidas com metade ou mais das distâncias rejeitadas a partir das
 * quais se testa a reinicialização pela multilateração da rodada.
 */
#define LOC_MAX_REJECT_STREAK 5
/* Reinicializa se a soma das variâncias de posição passar disto (m^2). */
#define LOC_MAX_POS_VAR_M2 100.0f
/* Desvio padrão inicial de cada componente da velocidade (m/s). */
#define LOC_INIT_VEL_STD_MPS 1.0f

void rtls_loc_default_cfg(struct rtls_loc_cfg *cfg)
{
	cfg->dim = IS_ENABLED(CONFIG_RTLS_LOC_MODE_3D) ? RTLS_LOC_3D : RTLS_LOC_2D;
	cfg->tag_height_m = CONFIG_RTLS_LOC_TAG_HEIGHT_MM / 1000.0f;
	cfg->range_std_m = CONFIG_RTLS_LOC_RANGE_STD_MM / 1000.0f;
	cfg->accel_std_mps2 = CONFIG_RTLS_LOC_ACCEL_STD_MMS2 / 1000.0f;
	cfg->gate_chi2 = CONFIG_RTLS_LOC_GATE_CHI2_X100 / 100.0f;
}

int rtls_loc_init(struct rtls_loc_ctx *ctx, const struct rtls_loc_cfg *cfg)
{
	if (ctx == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if ((cfg->dim != RTLS_LOC_2D && cfg->dim != RTLS_LOC_3D) || !(cfg->range_std_m > 0.0f) ||
	    !(cfg->accel_std_mps2 > 0.0f) || !(cfg->gate_chi2 > 0.0f)) {
		return -EINVAL;
	}

	ctx->cfg = *cfg;
	ctx->n_states = 2 * cfg->dim;
	memset(&ctx->stats, 0, sizeof(ctx->stats));
	rtls_loc_reset(ctx);
	return 0;
}

void rtls_loc_reset(struct rtls_loc_ctx *ctx)
{
	ctx->initialized = false;
	ctx->last_t_ms = 0;
	ctx->last_accept_ms = 0;
	ctx->reject_streak = 0;
	memset(ctx->x, 0, sizeof(ctx->x));
	rtls_mat_zero(&ctx->P, ctx->n_states, ctx->n_states);
}

void rtls_loc_get_stats(struct rtls_loc_ctx *ctx, struct rtls_loc_stats *stats, bool reset)
{
	*stats = ctx->stats;
	if (reset) {
		memset(&ctx->stats, 0, sizeof(ctx->stats));
	}
}

static void anchor_m(const struct rtls_range *r, float a[3])
{
	for (int i = 0; i < 3; i++) {
		a[i] = (float)r->anchor_pos_mm[i] / 1000.0f;
	}
}

/*
 * Vetor tag - âncora com a posição p. Em 2D, a componente z usa a altura fixa
 * da tag; p[2] é ignorado.
 */
static float range_diff(const struct rtls_loc_cfg *cfg, const float *p, const float a[3],
			float diff[3])
{
	diff[0] = p[0] - a[0];
	diff[1] = p[1] - a[1];
	diff[2] = ((cfg->dim == RTLS_LOC_3D) ? p[2] : cfg->tag_height_m) - a[2];

	return sqrtf(diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2]);
}

/*
 * Solução linear relativa à âncora 0. Com q = p - a0:
 *   2 (a_i - a0)^T q = d0^2 - di^2 + |a_i - a0|^2,  i = 1..N-1
 * Em 2D usa as distâncias horizontais d'^2 = d^2 - (z_t - z_i)^2.
 */
static int solve_linear(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set, int n,
			float pos[3])
{
	const int dim = cfg->dim;
	struct rtls_mat ata, l;
	float atb[3] = {0.0f};
	float a0[3], ai[3], q[3];
	float d0sq;

	anchor_m(&set->ranges[0], a0);
	d0sq = set->ranges[0].distance_m * set->ranges[0].distance_m;
	if (dim == RTLS_LOC_2D) {
		const float dz = cfg->tag_height_m - a0[2];

		d0sq = fmaxf(d0sq - dz * dz, 0.0f);
	}

	rtls_mat_zero(&ata, dim, dim);
	for (int i = 1; i < n; i++) {
		float g[3], b, disq, gsq = 0.0f;

		anchor_m(&set->ranges[i], ai);
		disq = set->ranges[i].distance_m * set->ranges[i].distance_m;
		if (dim == RTLS_LOC_2D) {
			const float dz = cfg->tag_height_m - ai[2];

			disq = fmaxf(disq - dz * dz, 0.0f);
		}

		for (int j = 0; j < dim; j++) {
			g[j] = ai[j] - a0[j];
			gsq += g[j] * g[j];
		}
		b = d0sq - disq + gsq;

		for (int j = 0; j < dim; j++) {
			for (int k = 0; k < dim; k++) {
				ata.a[j][k] += 4.0f * g[j] * g[k];
			}
			atb[j] += 2.0f * g[j] * b;
		}
	}

	if (rtls_mat_cholesky(&l, &ata) != 0) {
		return -EINVAL;
	}
	rtls_mat_chol_solve(&l, atb, q);

	for (int j = 0; j < dim; j++) {
		pos[j] = a0[j] + q[j];
	}
	if (dim == RTLS_LOC_2D) {
		pos[2] = cfg->tag_height_m;
	}

	return 0;
}

/*
 * LS + Gauss-Newton. Na saída, @p cov = (J^T J)^-1 na solução e @p s2 =
 * soma dos resíduos^2 / (N - dim), a variância a posteriori da distância.
 */
static int solve(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set, float pos[3],
		 struct rtls_mat *cov, float *s2)
{
	const int dim = cfg->dim;
	const int n = MIN(set->count, RTLS_MAX_ANCHORS_PER_ROUND);
	struct rtls_mat jtj, l;
	bool converged = false;

	if (n < dim + 1) {
		return -EINVAL;
	}
	if (solve_linear(cfg, set, n, pos) != 0) {
		return -EINVAL;
	}

	for (int it = 0;; it++) {
		float jtr[3] = {0.0f};
		float delta[3];
		float rss = 0.0f;
		float step_sq = 0.0f;
		int used = 0;

		rtls_mat_zero(&jtj, dim, dim);
		for (int i = 0; i < n; i++) {
			float a[3], diff[3], u[3], pred, res;

			anchor_m(&set->ranges[i], a);
			pred = range_diff(cfg, pos, a, diff);
			if (pred < LOC_MIN_RANGE_M) {
				continue;
			}

			res = set->ranges[i].distance_m - pred;
			for (int j = 0; j < dim; j++) {
				u[j] = diff[j] / pred;
			}
			for (int j = 0; j < dim; j++) {
				for (int k = 0; k < dim; k++) {
					jtj.a[j][k] += u[j] * u[k];
				}
				jtr[j] += u[j] * res;
			}
			rss += res * res;
			used++;
		}

		if (used < dim + 1 || rtls_mat_cholesky(&l, &jtj) != 0) {
			return -EINVAL;
		}
		if (it == LOC_GN_ITERS || converged) {
			*s2 = rss / (float)(used - dim);
			break;
		}

		rtls_mat_chol_solve(&l, jtr, delta);
		for (int j = 0; j < dim; j++) {
			pos[j] += delta[j];
			step_sq += delta[j] * delta[j];
		}
		converged = step_sq < LOC_GN_STOP_M * LOC_GN_STOP_M;
	}

	return rtls_mat_inv_spd(cov, &jtj) == 0 ? 0 : -EINVAL;
}

int rtls_loc_multilaterate(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set,
			   float pos_m[3], float *gdop)
{
	struct rtls_mat cov;
	float pos[3];
	float s2;
	int err;

	if (cfg == NULL || set == NULL || pos_m == NULL) {
		return -EINVAL;
	}

	err = solve(cfg, set, pos, &cov, &s2);
	if (err) {
		return err;
	}

	memcpy(pos_m, pos, sizeof(pos));
	if (gdop != NULL) {
		*gdop = sqrtf(rtls_mat_trace(&cov));
	}
	return 0;
}

/*
 * Inicializa o filtro pela multilateração: p = LS, v = 0,
 * P_pp = max(sigma_r^2, s2) (J^T J)^-1, P_vv = (1 m/s)^2 I.
 * Usar s2 infla P0 quando as distâncias da rodada são incoerentes entre si.
 */
static int init_from_set(struct rtls_loc_ctx *ctx, const struct rtls_range_set *set)
{
	const int dim = ctx->cfg.dim;
	const float r2 = ctx->cfg.range_std_m * ctx->cfg.range_std_m;
	struct rtls_mat cov;
	float pos[3];
	float s2, var0;

	if (solve(&ctx->cfg, set, pos, &cov, &s2) != 0) {
		return -EAGAIN;
	}

	var0 = fmaxf(r2, s2);
	memset(ctx->x, 0, sizeof(ctx->x));
	rtls_mat_zero(&ctx->P, ctx->n_states, ctx->n_states);
	for (int i = 0; i < dim; i++) {
		ctx->x[i] = pos[i];
		for (int j = 0; j < dim; j++) {
			ctx->P.a[i][j] = var0 * cov.a[i][j];
		}
		ctx->P.a[dim + i][dim + i] = LOC_INIT_VEL_STD_MPS * LOC_INIT_VEL_STD_MPS;
	}

	ctx->initialized = true;
	ctx->last_t_ms = set->t_ms;
	ctx->last_accept_ms = set->t_ms;
	ctx->reject_streak = 0;
	return 0;
}

/*
 * Predição de velocidade constante: x = F x, P = F P F^T + Q, com o modelo de
 * aceleração branca discreta (DWNA) por eixo:
 *   Q = sigma_a^2 [dt^4/4  dt^3/2; dt^3/2  dt^2]
 */
static void predict(struct rtls_loc_ctx *ctx, float dt)
{
	const int dim = ctx->cfg.dim;
	const float qa = ctx->cfg.accel_std_mps2 * ctx->cfg.accel_std_mps2;
	const float dt2 = dt * dt;
	struct rtls_mat f, fp;

	if (!(dt > 0.0f)) {
		return;
	}

	for (int i = 0; i < dim; i++) {
		ctx->x[i] += dt * ctx->x[dim + i];
	}

	rtls_mat_identity(&f, ctx->n_states);
	for (int i = 0; i < dim; i++) {
		f.a[i][dim + i] = dt;
	}
	(void)rtls_mat_mul(&fp, &f, &ctx->P);
	(void)rtls_mat_mul_bt(&ctx->P, &fp, &f);

	for (int i = 0; i < dim; i++) {
		ctx->P.a[i][i] += qa * dt2 * dt2 / 4.0f;
		ctx->P.a[i][dim + i] += qa * dt2 * dt / 2.0f;
		ctx->P.a[dim + i][i] += qa * dt2 * dt / 2.0f;
		ctx->P.a[dim + i][dim + i] += qa * dt2;
	}
}

/*
 * Atualização por uma distância (escalar): nu = z - h(x), S = H P H^T + R,
 * gating nu^2/S <= gamma, K = P H^T / S e P na forma de Joseph.
 * Retorna true se a distância foi aceita.
 */
static bool update_range(struct rtls_loc_ctx *ctx, const struct rtls_range *r)
{
	const int dim = ctx->cfg.dim;
	const int n = ctx->n_states;
	const float r2 = ctx->cfg.range_std_m * ctx->cfg.range_std_m;
	struct rtls_mat ikh, tmp;
	float a[3], diff[3], h[3];
	float ph[RTLS_LOC_MAX_STATES];
	float k[RTLS_LOC_MAX_STATES];
	float pred, nu, s;

	anchor_m(r, a);
	pred = range_diff(&ctx->cfg, ctx->x, a, diff);
	if (pred < LOC_MIN_RANGE_M) {
		return false;
	}
	for (int j = 0; j < dim; j++) {
		h[j] = diff[j] / pred;
	}

	/* ph = P H^T (H só tem as colunas de posição) */
	s = r2;
	for (int i = 0; i < n; i++) {
		ph[i] = 0.0f;
		for (int j = 0; j < dim; j++) {
			ph[i] += ctx->P.a[i][j] * h[j];
		}
	}
	for (int j = 0; j < dim; j++) {
		s += h[j] * ph[j];
	}

	nu = r->distance_m - pred;
	/* "!(a <= b)" rejeita também NaN */
	if (!(nu * nu <= ctx->cfg.gate_chi2 * s)) {
		return false;
	}

	for (int i = 0; i < n; i++) {
		k[i] = ph[i] / s;
		ctx->x[i] += k[i] * nu;
	}

	/* Joseph: P = (I - K H) P (I - K H)^T + K R K^T */
	rtls_mat_identity(&ikh, n);
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < dim; j++) {
			ikh.a[i][j] -= k[i] * h[j];
		}
	}
	(void)rtls_mat_mul(&tmp, &ikh, &ctx->P);
	(void)rtls_mat_mul_bt(&ctx->P, &tmp, &ikh);
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			ctx->P.a[i][j] += r2 * k[i] * k[j];
		}
	}

	return true;
}

/* Índices das distâncias da mais forte para a mais fraca (rx_power_cdbm). */
static void order_by_power(const struct rtls_range_set *set, int n, uint8_t *order)
{
	for (int i = 0; i < n; i++) {
		int j = i;

		while (j > 0 &&
		       set->ranges[order[j - 1]].rx_power_cdbm < set->ranges[i].rx_power_cdbm) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = (uint8_t)i;
	}
}

/* Filtro perdido: variância de posição grande demais ou estado não finito. */
static bool state_ok(const struct rtls_loc_ctx *ctx)
{
	float pos_var = 0.0f;

	for (int i = 0; i < ctx->cfg.dim; i++) {
		pos_var += ctx->P.a[i][i];
	}
	if (!(pos_var <= LOC_MAX_POS_VAR_M2)) {
		return false;
	}
	for (int i = 0; i < ctx->n_states; i++) {
		if (!isfinite(ctx->x[i])) {
			return false;
		}
	}

	return true;
}

/*
 * As distâncias da rodada concordam entre si? Critério: variância a
 * posteriori da multilateração s2 <= gamma sigma_r^2. Evita reinicializar
 * com uma rodada que tem, ela mesma, distâncias ruins.
 */
static bool set_consistent(const struct rtls_loc_ctx *ctx, const struct rtls_range_set *set)
{
	const float r2 = ctx->cfg.range_std_m * ctx->cfg.range_std_m;
	struct rtls_mat cov;
	float pos[3];
	float s2;

	if (solve(&ctx->cfg, set, pos, &cov, &s2) != 0) {
		return false;
	}

	return s2 <= ctx->cfg.gate_chi2 * r2;
}

static void restart(struct rtls_loc_ctx *ctx)
{
	rtls_loc_reset(ctx);
	ctx->stats.resets++;
}

static void fill_output(const struct rtls_loc_ctx *ctx, int64_t t_ms, uint8_t n_ranges,
			uint8_t flags, struct rtls_position *out)
{
	const int dim = ctx->cfg.dim;

	memset(out, 0, sizeof(*out));
	out->t_ms = t_ms;
	for (int i = 0; i < dim; i++) {
		out->pos_m[i] = ctx->x[i];
		out->vel_mps[i] = ctx->x[dim + i];
		out->std_m[i] = sqrtf(fmaxf(ctx->P.a[i][i], 0.0f));
	}
	if (dim == RTLS_LOC_2D) {
		out->pos_m[2] = ctx->cfg.tag_height_m;
	}

	out->n_ranges = n_ranges;
	out->flags = flags | RTLS_POS_FLAG_VALID | ((dim == RTLS_LOC_3D) ? RTLS_POS_FLAG_3D : 0);
}

int rtls_loc_update(struct rtls_loc_ctx *ctx, const struct rtls_range_set *set,
		    struct rtls_position *out)
{
	uint8_t accepted = 0;
	uint8_t rejected = 0;
	uint8_t flags = 0;
	int n;

	if (ctx == NULL || set == NULL || out == NULL) {
		return -EINVAL;
	}
	n = MIN(set->count, RTLS_MAX_ANCHORS_PER_ROUND);

	if (ctx->initialized && set->t_ms - ctx->last_accept_ms > LOC_TIMEOUT_MS) {
		restart(ctx);
	}

	if (ctx->initialized) {
		uint8_t order[RTLS_MAX_ANCHORS_PER_ROUND];

		predict(ctx, (float)(set->t_ms - ctx->last_t_ms) / 1000.0f);
		ctx->last_t_ms = set->t_ms;

		order_by_power(set, n, order);
		for (int i = 0; i < n; i++) {
			if (update_range(ctx, &set->ranges[order[i]])) {
				accepted++;
			} else {
				rejected++;
			}
		}
		rtls_mat_symmetrize(&ctx->P);

		ctx->stats.accepted += accepted;
		ctx->stats.rejected += rejected;
		if (rejected > 0) {
			flags |= RTLS_POS_FLAG_OUTLIER;
		}
		if (accepted > 0) {
			ctx->last_accept_ms = set->t_ms;
		}
		/*
		 * Rodada suspeita: metade ou mais rejeitadas. Pega também o filtro
		 * preso numa solução espelhada, coerente só com parte das âncoras.
		 */
		if (n > 0) {
			ctx->reject_streak = (2 * rejected >= n) ? ctx->reject_streak + 1 : 0;
		}

		/* Divergiu: recomeça já com as distâncias desta rodada. */
		if (!state_ok(ctx)) {
			restart(ctx);
		} else if (ctx->reject_streak > LOC_MAX_REJECT_STREAK && set_consistent(ctx, set)) {
			restart(ctx);
		}
	}

	if (!ctx->initialized) {
		if (init_from_set(ctx, set) != 0) {
			return -EAGAIN;
		}
		accepted = (uint8_t)n;
		ctx->stats.accepted += accepted;
		flags = RTLS_POS_FLAG_INIT;
	}

	fill_output(ctx, set->t_ms, accepted, flags, out);
	ctx->stats.updates++;
	return 0;
}
