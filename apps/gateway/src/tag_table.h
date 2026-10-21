/*
 * Tabela das tags vistas pelo gateway. Acessada só pela thread main.
 */

#ifndef TAG_TABLE_H_
#define TAG_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <rtls/frames.h>
#include <rtls/host_if.h>

void tag_table_init(void);

/** Insere ou atualiza a entrada da tag @p tag_id com um POSITION recebido. */
int tag_table_update(uint16_t tag_id, const struct rtls_msg_position *msg, int8_t rssi_dbm,
		     int64_t now_ms);

/**
 * Remove as entradas expiradas e copia as ativas para @p out, com age_ms
 * calculado em @p now_ms.
 *
 * @return quantidade de registros copiados
 */
size_t tag_table_snapshot(struct rtls_tag_record *out, size_t max, int64_t now_ms);

#endif /* TAG_TABLE_H_ */
