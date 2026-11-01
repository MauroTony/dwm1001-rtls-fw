/*
 * Localização na tag: multilateração LS (inicialização/baseline) e EKF de
 * velocidade constante com atualização sequencial por distância.
 * Especificação: docs/04-algoritmos.md, seções 4 e 5.
 */

#ifndef RTLS_LOC_H_
#define RTLS_LOC_H_

#include <stdbool.h>
#include <stdint.h>

#include <rtls/loc_mat.h>
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
	/*
	 * Ponderação por NLOS (M8): a variância da distância cresce com o
	 * indicador P_RX - P_FP do RESP, de 1 (até nlos_los_db) até
	 * nlos_var_gain (a partir de nlos_nlos_db), em rampa linear.
	 */
	bool nlos_weighting;
	float nlos_los_db;
	float nlos_nlos_db;
	float nlos_var_gain;
};

/** Contadores do filtro, desde o último rtls_loc_get_stats(..., true). */
struct rtls_loc_stats {
	uint32_t updates;       /**< chamadas que publicaram posição */
	uint32_t accepted;      /**< distâncias usadas na atualização */
	uint32_t rejected;      /**< distâncias rejeitadas pelo gating */
	uint32_t resets;        /**< reinicializações (tempo, rejeições seguidas, P inválida) */
	uint32_t nlos_weighted; /**< distâncias com variância aumentada por NLOS */
};

/** Estado do filtro (alocação estática pelo chamador). */
struct rtls_loc_ctx {
	struct rtls_loc_cfg cfg;
	bool initialized;
	uint8_t n_states;               /**< 4 (2D) ou 6 (3D) */
	int64_t last_t_ms;              /**< instante do estado x */
	int64_t last_accept_ms;         /**< última distância aceita */
	uint32_t reject_streak;         /**< rodadas seguidas com tudo rejeitado */
	float x[RTLS_LOC_MAX_STATES];   /**< [p; v] */
	struct rtls_mat P;              /**< covariância, n_states x n_states */
	struct rtls_loc_stats stats;
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
 * No modo 2D, z = cfg->tag_height_m e pos_m[2] recebe esse valor.
 *
 * @param gdop se não for NULL, recebe sqrt(tr((J^T J)^-1)) na solução
 *             (em 2D, só x e y)
 * @retval -EINVAL distâncias insuficientes (2D: < 3; 3D: < 4) ou geometria degenerada
 */
int rtls_loc_multilaterate(const struct rtls_loc_cfg *cfg, const struct rtls_range_set *set,
			   float pos_m[3], float *gdop);

/**
 * Um ciclo do EKF: predição com o dt real + atualização por distância.
 * Reinicializa sozinho (docs/04-algoritmos.md, seção 5.4); a posição que sai
 * da multilateração leva RTLS_POS_FLAG_INIT.
 *
 * @retval 0 @p out preenchido
 * @retval -EAGAIN filtro ainda não inicializado e medidas insuficientes
 * @retval -EINVAL ponteiro nulo
 */
int rtls_loc_update(struct rtls_loc_ctx *ctx, const struct rtls_range_set *set,
		    struct rtls_position *out);

/**
 * Fator da variância de uma distância pelo indicador de NLOS
 * (P_RX - P_FP do RESP). 1 se a ponderação estiver desligada ou se a
 * rodada não trouxer as potências.
 */
float rtls_loc_nlos_var_factor(const struct rtls_loc_cfg *cfg, const struct rtls_range *r);

/** Copia os contadores do filtro e, se @p reset, zera-os. */
void rtls_loc_get_stats(struct rtls_loc_ctx *ctx, struct rtls_loc_stats *stats, bool reset);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_LOC_H_ */
