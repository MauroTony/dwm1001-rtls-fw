/*
 * Firmware da ÂNCORA.
 *
 * Nó fixo em posição conhecida: responde ao DS-TWR das tags e informa a
 * própria posição no REPORT. Fluxo: docs/02-arquitetura.md, seção 4.2.
 * ID, PAN, posição e atrasos de antena vêm do registro de parâmetros (M8):
 * Kconfig/nodes/aN.conf como padrão, NVS (shell "rtls set") por cima.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/config.h>
#include <rtls/dw1000.h>
#include <rtls/status_led.h>
#include <rtls/twr.h>
#include <rtls/watchdog.h>

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

static void apply_antenna_delay(void)
{
	const uint16_t tx = (uint16_t)rtls_config_get(RTLS_CFG_TX_ANT_DLY);
	const uint16_t rx = (uint16_t)rtls_config_get(RTLS_CFG_RX_ANT_DLY);

	if (dw1000_set_antenna_delay(tx, rx) == 0) {
		LOG_INF("atrasos de antena: TX %u, RX %u", tx, rx);
	}
}

static void current_position(int32_t pos_mm[3])
{
	pos_mm[0] = rtls_config_get(RTLS_CFG_ANCHOR_POS_X);
	pos_mm[1] = rtls_config_get(RTLS_CFG_ANCHOR_POS_Y);
	pos_mm[2] = rtls_config_get(RTLS_CFG_ANCHOR_POS_Z);
}

/*
 * Thread main, entre rodadas do responder: alimenta o watchdog e aplica os
 * parâmetros de classe imediata alterados pelo shell. Os demais (ID, PAN)
 * valem no próximo boot.
 */
static void idle_hook(void)
{
	int32_t pos_mm[3];
	uint16_t key;

	rtls_watchdog_feed();

	while ((key = rtls_config_next_change()) != 0) {
		switch (key) {
		case RTLS_CFG_ANCHOR_POS_X:
		case RTLS_CFG_ANCHOR_POS_Y:
		case RTLS_CFG_ANCHOR_POS_Z:
			current_position(pos_mm);
			rtls_twr_responder_set_position(pos_mm);
			LOG_INF("posicao: (%d, %d, %d) mm", pos_mm[0], pos_mm[1], pos_mm[2]);
			break;
		case RTLS_CFG_TX_ANT_DLY:
		case RTLS_CFG_RX_ANT_DLY:
			apply_antenna_delay();
			break;
		default:
			break;
		}
	}
}

static int radio_init(uint16_t pan_id, uint16_t node_id)
{
	const struct dw1000_phy_config phy = DW1000_PHY_CONFIG_DEFAULT(pan_id, node_id);
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
	struct rtls_twr_responder_cfg cfg = {.idle_hook = idle_hook};
	int err;

	/* Falha da NVS: segue com os padrões do Kconfig (já registrado no log). */
	(void)rtls_config_init();

	cfg.pan_id = (uint16_t)rtls_config_get(RTLS_CFG_PAN_ID);
	cfg.anchor_id = (uint16_t)rtls_config_get(RTLS_CFG_NODE_ID);
	current_position(cfg.pos_mm);

	LOG_INF("RTLS ancora 0x%04x em (%d, %d, %d) mm", cfg.anchor_id, cfg.pos_mm[0],
		cfg.pos_mm[1], cfg.pos_mm[2]);

	err = rtls_status_led_init();
	if (err) {
		LOG_WRN("LEDs indisponiveis (%d)", err);
	}

	err = radio_init(cfg.pan_id, cfg.anchor_id);
	if (err == 0) {
		err = rtls_twr_responder_init(&cfg);
	}
	if (err) {
		LOG_ERR("falha na inicializacao (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	err = rtls_watchdog_init();
	if (err) {
		LOG_WRN("sem watchdog (%d)", err);
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
