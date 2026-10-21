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

int main(void)
{
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
		err = rtls_twr_responder_init(&cfg);
	}
	if (err) {
		LOG_ERR("falha na inicializacao (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	while (true) {
		err = rtls_twr_responder_run();
		if (err == -ENOSYS) {
			LOG_WRN("responder DS-TWR ainda nao implementado (M2)");
			k_sleep(K_FOREVER);
			continue;
		}

		LOG_ERR("responder parou (%d); reiniciando", err);
		rtls_status_led_error(true);
		k_sleep(K_SECONDS(1));
		rtls_status_led_error(false);
	}

	return 0;
}
