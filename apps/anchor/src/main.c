/*
 * Firmware da ÂNCORA.
 *
 * Nó fixo em posição conhecida: responde ao DS-TWR das tags e informa a
 * própria posição no REPORT. Fluxo: docs/02-arquitetura.md, seção 4.2.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/dw1000.h>
#include <rtls/status_led.h>
#include <rtls/twr.h>

LOG_MODULE_REGISTER(anchor, LOG_LEVEL_INF);

#define STATS_PERIOD K_SECONDS(10)

static void stats_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(stats_work, stats_work_handler);

/* Resumo periódico (roda na workqueue do sistema; o laço do responder não para). */
static void stats_work_handler(struct k_work *work)
{
	struct rtls_twr_responder_stats st;

	ARG_UNUSED(work);

	rtls_twr_responder_get_stats(&st, true);
	if (st.polls > 0) {
		LOG_INF("TWR: POLL %u, RESP %u, FINAL %u (timeout %u), REPORT %u, TX atrasado %u",
			st.polls, st.resp_sent, st.finals, st.final_timeout, st.reports_sent,
			st.late_tx);
		rtls_status_led_activity();
	}

	k_work_schedule(&stats_work, STATS_PERIOD);
}

int main(void)
{
	const struct dw1000_phy_config phy =
		DW1000_PHY_CONFIG_DEFAULT(CONFIG_RTLS_PAN_ID, CONFIG_RTLS_NODE_ID);
	const struct rtls_twr_responder_cfg cfg = {
		.pan_id = CONFIG_RTLS_PAN_ID,
		.anchor_id = CONFIG_RTLS_NODE_ID,
		.pos_mm = {CONFIG_RTLS_ANCHOR_POS_X_MM, CONFIG_RTLS_ANCHOR_POS_Y_MM,
			   CONFIG_RTLS_ANCHOR_POS_Z_MM},
	};
	int err;

	LOG_INF("RTLS ancora 0x%04x em (%d, %d, %d) mm", cfg.anchor_id, cfg.pos_mm[0],
		cfg.pos_mm[1], cfg.pos_mm[2]);

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
	}

	err = dw1000_init();
	if (err == 0) {
		err = dw1000_configure(&phy);
	}
	if (err == 0) {
		err = rtls_twr_responder_init(&cfg);
	}
	if (err) {
		LOG_ERR("falha na inicializacao (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	k_work_schedule(&stats_work, STATS_PERIOD);

	while (true) {
		err = rtls_twr_responder_run();

		LOG_ERR("responder parou (%d); reiniciando", err);
		rtls_status_led_error(true);
		k_sleep(K_SECONDS(1));
		rtls_status_led_error(false);
	}

	return 0;
}
