/*
 * Firmware da TAG.
 *
 * A cada ciclo (CONFIG_RTLS_TAG_UPDATE_RATE_HZ) mede a distância até as
 * âncoras por DS-TWR, estima a posição com o EKF e envia o resultado ao
 * gateway. Fluxo: docs/02-arquitetura.md, seção 4.1.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/loc.h>
#include <rtls/status_led.h>
#include <rtls/twr.h>

LOG_MODULE_REGISTER(tag, LOG_LEVEL_INF);

#define CYCLE_MS (1000 / CONFIG_RTLS_TAG_UPDATE_RATE_HZ)

static struct rtls_loc_ctx loc;
static uint8_t mac_seq;

static int32_t m_to_mm(float m)
{
	return (int32_t)(m * 1000.0f + (m >= 0.0f ? 0.5f : -0.5f));
}

static uint16_t std_to_mm(float std_m)
{
	const float mm = std_m * 1000.0f;

	return (mm >= (float)UINT16_MAX) ? UINT16_MAX : (uint16_t)(mm + 0.5f);
}

static int send_position(const struct rtls_position *pos, uint8_t round)
{
	struct rtls_msg_position msg = {
		.type = RTLS_MSG_POSITION,
		.round = round,
		.n_ranges = pos->n_ranges,
		.flags = pos->flags,
		.t_ms = (uint32_t)pos->t_ms,
	};
	struct rtls_mac_hdr hdr;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	int len;

	for (int i = 0; i < 3; i++) {
		msg.pos_mm[i] = m_to_mm(pos->pos_m[i]);
		msg.std_mm[i] = std_to_mm(pos->std_m[i]);
	}

	rtls_mac_hdr_init(&hdr, mac_seq++, CONFIG_RTLS_PAN_ID, RTLS_ADDR_GATEWAY,
			  CONFIG_RTLS_NODE_ID);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, &msg, sizeof(msg));
	if (len < 0) {
		return len;
	}

	return dw1000_transmit(frame, len, NULL, NULL);
}

/* Uma linha CSV por distância (lida por tools/rtls_log.py). */
static void print_ranges(const struct rtls_range_set *set)
{
	if (!IS_ENABLED(CONFIG_RTLS_TAG_CSV_OUTPUT)) {
		return;
	}

	for (int i = 0; i < set->count; i++) {
		const struct rtls_range *r = &set->ranges[i];

		printk("R,%u,%u,%04x,%d,%d,%d\n", set->round, (uint32_t)set->t_ms, r->anchor_id,
		       m_to_mm(r->distance_m), r->rx_power_cdbm, r->fp_power_cdbm);
	}
}

static void log_stats(void)
{
	struct rtls_twr_initiator_stats st;
	uint32_t pct;

	rtls_twr_initiator_get_stats(&st, true);
	if (st.rounds == 0) {
		return;
	}

	LOG_INF("TWR: %u rodadas, %u%% completas, ultima %u us, max %u us, TX atrasado %u",
		st.rounds, 100U * st.rounds_complete / st.rounds, st.last_round_us,
		st.max_round_us, st.late_tx);
	for (int i = 0; i < CONFIG_RTLS_TAG_NUM_ANCHORS; i++) {
		pct = 100U * st.anchors[i].range_ok / st.rounds;
		LOG_INF("  ancora 0x%04x: RESP %u, distancias %u (%u%%), invalidas %u",
			CONFIG_RTLS_TAG_ANCHOR_BASE_ID + i, st.anchors[i].resp_ok,
			st.anchors[i].range_ok, pct, st.anchors[i].invalid);
	}
}

int main(void)
{
	const struct dw1000_phy_config phy =
		DW1000_PHY_CONFIG_DEFAULT(CONFIG_RTLS_PAN_ID, CONFIG_RTLS_NODE_ID);
	struct rtls_twr_initiator_cfg twr_cfg = {
		.pan_id = CONFIG_RTLS_PAN_ID,
		.tag_id = CONFIG_RTLS_NODE_ID,
		.n_anchors = CONFIG_RTLS_TAG_NUM_ANCHORS,
	};
	struct rtls_loc_cfg loc_cfg;
	bool loc_warned = false;
	int64_t next_ms, next_stats_ms;
	int err;

	LOG_INF("RTLS tag 0x%04x: %d Hz, %d ancoras a partir de 0x%04x", CONFIG_RTLS_NODE_ID,
		CONFIG_RTLS_TAG_UPDATE_RATE_HZ, CONFIG_RTLS_TAG_NUM_ANCHORS,
		CONFIG_RTLS_TAG_ANCHOR_BASE_ID);

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
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

	for (int i = 0; i < CONFIG_RTLS_TAG_NUM_ANCHORS; i++) {
		twr_cfg.anchors[i] = CONFIG_RTLS_TAG_ANCHOR_BASE_ID + i;
	}

	err = rtls_twr_initiator_init(&twr_cfg);
	if (err == 0) {
		rtls_loc_default_cfg(&loc_cfg);
		err = rtls_loc_init(&loc, &loc_cfg);
	}
	if (err) {
		LOG_ERR("falha na configuracao (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	next_ms = k_uptime_get();
	next_stats_ms = next_ms + CONFIG_RTLS_TAG_STATS_PERIOD_S * 1000;
	while (true) {
		struct rtls_range_set ranges;
		struct rtls_position pos;

		next_ms += CYCLE_MS;

		err = rtls_twr_initiator_run_round(&ranges);
		if (err == 0) {
			rtls_status_led_activity();
			print_ranges(&ranges);

			err = rtls_loc_update(&loc, &ranges, &pos);
			if (err == 0) {
				err = send_position(&pos, ranges.round);
			} else if (err == -ENOSYS && !loc_warned) {
				LOG_WRN("localizacao (EKF, M4) ainda nao implementada");
				loc_warned = true;
			}
		}

		if (CONFIG_RTLS_TAG_STATS_PERIOD_S > 0 && k_uptime_get() >= next_stats_ms) {
			log_stats();
			next_stats_ms += CONFIG_RTLS_TAG_STATS_PERIOD_S * 1000;
		}

		k_sleep(K_TIMEOUT_ABS_MS(next_ms));
	}

	return 0;
}
