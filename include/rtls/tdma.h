/*
 * Superquadro TDMA (M7, docs/03-protocolo-uwb.md, seção 5.2).
 *
 *   |<----------------------- superquadro ----------------------->|
 *   | BEACON (4 ms) | slot 0 | slot 1 | ... | slot n_slots - 1     |
 *
 * O gateway anuncia n_slots e slot_us no BEACON; a tag com slot k inicia o
 * POLL em beacon + RTLS_TDMA_BEACON_SEG_US + k * slot_us. Lógica pura: o
 * relógio é o do chamador, em µs.
 */

#ifndef RTLS_TDMA_H_
#define RTLS_TDMA_H_

#include <stdbool.h>
#include <stdint.h>

#include <rtls/frames.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Do início do BEACON ao início do slot 0. */
#define RTLS_TDMA_BEACON_SEG_US 4000

/** Beacons perdidos seguidos depois dos quais a tag para de transmitir. */
#define RTLS_TDMA_MAX_MISSED 3

/** Primeiro ID de tag: slot k = tag_id - RTLS_TDMA_FIRST_TAG_ID. */
#define RTLS_TDMA_FIRST_TAG_ID 0x1001

/** Sincronismo de uma tag com o superquadro. Zere para começar sem sincronismo. */
struct rtls_tdma_sync {
	bool synced;
	uint8_t missed;      /**< beacons perdidos seguidos */
	uint8_t n_slots;
	uint16_t slot_us;
	uint16_t superframe; /**< número do último superquadro (recebido ou previsto) */
	int64_t beacon_us;   /**< início do último superquadro (recebido ou previsto) */
};

/** Slot de uma tag pelo ID, ou -EINVAL se o ID for menor que o primeiro. */
int rtls_tdma_slot_of(uint16_t tag_id);

/** Período do superquadro: RTLS_TDMA_BEACON_SEG_US + n_slots * slot_us. */
uint32_t rtls_tdma_period_us(uint8_t n_slots, uint16_t slot_us);

/**
 * BEACON recebido com início em @p rx_us: (re)sincroniza.
 *
 * @retval -EINVAL BEACON sem slots ou com slot de duração zero
 */
int rtls_tdma_on_beacon(struct rtls_tdma_sync *s, const struct rtls_msg_beacon *b, int64_t rx_us);

/**
 * O BEACON esperado não chegou: o superquadro avança pela previsão. Depois
 * de RTLS_TDMA_MAX_MISSED seguidos, o sincronismo se perde.
 */
void rtls_tdma_on_missed(struct rtls_tdma_sync *s);

/**
 * Início do slot @p slot no superquadro atual.
 *
 * @retval -EAGAIN sem sincronismo (a tag não pode transmitir)
 * @retval -ERANGE slot fora do superquadro anunciado
 */
int rtls_tdma_slot_start(const struct rtls_tdma_sync *s, int slot, int64_t *start_us);

/** Instante previsto do próximo BEACON (sem sincronismo, o último conhecido). */
int64_t rtls_tdma_next_beacon(const struct rtls_tdma_sync *s);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_TDMA_H_ */
