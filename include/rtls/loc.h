/*
 * Localização na tag: multilateração LS (inicialização/baseline) e EKF de
 * velocidade constante com atualização sequencial por distância.
 * Especificação: docs/04-algoritmos.md, seções 4 e 5.
 */

#ifndef RTLS_LOC_H_
#define RTLS_LOC_H_

#include <stdbool.h>
#include <stdint.h>

#include <rtls/twr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLS_LOC_MAX_STATES 6

enum rtls_loc_dim {
	RTLS_LOC_2D = 2,
	RTLS_LOC_3D = 3,
};

struct rtls_loc_cfg {
	enum rtls_loc_dim dim;
	float tag_height_m;   /**< z fixo no modo 2D */
	float range_std_m;    /**< sigma_r */
	float accel_std_mps2; /**< sigma_a */
	float gate_chi2;      /**< limiar do teste nu^2/S */
};

/** Estado do filtro (alocação estática pelo chamador). */
struct rtls_loc_ctx {
	struct rtls_loc_cfg cfg;
	bool initialized;
	uint8_t n_states;   /**< 4 (2D) ou 6 (3D) */
	int64_t last_t_ms;
	uint32_t reject_streak;
	float x[RTLS_LOC_MAX_STATES];                      /**< [p; v] */
	float P[RTLS_LOC_MAX_STATES][RTLS_LOC_MAX_STATES]; /**< covariância */
};

/** Estimativa publicada a cada ciclo. */
struct rtls_position {
	int64_t t_ms;
	float pos_m[3];
	float vel_mps[3];
	float std_m[3];
	uint8_t n_ranges; /**< distâncias aceitas nesta atualização */
	uint8_t flags;    /**< RTLS_POS_FLAG_* (rtls/frames.h) */
};

/** Preenche @p cfg com os valores do Kconfig (CONFIG_RTLS_LOC_*). */
void rtls_loc_default_cfg(struct rtls_loc_cfg *cfg);

int rtls_loc_init(struct rtls_loc_ctx *ctx, const struct rtls_loc_cfg *cfg);

/** Descarta o estado; a próxima atualização reinicializa por multilateração. */
void rtls_loc_reset(struct rtls_loc_ctx *ctx);

/**
 * Multilateração LS + refinamento Gauss-Newton.
 *
 * @retval -EINVAL distâncias insuficientes (2D: < 3; 3D: < 4) ou geometria degenerada
 */
int rtls_loc_multilaterate(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set,
			   float pos_m[3]);

/**
 * Um ciclo do EKF: predição com o dt real + atualização por distância.
 *
 * @retval 0 @p out preenchido
 * @retval -EAGAIN filtro ainda não inicializado e medidas insuficientes
 */
int rtls_loc_update(struct rtls_loc_ctx *ctx, const struct rtls_range_set *set,
		    struct rtls_position *out);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_LOC_H_ */
