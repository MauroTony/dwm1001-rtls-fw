/*
 * Firmware do GATEWAY.
 *
 * Recebe os quadros POSITION das tags, mantém a tabela de tags e publica
 * snapshots periódicos para o host via SPI escravo.
 * Fluxo: docs/02-arquitetura.md, seção 4.3.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <rtls/config.h>
#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/host_if.h>
#include <rtls/status_led.h>
#include <rtls/tdma.h>
#include <rtls/watchdog.h>

#include "tag_table.h"

LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

#define SNAPSHOT_RECORDS (CONFIG_RTLS_GATEWAY_MAX_TAGS + CONFIG_RTLS_GATEWAY_SIM_TAGS)

BUILD_ASSERT(SNAPSHOT_RECORDS <= CONFIG_RTLS_HOST_IF_MAX_RECORDS,
	     "aumente CONFIG_RTLS_HOST_IF_MAX_RECORDS");

/* Primeiro ID das tags simuladas (CONFIG_RTLS_GATEWAY_SIM_TAGS). */
#define SIM_TAG_BASE 0x1F00

/* Superquadro TDMA: 4 ms de BEACON + slots de tag (docs/03, seção 5.2). */
#define SUPERFRAME_US                                                                              \
	(RTLS_TDMA_BEACON_SEG_US + CONFIG_RTLS_GATEWAY_TDMA_SLOTS * CONFIG_RTLS_GATEWAY_TDMA_SLOT_US)

static uint8_t mac_seq;

/* PAN do registro de parâmetros (M8); o ID do gateway é fixo (RTLS_ADDR_GATEWAY). */
static uint16_t pan_id;

/* BEACONs desde o último resumo (T7.1: período estável). */
static struct {
	uint32_t sent;
	uint32_t errors;
	int64_t max_late_us; /**< maior atraso em relação à grade de 100 ms */
} beacon_stats;

static int64_t now_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

/* BEACON em broadcast: abre o superquadro @p superframe. */
static int send_beacon(uint16_t superframe)
{
	const struct rtls_msg_beacon msg = {
		.type = RTLS_MSG_BEACON,
		.superframe = superframe,
		.n_slots = CONFIG_RTLS_GATEWAY_TDMA_SLOTS,
		.slot_us = CONFIG_RTLS_GATEWAY_TDMA_SLOT_US,
	};
	struct rtls_mac_hdr hdr;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	int len;

	rtls_mac_hdr_init(&hdr, mac_seq++, pan_id, RTLS_ADDR_BROADCAST, CONFIG_RTLS_NODE_ID);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, &msg, sizeof(msg));
	if (len < 0) {
		return len;
	}

	/* O RX já está desligado: dw1000_receive() o desliga no timeout. */
	return dw1000_transmit(frame, len, NULL, NULL);
}

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

	if (rtls_frame_parse(frame, len, pan_id, &hdr, &payload, &payload_len) !=
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

	if (IS_ENABLED(CONFIG_RTLS_GATEWAY_BEACON)) {
		LOG_INF("GW: %u BEACONs, %u erros de TX, atraso max %d us", beacon_stats.sent,
			beacon_stats.errors, (int)beacon_stats.max_late_us);
		memset(&beacon_stats, 0, sizeof(beacon_stats));
	}

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

/*
 * Registros sintéticos para testar a paginação do SPI (T6.3/T6.4): cada tag
 * anda 10 mm em x por snapshot, numa faixa de 6 m, e o round avança.
 */
static size_t add_sim_tags(struct rtls_tag_record *out)
{
	static uint8_t sim_round;

	sim_round++;
	for (int i = 0; i < CONFIG_RTLS_GATEWAY_SIM_TAGS; i++) {
		out[i] = (struct rtls_tag_record){
			.tag_id = SIM_TAG_BASE + i,
			.round = sim_round,
			.n_ranges = 4,
			.flags = RTLS_POS_FLAG_VALID,
			.rssi_dbm = -80,
			.pos_mm = {1000 + (sim_round * 10 + i * 500) % 6000, 500 * i, 1000},
			.std_mm = {50, 50, 0},
		};
	}

	return CONFIG_RTLS_GATEWAY_SIM_TAGS;
}

static void publish_snapshot(void)
{
	static struct rtls_tag_record records[SNAPSHOT_RECORDS];
	size_t n = tag_table_snapshot(records, CONFIG_RTLS_GATEWAY_MAX_TAGS, k_uptime_get());

	if (CONFIG_RTLS_GATEWAY_SIM_TAGS > 0) {
		n += add_sim_tags(&records[n]);
	}
	(void)rtls_host_if_publish(records, n);
}

static void apply_antenna_delay(void)
{
	const uint16_t tx = (uint16_t)rtls_config_get(RTLS_CFG_TX_ANT_DLY);
	const uint16_t rx = (uint16_t)rtls_config_get(RTLS_CFG_RX_ANT_DLY);

	if (dw1000_set_antenna_delay(tx, rx) == 0) {
		LOG_INF("atrasos de antena: TX %u, RX %u", tx, rx);
	}
}

/* Parâmetros de classe imediata alterados pelo shell; o PAN vale no próximo boot. */
static void apply_config_changes(void)
{
	uint16_t key;

	while ((key = rtls_config_next_change()) != 0) {
		if (key == RTLS_CFG_TX_ANT_DLY || key == RTLS_CFG_RX_ANT_DLY) {
			apply_antenna_delay();
		}
	}
}

static int radio_init(void)
{
	const struct dw1000_phy_config phy = DW1000_PHY_CONFIG_DEFAULT(pan_id, CONFIG_RTLS_NODE_ID);
	int err;

	err = dw1000_init();
	if (err == 0) {
		err = dw1000_configure(&phy);
	}
	if (err == 0) {
		apply_antenna_delay();
	}
	return err;
}

int main(void)
{
	/* Anunciado ao host em RTLS_INFO_GET; versão do arquivo VERSION do app. */
	const struct rtls_host_if_info host_info = {
		.fw_version = {APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL},
		.gateway_id = CONFIG_RTLS_NODE_ID,
		.capacity = CONFIG_RTLS_GATEWAY_MAX_TAGS,
		.superframe_ms = IS_ENABLED(CONFIG_RTLS_GATEWAY_BEACON) ? SUPERFRAME_US / 1000
									: CONFIG_RTLS_GATEWAY_PUBLISH_MS,
	};
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	struct dw1000_rx_info info;
	int64_t next_publish_us, next_stats_us, next_beacon_us;
	uint16_t superframe = 0;
	int err;

	/* Falha da NVS: segue com os padrões do Kconfig (já registrado no log). */
	(void)rtls_config_init();
	pan_id = (uint16_t)rtls_config_get(RTLS_CFG_PAN_ID);

	LOG_INF("RTLS gateway 0x%04x v%s: ate %d tags, PAN 0x%04x", CONFIG_RTLS_NODE_ID,
		APP_VERSION_STRING, CONFIG_RTLS_GATEWAY_MAX_TAGS, pan_id);
	if (CONFIG_RTLS_GATEWAY_SIM_TAGS > 0) {
		LOG_WRN("modo de teste: %d tags simuladas no snapshot (0x%04x...)",
			CONFIG_RTLS_GATEWAY_SIM_TAGS, SIM_TAG_BASE);
	}
	if (IS_ENABLED(CONFIG_RTLS_GATEWAY_BEACON)) {
		LOG_INF("TDMA: BEACON a cada %d us, %d slots de %d us", SUPERFRAME_US,
			CONFIG_RTLS_GATEWAY_TDMA_SLOTS, CONFIG_RTLS_GATEWAY_TDMA_SLOT_US);
	}

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
	}

	tag_table_init();

	err = rtls_host_if_init(&host_info);
	if (err) {
		/* Segue sem host: as posições continuam no log. */
		LOG_ERR("interface com o host indisponivel (%d)", err);
	}

	err = radio_init();
	if (err) {
		LOG_ERR("DW1000 indisponivel (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	err = rtls_watchdog_init();
	if (err) {
		LOG_WRN("sem watchdog (%d)", err);
	}

	/* Agenda em µs: o BEACON precisa de uma grade estável de 100 ms. */
	next_beacon_us = now_us();
	next_publish_us = now_us() + CONFIG_RTLS_GATEWAY_PUBLISH_MS * 1000LL;
	next_stats_us = now_us() + CONFIG_RTLS_GATEWAY_STATS_PERIOD_S * 1000000LL;
	while (true) {
		int64_t wake_us = next_publish_us;
		int len;

		/* O laço volta pelo menos a cada snapshot (100 ms). */
		rtls_watchdog_feed();
		apply_config_changes();

		if (IS_ENABLED(CONFIG_RTLS_GATEWAY_BEACON)) {
			wake_us = MIN(wake_us, next_beacon_us);
		}
		len = dw1000_receive(frame, sizeof(frame), &info,
				     K_TIMEOUT_ABS_TICKS(k_us_to_ticks_ceil64(MAX(wake_us, 0))));
		if (len > 0) {
			handle_frame(frame, (size_t)len, &info);
		}

		if (IS_ENABLED(CONFIG_RTLS_GATEWAY_BEACON) && now_us() >= next_beacon_us) {
			const int64_t late_us = now_us() - next_beacon_us;

			if (send_beacon(superframe++) == 0) {
				beacon_stats.sent++;
				beacon_stats.max_late_us = MAX(beacon_stats.max_late_us, late_us);
			} else {
				beacon_stats.errors++;
			}
			next_beacon_us += SUPERFRAME_US;
			/* Atrasou mais de um superquadro: recomeça a grade a partir de agora. */
			if (now_us() >= next_beacon_us) {
				next_beacon_us = now_us() + SUPERFRAME_US;
			}
		}

		if (now_us() >= next_publish_us) {
			publish_snapshot();
			next_publish_us += CONFIG_RTLS_GATEWAY_PUBLISH_MS * 1000LL;
		}

		if (CONFIG_RTLS_GATEWAY_STATS_PERIOD_S > 0 && now_us() >= next_stats_us) {
			log_stats();
			next_stats_us += CONFIG_RTLS_GATEWAY_STATS_PERIOD_S * 1000000LL;
		}
	}

	return 0;
}
