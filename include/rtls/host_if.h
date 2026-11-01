/*
 * Interface SPI escravo gateway -> host (docs/05-interface-spi-gateway.md).
 *
 * Duas camadas:
 *  - codec (rtls_host_if_handle(), CRC, páginas): puro, testável em native_sim;
 *  - transporte (rtls_host_if_init()/_publish()): thread com o SPIS e o DRDY.
 */

#ifndef RTLS_HOST_IF_H_
#define RTLS_HOST_IF_H_

#include <stdbool.h>
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

/** Status do envelope [0x40][0x01][status]. */
enum rtls_host_if_status {
	RTLS_HOST_IF_ST_OK = 0,
	RTLS_HOST_IF_ST_UNKNOWN = 1, /**< comando desconhecido */
	RTLS_HOST_IF_ST_INVALID = 2, /**< parâmetro inválido */
	RTLS_HOST_IF_ST_BUSY = 3,    /**< ocupado */
};

/** Registros de tag por página (cabe numa transação de 255 B do SPIS). */
#define RTLS_HOST_IF_RECORDS_PER_PAGE 9

/** Maior transação do SPIS do nRF52832 (EasyDMA com MAXCNT de 8 bits). */
#define RTLS_HOST_IF_MAX_XFER 255

/** Tamanho do value do TLV 0xB0 (docs/05, seção 3.1). */
#define RTLS_HOST_IF_INFO_LEN 11

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

/* Página cheia: envelope (3) + TLV 0xB1 (2 + 4 + 9 x 26 + 2) = 245 B. */
BUILD_ASSERT(3 + 2 + 4 + RTLS_HOST_IF_RECORDS_PER_PAGE * sizeof(struct rtls_tag_record) + 2 <=
	     RTLS_HOST_IF_MAX_XFER);

/** Dados fixos do gateway anunciados em RTLS_INFO_GET. */
struct rtls_host_if_info {
	uint8_t fw_version[3]; /**< major, minor, patch */
	uint16_t gateway_id;
	uint8_t capacity;      /**< tags na tabela do gateway */
	uint16_t superframe_ms;
};

/* --------------------------------------------------------------- codec */

/** O que o codec precisa para responder: snapshot atual e dados fixos. */
struct rtls_host_if_view {
	const struct rtls_host_if_info *info;
	const struct rtls_tag_record *records;
	size_t count;
	uint8_t snapshot; /**< sequência do snapshot */
};

/** Efeitos de um request que o transporte aplica. */
struct rtls_host_if_event {
	int8_t drdy_enable; /**< -1 sem mudança; 0 ou 1 (RTLS_DRDY_CFG_SET) */
	bool last_page;     /**< a resposta é a última página do snapshot */
};

/** CRC-16/CCITT-FALSE (polinômio 0x1021, init 0xFFFF): crc16_itu_t(0xFFFF, ...). */
uint16_t rtls_host_if_crc(const uint8_t *data, size_t len);

/** Páginas de um snapshot com @p count registros (no mínimo 1, no máximo 255). */
uint8_t rtls_host_if_n_pages(size_t count);

/**
 * Processa um request recebido (TLV seguido de qualquer preenchimento) e
 * monta a resposta em @p rsp, que precisa de RTLS_HOST_IF_MAX_XFER bytes.
 *
 * @return tamanho da resposta; 0 para NOP (0xFF) ou transação vazia, que
 *         não têm resposta
 */
size_t rtls_host_if_handle(const uint8_t *req, size_t req_len, const struct rtls_host_if_view *view,
			   uint8_t *rsp, struct rtls_host_if_event *ev);

/* ----------------------------------------------------------- transporte */

/** Prepara SPIS e DRDY e inicia a thread host_if. */
int rtls_host_if_init(const struct rtls_host_if_info *info);

/**
 * Publica um snapshot da tabela (cópia interna sob mutex). Se algo além do
 * age_ms mudou, a sequência avança e o DRDY sobe até o host ler a última
 * página.
 *
 * @retval -ENOMEM mais registros que CONFIG_RTLS_HOST_IF_MAX_RECORDS (o
 *         excesso fica de fora)
 */
int rtls_host_if_publish(const struct rtls_tag_record *records, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_HOST_IF_H_ */
