#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/dw1000.h>
#include <rtls/twr.h>
#include <rtls/twr_math.h>

LOG_MODULE_REGISTER(rtls_twr, CONFIG_RTLS_TWR_LOG_LEVEL);

static struct rtls_twr_initiator_cfg initiator_cfg;
static uint8_t round_seq;

int rtls_twr_initiator_init(const struct rtls_twr_initiator_cfg *cfg)
{
	if (cfg == NULL || cfg->n_anchors == 0 ||
	    cfg->n_anchors > RTLS_MAX_ANCHORS_PER_ROUND) {
		return -EINVAL;
	}

	initiator_cfg = *cfg;
	round_seq = 0;
	return 0;
}

int rtls_twr_initiator_run_round(struct rtls_range_set *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	out->round = round_seq++;
	out->count = 0;
	out->t_ms = k_uptime_get();

	/*
	 * TODO(M2/M3), docs/03-protocolo-uwb.md seções 3 e 4:
	 *  1. POLL broadcast com initiator_cfg.anchors; guardar poll_tx real.
	 *  2. Para cada âncora i: RX do RESP (round, origem = anchors[i]) até
	 *     poll_tx + T_reply + i*T_slot + margem; guardar resp_rx[i].
	 *  3. FINAL com TX atrasado em poll_tx + T_reply + n*T_slot; guardar
	 *     final_tx real (registrador TX_STAMP).
	 *  4. Para cada âncora i: RX do REPORT; rtls_twr_tag_intervals() ->
	 *     rtls_twr_ds_tof() -> rtls_twr_tof_to_m() -> out->ranges[count++].
	 */
	return -ENOSYS;
}
