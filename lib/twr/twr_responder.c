/*
 * DS-TWR responder (âncora): responde ao POLL no seu slot, espera o FINAL e
 * devolve no REPORT os intervalos medidos no próprio relógio e a posição
 * (docs/02-arquitetura.md seção 4.2, docs/03-protocolo-uwb.md seção 3).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <rtls/twr.h>
#include <rtls/twr_math.h>

#include "twr_internal.h"

LOG_MODULE_DECLARE(rtls_twr, CONFIG_RTLS_TWR_LOG_LEVEL);

static struct rtls_twr_responder_cfg responder_cfg;
static struct rtls_twr_responder_stats responder_stats;

int rtls_twr_responder_init(const struct rtls_twr_responder_cfg *cfg)
{
	if (cfg == NULL) {
		return -EINVAL;
	}

	responder_cfg = *cfg;
	memset(&responder_stats, 0, sizeof(responder_stats));
	return 0;
}

static int slot_of(const struct rtls_msg_poll *poll)
{
	const uint8_t n = MIN(poll->n_anchors, RTLS_MAX_ANCHORS_PER_ROUND);

	for (int i = 0; i < n; i++) {
		if (poll->anchors[i] == responder_cfg.anchor_id) {
			return i;
		}
	}
	return -1;
}

/* Uma rodada a partir do POLL recebido em @p poll_rx. */
static void respond(uint16_t tag_id, uint8_t round, uint8_t slot, uint8_t n_anchors,
		    uint64_t poll_rx)
{
	const struct rtls_msg_resp resp = {.type = RTLS_MSG_RESP, .round = round};
	struct dw1000_tx_opts opts = {
		.delayed_ts = rtls_twr_slot_ts(poll_rx, TWR_REPLY_US, TWR_SLOT_US, slot),
		.rx_after_tx = true,
	};
	struct rtls_msg_report report = {.type = RTLS_MSG_REPORT, .round = round};
	struct rtls_msg_final final;
	struct dw1000_rx_info info;
	struct rtls_mac_hdr hdr;
	k_timepoint_t deadline;
	uint64_t resp_tx, final_rx;
	int err;

	err = twr_send(responder_cfg.pan_id, responder_cfg.anchor_id, tag_id, &resp, sizeof(resp),
		       &opts, &resp_tx);
	if (err) {
		if (err == -ETIME) {
			responder_stats.late_tx++;
		}
		return;
	}
	responder_stats.resp_sent++;

	/* O FINAL chega ~(n_anchors - slot) slots depois deste RESP. */
	deadline = sys_timepoint_calc(
		K_USEC((n_anchors - slot) * TWR_SLOT_US + TWR_REPLY_US + TWR_MARGIN_US));
	do {
		err = twr_recv(responder_cfg.pan_id, RTLS_MSG_FINAL, deadline, &hdr, &final,
			       sizeof(final), &info);
	} while (err == 0 && (hdr.src != tag_id || final.round != round));

	if (err) {
		responder_stats.final_timeout++;
		return;
	}
	final_rx = info.rx_ts;
	responder_stats.finals++;

	report.reply_b = (uint32_t)rtls_dw_ts_diff(resp_tx, poll_rx);
	report.round_b = (uint32_t)rtls_dw_ts_diff(final_rx, resp_tx);
	report.fp_power_cdbm = info.fp_power_cdbm;
	for (int k = 0; k < 3; k++) {
		report.pos_mm[k] = responder_cfg.pos_mm[k];
	}

	opts.delayed_ts = rtls_twr_slot_ts(final_rx, TWR_REPLY_US, TWR_SLOT_US, slot);
	opts.rx_after_tx = false;
	err = twr_send(responder_cfg.pan_id, responder_cfg.anchor_id, tag_id, &report,
		       sizeof(report), &opts, NULL);
	if (err == -ETIME) {
		responder_stats.late_tx++;
	} else if (err == 0) {
		responder_stats.reports_sent++;
	}
}

int rtls_twr_responder_run(void)
{
	const k_timepoint_t forever = sys_timepoint_calc(K_FOREVER);
	struct rtls_msg_poll poll;
	struct dw1000_rx_info info;
	struct rtls_mac_hdr hdr;
	int slot;

	while (true) {
		if (twr_recv(responder_cfg.pan_id, RTLS_MSG_POLL, forever, &hdr, &poll, sizeof(poll),
			     &info) != 0) {
			continue;
		}

		slot = slot_of(&poll);
		if (slot < 0) {
			continue;
		}

		responder_stats.polls++;
		respond(hdr.src, poll.round, (uint8_t)slot,
			MIN(poll.n_anchors, RTLS_MAX_ANCHORS_PER_ROUND), info.rx_ts);
		dw1000_idle();
	}

	return 0;
}

void rtls_twr_responder_get_stats(struct rtls_twr_responder_stats *stats, bool reset)
{
	*stats = responder_stats;
	if (reset) {
		memset(&responder_stats, 0, sizeof(responder_stats));
	}
}
