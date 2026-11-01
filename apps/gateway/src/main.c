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
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/host_if.h>
#include <rtls/status_led.h>

#include "tag_table.h"

LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

/* Uma linha CSV por POSITION recebido (lida por tools/rtls_log.py). */
static void print_position(uint16_t tag_id, const struct rtls_msg_position *msg,
			   const struct dw1000_rx_info *info, int64_t now_ms)
{
	if (!IS_ENABLED(CONFIG_RTLS_GATEWAY_CSV_OUTPUT)) {
		return;
	}

	printk("P,%04x,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u,%d,%d,%u\n", tag_id, msg->round, msg->t_ms,
	       msg->pos_mm[0], msg->pos_mm[1], msg->pos_mm[2], msg->std_mm[0], msg->std_mm[1],
	       msg->std_mm[2], msg->n_ranges, msg->flags, info->rx_power_cdbm,
	       info->fp_power_cdbm, (uint32_t)now_ms);
}

static void handle_frame(const uint8_t *frame, size_t len, const struct dw1000_rx_info *info)
{
	const int64_t now_ms = k_uptime_get();
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
	err = tag_table_update(hdr.src, &msg, info->rx_power_cdbm, now_ms);
	if (err == -ENOMEM) {
		LOG_WRN("tabela cheia; tag 0x%04x ignorada", hdr.src);
		return;
	}
	if (err) {
		return;
	}

	print_position(hdr.src, &msg, info, now_ms);
	rtls_status_led_activity();
}

/* Resumo por tag: POSITION recebidos e rodadas sem POSITION (lacunas de round). */
static void log_stats(void)
{
	static struct tag_stats stats[CONFIG_RTLS_GATEWAY_MAX_TAGS];
	const size_t n = tag_table_get_stats(stats, ARRAY_SIZE(stats), true);

	if (n == 0) {
		LOG_INF("GW: nenhuma tag ativa");
		return;
	}

	for (size_t i = 0; i < n; i++) {
		const struct tag_stats *st = &stats[i];
		const uint32_t rounds = st->received + st->lost;

		if (st->received == 0) {
			continue;
		}
		LOG_INF("GW: tag 0x%04x: %u posicoes, %u lacunas (%u%% recebidas), RX %d dBm",
			st->tag_id, st->received, st->lost, 100U * st->received / rounds,
			st->rx_power_sum_cdbm / (int32_t)st->received / 100);
	}
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
	int64_t next_publish_ms, next_stats_ms;
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
	next_stats_ms = k_uptime_get() + CONFIG_RTLS_GATEWAY_STATS_PERIOD_S * 1000;
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

		if (CONFIG_RTLS_GATEWAY_STATS_PERIOD_S > 0 && k_uptime_get() >= next_stats_ms) {
			log_stats();
			next_stats_ms += CONFIG_RTLS_GATEWAY_STATS_PERIOD_S * 1000;
		}
	}

	return 0;
}
