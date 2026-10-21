#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/dw1000.h>
#include <rtls/twr.h>

LOG_MODULE_DECLARE(rtls_twr, CONFIG_RTLS_TWR_LOG_LEVEL);

static struct rtls_twr_responder_cfg responder_cfg;

int rtls_twr_responder_init(const struct rtls_twr_responder_cfg *cfg)
{
	if (cfg == NULL) {
		return -EINVAL;
	}

	responder_cfg = *cfg;
	return 0;
}

int rtls_twr_responder_run(void)
{
	/*
	 * TODO(M2/M3), docs/02-arquitetura.md seção 4.2:
	 *  - RX contínuo; POLL que cita responder_cfg.anchor_id na posição i:
	 *    RESP com TX atrasado em poll_rx + T_reply + i*T_slot.
	 *  - FINAL da mesma rodada: REPORT em final_rx + T_reply + i*T_slot
	 *    com Db = resp_tx - poll_rx, Rb = final_rx - resp_tx e a posição.
	 *  - Timeout ou rodada inesperada: descarta o estado e volta ao RX.
	 */
	return -ENOSYS;
}
