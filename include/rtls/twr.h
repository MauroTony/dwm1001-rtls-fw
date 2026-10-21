/*
 * Motor DS-TWR: initiator (tag) e responder (âncora).
 * Sequência e temporização: docs/03-protocolo-uwb.md, seções 3 e 4.
 */

#ifndef RTLS_TWR_H_
#define RTLS_TWR_H_

#include <stdint.h>

#include <rtls/frames.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Distância medida até uma âncora numa rodada. */
struct rtls_range {
	uint16_t anchor_id;
	int32_t anchor_pos_mm[3]; /**< posição informada pela âncora no REPORT */
	float distance_m;
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

int rtls_twr_initiator_init(const struct rtls_twr_initiator_cfg *cfg);

/**
 * Executa uma rodada completa (POLL, RESPs, FINAL, REPORTs). Bloqueia por
 * ~10 ms com 4 âncoras.
 *
 * @retval 0 rodada concluída (out->count pode ser < n_anchors)
 * @retval -ENODATA nenhuma âncora respondeu
 */
int rtls_twr_initiator_run_round(struct rtls_range_set *out);

int rtls_twr_responder_init(const struct rtls_twr_responder_cfg *cfg);

/** Laço do responder; só retorna em erro irrecuperável. */
int rtls_twr_responder_run(void);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_TWR_H_ */
