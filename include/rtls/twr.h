/*
 * Motor DS-TWR: initiator (tag) e responder (âncora).
 * Sequência e temporização: docs/03-protocolo-uwb.md, seções 3 e 4.
 *
 * Tag e âncoras precisam usar os mesmos CONFIG_RTLS_TWR_REPLY_DELAY_US e
 * CONFIG_RTLS_TWR_SLOT_US.
 */

#ifndef RTLS_TWR_H_
#define RTLS_TWR_H_

#include <stdbool.h>
#include <stdint.h>

#include <rtls/frames.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Distância medida até uma âncora numa rodada. */
struct rtls_range {
	uint16_t anchor_id;
	int32_t anchor_pos_mm[3]; /**< posição informada pela âncora no REPORT */
	float distance_m;         /**< distância final (corrigida, se habilitado) */
	float distance_raw_m;     /**< DS-TWR antes da correção de viés por RSL */
	int16_t rx_power_cdbm;    /**< potência do RESP recebido na tag */
	int16_t fp_power_cdbm;    /**< potência do 1º caminho do FINAL na âncora */
};

/** Resultado de uma rodada: só as âncoras que completaram a troca. */
struct rtls_range_set {
	uint8_t round;
	uint8_t count;
	int64_t t_ms; /**< uptime da tag no instante do POLL */
	struct rtls_range ranges[RTLS_MAX_ANCHORS_PER_ROUND];
};

struct rtls_twr_initiator_cfg {
	uint16_t pan_id;
	uint16_t tag_id;
	uint8_t n_anchors;
	uint16_t anchors[RTLS_MAX_ANCHORS_PER_ROUND]; /**< ordem = slot de resposta */
};

struct rtls_twr_responder_cfg {
	uint16_t pan_id;
	uint16_t anchor_id;
	int32_t pos_mm[3];
};

/** Contadores por âncora, na ordem de rtls_twr_initiator_cfg.anchors. */
struct rtls_twr_anchor_stats {
	uint32_t resp_ok;  /**< RESP recebido */
	uint32_t range_ok; /**< REPORT recebido e distância válida */
	uint32_t invalid;  /**< distância fora da faixa plausível */
};

struct rtls_twr_initiator_stats {
	uint32_t rounds;
	uint32_t rounds_complete; /**< todas as âncoras com distância */
	uint32_t late_tx;         /**< FINAL programado tarde demais */
	uint32_t last_round_us;
	uint32_t max_round_us;
	struct rtls_twr_anchor_stats anchors[RTLS_MAX_ANCHORS_PER_ROUND];
};

struct rtls_twr_responder_stats {
	uint32_t polls;         /**< POLLs que citam esta âncora */
	uint32_t resp_sent;
	uint32_t finals;
	uint32_t final_timeout; /**< RESP enviado, FINAL não chegou */
	uint32_t reports_sent;
	uint32_t late_tx;       /**< RESP/REPORT programado tarde demais */
};

int rtls_twr_initiator_init(const struct rtls_twr_initiator_cfg *cfg);

/**
 * Executa uma rodada completa (POLL, RESPs, FINAL, REPORTs). Bloqueia por
 * ~10 ms com 4 âncoras e deixa o rádio ocioso ao terminar.
 *
 * @retval 0 rodada concluída (out->count pode ser < n_anchors)
 * @retval -ENODATA nenhuma distância válida
 * @retval -ETIME FINAL programado tarde demais
 */
int rtls_twr_initiator_run_round(struct rtls_range_set *out);

/** Copia os contadores do initiator e, se @p reset, zera-os. */
void rtls_twr_initiator_get_stats(struct rtls_twr_initiator_stats *stats, bool reset);

int rtls_twr_responder_init(const struct rtls_twr_responder_cfg *cfg);

/** Laço do responder; só retorna em erro irrecuperável. */
int rtls_twr_responder_run(void);

/** Copia os contadores do responder e, se @p reset, zera-os. */
void rtls_twr_responder_get_stats(struct rtls_twr_responder_stats *stats, bool reset);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_TWR_H_ */
