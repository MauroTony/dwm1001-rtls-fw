/*
 * Tabela das tags vistas pelo gateway. Acessada só pela thread main.
 */

#ifndef TAG_TABLE_H_
#define TAG_TABLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rtls/frames.h>
#include <rtls/host_if.h>

/** Contadores de uma tag desde o último tag_table_get_stats(..., true). */
struct tag_stats {
	uint16_t tag_id;
	uint32_t received; /**< POSITION recebidos */
	uint32_t lost;     /**< rodadas sem POSITION, pelas lacunas de `round` */
	int32_t rx_power_sum_cdbm;
};

void tag_table_init(void);

/**
 * Insere ou atualiza a entrada da tag @p tag_id com um POSITION recebido e
 * conta as lacunas de `round` (rtls_round_tracker_update()).
 *
 * @retval 0 entrada atualizada
 * @retval -ENOMEM tabela cheia
 * @retval -EALREADY POSITION repetido (ignorado)
 */
int tag_table_update(uint16_t tag_id, const struct rtls_msg_position *msg,
		     int16_t rx_power_cdbm, int64_t now_ms);

/**
 * Remove as entradas expiradas e copia as ativas para @p out, com age_ms
 * calculado em @p now_ms.
 *
 * @return quantidade de registros copiados
 */
size_t tag_table_snapshot(struct rtls_tag_record *out, size_t max, int64_t now_ms);

/**
 * Copia os contadores das tags ativas e, se @p reset, zera-os.
 *
 * @return quantidade de tags copiadas
 */
size_t tag_table_get_stats(struct tag_stats *out, size_t max, bool reset);

#endif /* TAG_TABLE_H_ */
