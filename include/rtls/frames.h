/*
 * Formato dos quadros UWB do RTLS (docs/03-protocolo-uwb.md, seção 2).
 *
 * Todos os quadros são de dados IEEE 802.15.4 com endereços curtos e PAN ID
 * comprimido. As estruturas abaixo são o formato no ar: empacotadas e
 * little-endian (todos os alvos suportados são little-endian).
 */

#ifndef RTLS_FRAMES_H_
#define RTLS_FRAMES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "protocolo RTLS assume little-endian");

/** Frame Control: dados, PAN ID comprimido, destino e origem curtos. */
#define RTLS_FC_DATA_SHORT 0x8841U

#define RTLS_FCS_LEN       2U
#define RTLS_FRAME_MAX_LEN 127U

/** Endereçamento (docs/02-arquitetura.md, seção 7). */
#define RTLS_ADDR_GATEWAY     0x0001U
#define RTLS_ADDR_TAG_BASE    0x1000U
#define RTLS_ADDR_ANCHOR_BASE 0xA000U
#define RTLS_ADDR_BROADCAST   0xFFFFU

/** Máximo de âncoras consultadas por rodada. */
#define RTLS_MAX_ANCHORS_PER_ROUND 4

enum rtls_msg_type {
	RTLS_MSG_BEACON = 0x10,
	RTLS_MSG_POLL = 0x20,
	RTLS_MSG_RESP = 0x21,
	RTLS_MSG_FINAL = 0x22,
	RTLS_MSG_REPORT = 0x23,
	RTLS_MSG_POSITION = 0x30,
	RTLS_MSG_PING = 0xF0, /**< diagnóstico de enlace (samples/uwb_pingpong) */
};

/** Flags do quadro POSITION (também usados em rtls_position e no host_if). */
#define RTLS_POS_FLAG_VALID   (1U << 0) /**< posição válida */
#define RTLS_POS_FLAG_INIT    (1U << 1) /**< só multilateração (filtro inicializando) */
#define RTLS_POS_FLAG_OUTLIER (1U << 2) /**< alguma distância rejeitada pelo gating */
#define RTLS_POS_FLAG_3D      (1U << 3) /**< solução 3D (senão 2D com z fixo) */

/** Cabeçalho MAC (9 bytes). O FCS (2 bytes) é gerado pelo DW1000. */
struct rtls_mac_hdr {
	uint16_t fc;
	uint8_t seq;
	uint16_t pan_id;
	uint16_t dst;
	uint16_t src;
} __packed;

/** BEACON: gateway -> broadcast, início do superquadro (M7). */
struct rtls_msg_beacon {
	uint8_t type;
	uint16_t superframe;
	uint8_t n_slots;
	uint16_t slot_us;
} __packed;

/** POLL: tag -> broadcast. A ordem em anchors[] define o slot de cada âncora. */
struct rtls_msg_poll {
	uint8_t type;
	uint8_t round;
	uint8_t n_anchors;
	uint16_t anchors[RTLS_MAX_ANCHORS_PER_ROUND];
} __packed;

/** RESP: âncora -> tag. */
struct rtls_msg_resp {
	uint8_t type;
	uint8_t round;
} __packed;

/** FINAL: tag -> broadcast. */
struct rtls_msg_final {
	uint8_t type;
	uint8_t round;
} __packed;

/** REPORT: âncora -> tag, com os intervalos medidos no relógio da âncora. */
struct rtls_msg_report {
	uint8_t type;
	uint8_t round;
	uint32_t reply_b;      /**< Db = resp_tx - poll_rx (ticks) */
	uint32_t round_b;      /**< Rb = final_rx - resp_tx (ticks) */
	int32_t pos_mm[3];     /**< posição da âncora */
	int16_t fp_power_cdbm; /**< potência do 1º caminho do FINAL (0,01 dBm) */
} __packed;

/** POSITION: tag -> gateway. */
struct rtls_msg_position {
	uint8_t type;
	uint8_t round;
	uint8_t n_ranges;
	uint8_t flags;      /**< RTLS_POS_FLAG_* */
	uint32_t t_ms;      /**< uptime da tag no POLL da rodada */
	int32_t pos_mm[3];
	uint16_t std_mm[3]; /**< desvio padrão por eixo, saturado em 65535 */
} __packed;

/** PING: diagnóstico de enlace; o "pong" devolve o mesmo seq. */
struct rtls_msg_ping {
	uint8_t type;
	uint32_t seq;
} __packed;

BUILD_ASSERT(sizeof(struct rtls_mac_hdr) == 9);
BUILD_ASSERT(sizeof(struct rtls_msg_ping) == 5);
BUILD_ASSERT(sizeof(struct rtls_msg_beacon) == 6);
BUILD_ASSERT(sizeof(struct rtls_msg_poll) == 11);
BUILD_ASSERT(sizeof(struct rtls_msg_resp) == 2);
BUILD_ASSERT(sizeof(struct rtls_msg_final) == 2);
BUILD_ASSERT(sizeof(struct rtls_msg_report) == 24);
BUILD_ASSERT(sizeof(struct rtls_msg_position) == 26);

static inline void rtls_mac_hdr_init(struct rtls_mac_hdr *hdr, uint8_t seq, uint16_t pan_id,
				     uint16_t dst, uint16_t src)
{
	hdr->fc = RTLS_FC_DATA_SHORT;
	hdr->seq = seq;
	hdr->pan_id = pan_id;
	hdr->dst = dst;
	hdr->src = src;
}

/**
 * Acompanha a sequência de `round` dos POSITION de uma tag para contar
 * lacunas. Zere a estrutura para começar (ou recomeçar) a sequência.
 */
struct rtls_round_tracker {
	bool valid;
	uint8_t last_round;
	uint32_t last_t_ms;
};

/**
 * Registra um POSITION com @p round e @p t_ms (uptime da tag).
 *
 * O `round` dá a volta em 256 rodadas (25,6 s a 10 Hz): o chamador deve
 * zerar o tracker depois de um silêncio longo (o gateway faz isso ao expirar
 * a tag). Um @p t_ms menor que o anterior indica que a tag reiniciou: a
 * sequência recomeça sem contar perdas.
 *
 * @return rodadas sem POSITION desde o anterior (0 se consecutivo ou se a
 *         sequência recomeçou)
 * @retval -EALREADY quadro repetido (mesmo @p t_ms)
 */
int rtls_round_tracker_update(struct rtls_round_tracker *trk, uint8_t round, uint32_t t_ms);

/** Tamanho do payload da mensagem @p type, ou 0 se o tipo for desconhecido. */
size_t rtls_msg_size(uint8_t type);

/**
 * Monta um quadro (cabeçalho + payload, sem FCS) em @p buf.
 *
 * @return tamanho do quadro sem FCS, ou -EINVAL / -ENOMEM
 */
int rtls_frame_build(uint8_t *buf, size_t size, const struct rtls_mac_hdr *hdr,
		     const void *payload, size_t payload_len);

/**
 * Valida um quadro recebido (sem FCS) e localiza o payload.
 *
 * Confere Frame Control, PAN ID e o tamanho mínimo do tipo de mensagem.
 *
 * @return tipo da mensagem (enum rtls_msg_type) ou -EBADMSG
 */
int rtls_frame_parse(const uint8_t *frame, size_t len, uint16_t pan_id,
		     struct rtls_mac_hdr *hdr, const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_FRAMES_H_ */
