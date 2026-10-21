/*
 * Firmware do GATEWAY.
 *
 * Recebe os quadros POSITION das tags, mantém a tabela de tags e publica
 * snapshots periódicos para o host via SPI escravo.
 * Fluxo: docs/02-arquitetura.md, seção 4.3.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/host_if.h>
#include <rtls/status_led.h>

#include "tag_table.h"

LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

static void handle_frame(const uint8_t *frame, size_t len, const struct dw1000_rx_info *info)
{
	struct rtls_msg_position msg;
	struct rtls_mac_hdr hdr;
	const uint8_t *payload;
	size_t payload_len;
	int err;

	if (rtls_frame_parse(frame, len, CONFIG_RTLS_PAN_ID, &hdr, &payload, &payload_len) !=
	    RTLS_MSG_POSITION) {
		return;
	}

	memcpy(&msg, payload, sizeof(msg));
	err = tag_table_update(hdr.src, &msg, (int8_t)(info->rx_power_cdbm / 100), k_uptime_get());
	if (err) {
		LOG_WRN("tabela cheia; tag 0x%04x ignorada", hdr.src);
		return;
	}

	LOG_DBG("tag 0x%04x r=%u n=%u pos=(%d, %d, %d) mm", hdr.src, msg.round, msg.n_ranges,
		msg.pos_mm[0], msg.pos_mm[1], msg.pos_mm[2]);
	rtls_status_led_activity();
}

static void publish_snapshot(void)
{
	static struct rtls_tag_record records[CONFIG_RTLS_GATEWAY_MAX_TAGS];
	const size_t n = tag_table_snapshot(records, ARRAY_SIZE(records), k_uptime_get());

	(void)rtls_host_if_publish(records, n);
}

int main(void)
{
	const struct dw1000_phy_config phy =
		DW1000_PHY_CONFIG_DEFAULT(CONFIG_RTLS_PAN_ID, CONFIG_RTLS_NODE_ID);
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	struct dw1000_rx_info info;
	int64_t next_publish_ms;
	int err;

	LOG_INF("RTLS gateway 0x%04x: ate %d tags", CONFIG_RTLS_NODE_ID,
		CONFIG_RTLS_GATEWAY_MAX_TAGS);

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
	}

	tag_table_init();

	err = rtls_host_if_init();
	if (err) {
		/* Segue sem host: as posições continuam no log. */
		LOG_ERR("interface com o host indisponivel (%d)", err);
	}

	err = dw1000_init();
	if (err == 0) {
		err = dw1000_configure(&phy);
	}
	if (err) {
		LOG_ERR("DW1000 indisponivel (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	next_publish_ms = k_uptime_get() + CONFIG_RTLS_GATEWAY_PUBLISH_MS;
	while (true) {
		const int64_t wait_ms = MAX(next_publish_ms - k_uptime_get(), 0);
		int len = dw1000_receive(frame, sizeof(frame), &info, K_MSEC(wait_ms));

		if (len > 0) {
			handle_frame(frame, (size_t)len, &info);
		}

		if (k_uptime_get() >= next_publish_ms) {
			publish_snapshot();
			next_publish_ms += CONFIG_RTLS_GATEWAY_PUBLISH_MS;
		}
	}

	return 0;
}
