/*
 * Matemática do DS-TWR assimétrico (docs/04-algoritmos.md, seção 2).
 * Puro: sem hardware nem kernel, testado em native_sim.
 */

#ifndef RTLS_TWR_MATH_H_
#define RTLS_TWR_MATH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Máscara dos 40 bits do relógio do DW1000. */
#define RTLS_DW_TS_MASK 0xFFFFFFFFFFULL

/** Duração de um tick do DW1000: 1 / (128 * 499,2 MHz) ≈ 15,65 ps. */
#define RTLS_DW_TICK_S (1.0 / (128.0 * 499.2e6))

/** Velocidade de propagação no ar (m/s). */
#define RTLS_SPEED_OF_LIGHT_AIR 299702547.0

/** Intervalos de uma rodada DS-TWR, em ticks do relógio de quem mediu. */
struct rtls_twr_intervals {
	uint64_t round_a; /**< Ra = resp_rx - poll_tx (tag) */
	uint64_t reply_a; /**< Da = final_tx - resp_rx (tag) */
	uint64_t round_b; /**< Rb = final_rx - resp_tx (âncora) */
	uint64_t reply_b; /**< Db = resp_tx - poll_rx (âncora) */
};

/** Intervalo @p end - @p start com wrap de 40 bits. */
static inline uint64_t rtls_dw_ts_diff(uint64_t end, uint64_t start)
{
	return (end - start) & RTLS_DW_TS_MASK;
}

/** µs -> ticks do DW1000 (128 * 499,2 MHz = 63897,6 ticks/µs). */
static inline uint64_t rtls_us_to_dw_ticks(uint32_t us)
{
	return (uint64_t)us * 638976ULL / 10ULL;
}

/** µs -> "UWB microseconds" (1 UUS = 512/499,2 µs), unidade do W4R_TIM. */
static inline uint32_t rtls_us_to_uus(uint32_t us)
{
	return (uint32_t)((uint64_t)us * 4992ULL / 5120ULL);
}

/**
 * Instante (40 bits) do slot @p idx: base + T_reply + idx * T_slot.
 * Usado para RESP/REPORT (âncoras) e FINAL (tag com idx = n_anchors).
 */
static inline uint64_t rtls_twr_slot_ts(uint64_t base, uint32_t reply_us, uint32_t slot_us,
					uint8_t idx)
{
	return (base + rtls_us_to_dw_ticks(reply_us) + (uint64_t)idx * rtls_us_to_dw_ticks(slot_us)) &
	       RTLS_DW_TS_MASK;
}

/** Monta os intervalos a partir dos timestamps da tag e dos valores do REPORT. */
static inline void rtls_twr_tag_intervals(struct rtls_twr_intervals *iv, uint64_t poll_tx,
					  uint64_t resp_rx, uint64_t final_tx, uint32_t reply_b,
					  uint32_t round_b)
{
	iv->round_a = rtls_dw_ts_diff(resp_rx, poll_tx);
	iv->reply_a = rtls_dw_ts_diff(final_tx, resp_rx);
	iv->round_b = round_b;
	iv->reply_b = reply_b;
}

/**
 * Tempo de voo pelo estimador assimétrico:
 *   ToF = (Ra*Rb - Da*Db) / (Ra + Rb + Da + Db)
 *
 * @return ToF em ticks (fracionário); 0 se os intervalos forem inválidos
 */
double rtls_twr_ds_tof(const struct rtls_twr_intervals *iv);

/** Converte ToF (ticks) em distância (m). */
double rtls_twr_tof_to_m(double tof_ticks);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_TWR_MATH_H_ */
