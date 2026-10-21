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

int main(void)
{
	struct rtls_twr_initiator_cfg twr_cfg = {
		.pan_id = CONFIG_RTLS_PAN_ID,
		.tag_id = CONFIG_RTLS_NODE_ID,
		.n_anchors = CONFIG_RTLS_TAG_NUM_ANCHORS,
	};
	struct rtls_loc_cfg loc_cfg;
	bool warned = false;
	int64_t next_ms;
	int err;

	LOG_INF("RTLS tag 0x%04x: %d Hz, %d ancoras a partir de 0x%04x", CONFIG_RTLS_NODE_ID,
		CONFIG_RTLS_TAG_UPDATE_RATE_HZ, CONFIG_RTLS_TAG_NUM_ANCHORS,
		CONFIG_RTLS_TAG_ANCHOR_BASE_ID);

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
	}

	err = dw1000_init();
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
	while (true) {
		struct rtls_range_set ranges;
		struct rtls_position pos;

		next_ms += CYCLE_MS;

		err = rtls_twr_initiator_run_round(&ranges);
		if (err == 0) {
			err = rtls_loc_update(&loc, &ranges, &pos);
		}
		if (err == 0) {
			err = send_position(&pos, ranges.round);
			rtls_status_led_activity();
		}

		if (err == -ENOSYS && !warned) {
			LOG_WRN("ciclo TWR -> EKF -> POSITION ainda nao implementado (M1-M5)");
			warned = true;
		}

		k_sleep(K_TIMEOUT_ABS_MS(next_ms));
	}

	return 0;
}
