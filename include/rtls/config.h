/*
 * Registro de parâmetros do nó (M8). As chaves são as do M12
 * (docs/03-protocolo-uwb.md, seção 8.2): o shell de agora e a configuração
 * remota de depois usam a mesma tabela.
 *
 * Cada parâmetro tem chave estável, nome no estilo do settings
 * ("anchor/pos_x_mm"), valor i32, faixa e classe. O padrão vem do Kconfig;
 * o valor salvo na NVS (subárvore "rtls/") o substitui no boot. A tabela de
 * cada firmware só tem os parâmetros do papel dele.
 *
 * Concorrência: o shell grava (rtls_config_set()) e a thread main aplica
 * (rtls_config_next_change()), porque só ela mexe no rádio e no EKF.
 */

#ifndef RTLS_CONFIG_H_
#define RTLS_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rtls_cfg_key {
	RTLS_CFG_NODE_ID = 0x0101,
	RTLS_CFG_PAN_ID = 0x0102,
	RTLS_CFG_ANCHOR_POS_X = 0x0201,
	RTLS_CFG_ANCHOR_POS_Y = 0x0202,
	RTLS_CFG_ANCHOR_POS_Z = 0x0203,
	RTLS_CFG_TX_ANT_DLY = 0x0301,
	RTLS_CFG_RX_ANT_DLY = 0x0302,
	RTLS_CFG_TAG_ANCHOR_BASE = 0x0501,
	RTLS_CFG_TAG_NUM_ANCHORS = 0x0502,
	RTLS_CFG_LOC_MODE_3D = 0x0601,
	RTLS_CFG_LOC_TAG_HEIGHT = 0x0602,
	RTLS_CFG_LOC_RANGE_STD = 0x0603,
	RTLS_CFG_LOC_ACCEL_STD = 0x0604,
	RTLS_CFG_LOC_GATE_CHI2 = 0x0605,
	RTLS_CFG_LOC_NLOS_WEIGHT = 0x0606,
};

/** Quando um valor novo passa a valer. */
enum rtls_cfg_class {
	RTLS_CFG_IMMEDIATE, /**< a thread main aplica na hora */
	RTLS_CFG_REBOOT,    /**< salvo; vale depois de reiniciar */
	RTLS_CFG_COORD,     /**< igual em todos os nós; vale depois de reiniciar */
	RTLS_CFG_READONLY,
};

struct rtls_cfg_param {
	uint16_t key;
	uint8_t cls; /**< enum rtls_cfg_class */
	bool hex;    /**< mostrar em hexadecimal (IDs) */
	const char *name;
	int32_t def;
	int32_t min;
	int32_t max;
};

/* ------------------------------------------------- tabela (pura) */

/** Parâmetros deste firmware. */
size_t rtls_config_count(void);

/** Parâmetro de índice @p idx (0 .. count - 1), ou NULL. */
const struct rtls_cfg_param *rtls_config_param(size_t idx);

/** Índice de um parâmetro pela chave, ou -ENOENT. */
int rtls_config_index(uint16_t key);

/** Parâmetro pelo nome ("loc/range_std_mm") ou pela chave ("0x0603"), ou NULL. */
const struct rtls_cfg_param *rtls_config_find(const char *name_or_key);

/**
 * @retval -EACCES parâmetro só de leitura
 * @retval -ERANGE fora da faixa
 */
int rtls_config_validate(const struct rtls_cfg_param *p, int32_t value);

/* ------------------------------------------ valores e NVS (settings) */

/** Carrega da NVS os valores salvos; os demais ficam no padrão do Kconfig. */
int rtls_config_init(void);

/** Valor atual (salvo ou padrão). Chave fora deste papel: 0. */
int32_t rtls_config_get(uint16_t key);

/** true se o valor atual veio da NVS. */
bool rtls_config_is_saved(uint16_t key);

/**
 * Valida, grava na NVS e marca a mudança para a thread main.
 *
 * @retval -ENOENT chave fora deste papel
 * @retval -EACCES / -ERANGE ver rtls_config_validate()
 */
int rtls_config_set(uint16_t key, int32_t value);

/** Apaga da NVS e volta ao padrão. key = 0 apaga todos. */
int rtls_config_reset(uint16_t key);

/**
 * Próxima chave alterada desde a última chamada (e a desmarca), ou 0 se
 * nada mudou. A thread main chama em laço e aplica as de classe imediata.
 */
uint16_t rtls_config_next_change(void);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_CONFIG_H_ */
