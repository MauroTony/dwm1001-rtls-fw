/*
 * Interface SPI escravo gateway -> host (docs/05-interface-spi-gateway.md).
 */

#ifndef RTLS_HOST_IF_H_
#define RTLS_HOST_IF_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Versão do protocolo SPI anunciada em RTLS_INFO_GET. */
#define RTLS_HOST_IF_PROTO_VERSION 1

/** Opcodes de request e tipos de resposta (TLV). */
#define RTLS_HOST_IF_REQ_INFO_GET     0xA0
#define RTLS_HOST_IF_REQ_TAGS_GET     0xA1
#define RTLS_HOST_IF_REQ_DRDY_CFG_SET 0xA2
#define RTLS_HOST_IF_REQ_NOP          0xFF
#define RTLS_HOST_IF_RSP_ENVELOPE     0x40
#define RTLS_HOST_IF_RSP_INFO         0xB0
#define RTLS_HOST_IF_RSP_TAGS         0xB1

/** Registros de tag por página (cabe numa transação de 255 B do SPIS). */
#define RTLS_HOST_IF_RECORDS_PER_PAGE 9

/** Registro de uma tag, no formato do barramento (26 bytes, little-endian). */
struct rtls_tag_record {
	uint16_t tag_id;
	uint8_t round;
	uint8_t n_ranges;
	uint8_t flags;      /**< RTLS_POS_FLAG_* */
	int8_t rssi_dbm;
	uint16_t age_ms;
	int32_t pos_mm[3];
	uint16_t std_mm[3];
} __packed;

BUILD_ASSERT(sizeof(struct rtls_tag_record) == 26);

/** Prepara SPIS e DRDY. */
int rtls_host_if_init(void);

/**
 * Publica um snapshot da tabela (cópia interna). Sobe o DRDY se o
 * conteúdo mudou desde o último snapshot lido pelo host.
 */
int rtls_host_if_publish(const struct rtls_tag_record *records, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_HOST_IF_H_ */
