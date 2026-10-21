/*
 * Utilitários internos do motor DS-TWR (envio e recepção de mensagens).
 */

#ifndef TWR_INTERNAL_H_
#define TWR_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>

#define TWR_REPLY_US  CONFIG_RTLS_TWR_REPLY_DELAY_US
#define TWR_SLOT_US   CONFIG_RTLS_TWR_SLOT_US
#define TWR_MARGIN_US CONFIG_RTLS_TWR_RX_MARGIN_US

/*
 * O RX automático (W4R) liga este tempo antes do primeiro slot de resposta
 * para pegar o preâmbulo inteiro (~140 µs a 6,8 Mbps com 128 símbolos).
 */
#define TWR_RX_GUARD_US 300

/** Monta o quadro (cabeçalho MAC + mensagem) e transmite. */
int twr_send(uint16_t pan_id, uint16_t src, uint16_t dst, const void *msg, size_t msg_len,
	     const struct dw1000_tx_opts *opts, uint64_t *tx_ts);

/**
 * Recebe até chegar uma mensagem do tipo @p type (descarta as demais) ou
 * vencer @p deadline. Copia até @p msg_size bytes do payload em @p msg.
 *
 * @retval 0 mensagem recebida
 * @retval -EAGAIN prazo vencido
 */
int twr_recv(uint16_t pan_id, uint8_t type, k_timepoint_t deadline, struct rtls_mac_hdr *hdr,
	     void *msg, size_t msg_size, struct dw1000_rx_info *info);

#endif /* TWR_INTERNAL_H_ */
