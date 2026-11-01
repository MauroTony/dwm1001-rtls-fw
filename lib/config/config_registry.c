/*
 * Tabela de parâmetros (pura). Cada entrada só existe se o Kconfig que lhe
 * dá o padrão existir neste firmware: é assim que a tabela fica com o papel
 * do nó (tag, âncora ou gateway).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <rtls/config.h>
#include <rtls/frames.h>

#define PARAM(k, n, d, lo, hi, c, h)                                                               \
	{.key = (k), .name = (n), .def = (d), .min = (lo), .max = (hi), .cls = (c), .hex = (h)}

static const struct rtls_cfg_param params[] = {
#if defined(CONFIG_RTLS_NODE_ID)
#if defined(CONFIG_RTLS_GATEWAY_MAX_TAGS)
	/* As tags enviam POSITION para RTLS_ADDR_GATEWAY: o ID do gateway é fixo. */
	PARAM(RTLS_CFG_NODE_ID, "node/id", CONFIG_RTLS_NODE_ID, CONFIG_RTLS_NODE_ID,
	      CONFIG_RTLS_NODE_ID, RTLS_CFG_READONLY, true),
#elif defined(CONFIG_RTLS_ANCHOR_POS_X_MM)
	PARAM(RTLS_CFG_NODE_ID, "node/id", CONFIG_RTLS_NODE_ID, 0xA001, 0xAFFF, RTLS_CFG_REBOOT,
	      true),
#else
	PARAM(RTLS_CFG_NODE_ID, "node/id", CONFIG_RTLS_NODE_ID, 0x1001, 0x1FFF, RTLS_CFG_REBOOT,
	      true),
#endif
#endif
#if defined(CONFIG_RTLS_PAN_ID)
	/* 0xFFFF é o PAN de broadcast. */
	PARAM(RTLS_CFG_PAN_ID, "node/pan", CONFIG_RTLS_PAN_ID, 0, 0xFFFE, RTLS_CFG_COORD, true),
#endif
#if defined(CONFIG_RTLS_ANCHOR_POS_X_MM)
	PARAM(RTLS_CFG_ANCHOR_POS_X, "anchor/pos_x_mm", CONFIG_RTLS_ANCHOR_POS_X_MM, -1000000,
	      1000000, RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_ANCHOR_POS_Y, "anchor/pos_y_mm", CONFIG_RTLS_ANCHOR_POS_Y_MM, -1000000,
	      1000000, RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_ANCHOR_POS_Z, "anchor/pos_z_mm", CONFIG_RTLS_ANCHOR_POS_Z_MM, -1000000,
	      1000000, RTLS_CFG_IMMEDIATE, false),
#endif
#if defined(CONFIG_RTLS_DW1000_TX_ANT_DLY)
	PARAM(RTLS_CFG_TX_ANT_DLY, "radio/tx_ant_dly", CONFIG_RTLS_DW1000_TX_ANT_DLY, 0, 65535,
	      RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_RX_ANT_DLY, "radio/rx_ant_dly", CONFIG_RTLS_DW1000_RX_ANT_DLY, 0, 65535,
	      RTLS_CFG_IMMEDIATE, false),
#endif
#if defined(CONFIG_RTLS_TAG_NUM_ANCHORS)
	PARAM(RTLS_CFG_TAG_ANCHOR_BASE, "tag/anchor_base", CONFIG_RTLS_TAG_ANCHOR_BASE_ID, 0xA001,
	      0xAFFF, RTLS_CFG_REBOOT, true),
	PARAM(RTLS_CFG_TAG_NUM_ANCHORS, "tag/num_anchors", CONFIG_RTLS_TAG_NUM_ANCHORS, 1,
	      RTLS_MAX_ANCHORS_PER_ROUND, RTLS_CFG_REBOOT, false),
#endif
#if defined(CONFIG_RTLS_LOC)
	PARAM(RTLS_CFG_LOC_MODE_3D, "loc/mode_3d", IS_ENABLED(CONFIG_RTLS_LOC_MODE_3D), 0, 1,
	      RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_LOC_TAG_HEIGHT, "loc/tag_height_mm", CONFIG_RTLS_LOC_TAG_HEIGHT_MM, -100000,
	      100000, RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_LOC_RANGE_STD, "loc/range_std_mm", CONFIG_RTLS_LOC_RANGE_STD_MM, 1, 5000,
	      RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_LOC_ACCEL_STD, "loc/accel_std_mms2", CONFIG_RTLS_LOC_ACCEL_STD_MMS2, 1,
	      100000, RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_LOC_GATE_CHI2, "loc/gate_chi2_x100", CONFIG_RTLS_LOC_GATE_CHI2_X100, 1,
	      100000, RTLS_CFG_IMMEDIATE, false),
	PARAM(RTLS_CFG_LOC_NLOS_WEIGHT, "loc/nlos_weight",
	      IS_ENABLED(CONFIG_RTLS_LOC_NLOS_WEIGHTING), 0, 1, RTLS_CFG_IMMEDIATE, false),
#endif
};

size_t rtls_config_count(void)
{
	return ARRAY_SIZE(params);
}

const struct rtls_cfg_param *rtls_config_param(size_t idx)
{
	return (idx < ARRAY_SIZE(params)) ? &params[idx] : NULL;
}

int rtls_config_index(uint16_t key)
{
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		if (params[i].key == key) {
			return (int)i;
		}
	}

	return -ENOENT;
}

const struct rtls_cfg_param *rtls_config_find(const char *name_or_key)
{
	if (name_or_key == NULL) {
		return NULL;
	}

	if (strncmp(name_or_key, "0x", 2) == 0 || strncmp(name_or_key, "0X", 2) == 0) {
		char *end;
		const unsigned long key = strtoul(name_or_key, &end, 16);
		const int idx = (*end == '\0' && key <= UINT16_MAX) ? rtls_config_index(key) : -1;

		return (idx >= 0) ? &params[idx] : NULL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		if (strcmp(params[i].name, name_or_key) == 0) {
			return &params[i];
		}
	}

	return NULL;
}

int rtls_config_validate(const struct rtls_cfg_param *p, int32_t value)
{
	if (p->cls == RTLS_CFG_READONLY) {
		return -EACCES;
	}
	if (value < p->min || value > p->max) {
		return -ERANGE;
	}

	return 0;
}
