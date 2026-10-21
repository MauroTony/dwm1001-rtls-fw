#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <rtls/loc.h>

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

	ctx->cfg = *cfg;
	ctx->n_states = (cfg->dim == RTLS_LOC_3D) ? 6 : 4;
	rtls_loc_reset(ctx);
	return 0;
}

void rtls_loc_reset(struct rtls_loc_ctx *ctx)
{
	ctx->initialized = false;
	ctx->last_t_ms = 0;
	ctx->reject_streak = 0;
	memset(ctx->x, 0, sizeof(ctx->x));
	memset(ctx->P, 0, sizeof(ctx->P));
}

int rtls_loc_multilaterate(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set,
			   float pos_m[3])
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(set);
	ARG_UNUSED(pos_m);

	/*
	 * TODO(M4), docs/04-algoritmos.md seção 4:
	 *  1. 2D: distâncias horizontais d_i' = sqrt(d_i^2 - (z_t - z_i)^2).
	 *  2. Sistema linear A p = b relativo à âncora 0; resolver por
	 *     equações normais (2x2 ou 3x3).
	 *  3. 2-3 iterações de Gauss-Newton; rejeitar se (J^T J) mal condicionada.
	 */
	return -ENOSYS;
}

int rtls_loc_update(struct rtls_loc_ctx *ctx, const struct rtls_range_set *set,
		    struct rtls_position *out)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(set);
	ARG_UNUSED(out);

	/*
	 * TODO(M4), docs/04-algoritmos.md seção 5:
	 *  1. Não inicializado: rtls_loc_multilaterate() -> x = [p; 0], P0.
	 *  2. Predição: dt = (set->t_ms - last_t_ms) / 1000; x = F x; P = F P F^T + Q.
	 *  3. Para cada distância: nu, S = H P H^T + sigma_r^2; gating nu^2/S;
	 *     K = P H^T / S; x += K nu; P na forma de Joseph.
	 *  4. Simetrizar P; preencher out (std = sqrt(diag P), flags).
	 *  5. Reinicializar conforme a seção 5.4.
	 */
	return -ENOSYS;
}
