/*
 * DS-TWR initiator (tag): POLL em broadcast, RESP de cada âncora no seu
 * slot, FINAL em broadcast e REPORT de cada âncora com os intervalos medidos
 * no relógio dela. A tag calcula todas as distâncias
 * (docs/03-protocolo-uwb.md, seções 3 e 4).
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

/* Distâncias fora desta faixa indicam medida corrompida (ex.: REPORT trocado). */
#define RANGE_MIN_M (-1.0f)
#define RANGE_MAX_M 300.0f

struct resp_slot {
	bool ok;
	uint64_t rx_ts;
	int16_t rx_power_cdbm;
	int16_t fp_power_cdbm;
};

static struct rtls_twr_initiator_cfg initiator_cfg;
static struct rtls_twr_initiator_stats initiator_stats;
static uint8_t round_seq;

/* Viés dependente do RSL (APS011), docs/04-algoritmos.md seção 3.1. */
static float correct_bias(float distance_m)
{
#if defined(CONFIG_RTLS_TWR_RANGE_BIAS_CORRECTION)
	return rtls_twr_correct_range(distance_m, CONFIG_RTLS_TWR_RANGE_BIAS_GAIN_DB_X10 / 10.0f);
#else
	return distance_m;
#endif
}

static int anchor_index(uint16_t anchor_id)
{
	for (int i = 0; i < initiator_cfg.n_anchors; i++) {
		if (initiator_cfg.anchors[i] == anchor_id) {
			return i;
		}
	}
	return -1;
}

int rtls_twr_initiator_init(const struct rtls_twr_initiator_cfg *cfg)
{
	if (cfg == NULL || cfg->n_anchors == 0 ||
	    cfg->n_anchors > RTLS_MAX_ANCHORS_PER_ROUND) {
		return -EINVAL;
	}

	initiator_cfg = *cfg;
	round_seq = 0;
	memset(&initiator_stats, 0, sizeof(initiator_stats));
	return 0;
}

/*
 * Recebe os RESPs (um por âncora) até todos chegarem ou a janela fechar. A
 * janela termina no meio do slot do FINAL: se uma âncora faltar, ainda sobra
 * meio slot para programar o FINAL a tempo.
 */
static int collect_responses(struct resp_slot resp[], uint8_t n)
{
	const k_timepoint_t deadline = sys_timepoint_calc(
		K_USEC(TWR_REPLY_US + (n - 1) * TWR_SLOT_US + TWR_SLOT_US / 2));
	struct rtls_msg_resp msg;
	struct rtls_mac_hdr hdr;
	struct dw1000_rx_info info;
	int received = 0;
	int idx;

	while (received < n) {
		if (twr_recv(initiator_cfg.pan_id, RTLS_MSG_RESP, deadline, &hdr, &msg, sizeof(msg),
			     &info) != 0) {
			break;
		}

		idx = anchor_index(hdr.src);
		if (hdr.dst != initiator_cfg.tag_id || msg.round != round_seq || idx < 0 ||
		    resp[idx].ok) {
			continue;
		}

		resp[idx].ok = true;
		resp[idx].rx_ts = info.rx_ts;
		resp[idx].rx_power_cdbm = info.rx_power_cdbm;
		resp[idx].fp_power_cdbm = info.fp_power_cdbm;
		initiator_stats.anchors[idx].resp_ok++;
		received++;
	}

	return received;
}

/* Recebe os REPORTs e calcula a distância de cada âncora que respondeu. */
static void collect_reports(struct resp_slot resp[], uint8_t n, int expected, uint64_t poll_tx,
			    uint64_t final_tx, struct rtls_range_set *out)
{
	const k_timepoint_t deadline =
		sys_timepoint_calc(K_USEC(TWR_REPLY_US + n * TWR_SLOT_US + TWR_MARGIN_US));
	struct rtls_twr_intervals iv;
	struct rtls_msg_report msg;
	struct rtls_mac_hdr hdr;
	struct rtls_range *range;
	float distance;
	int idx;

	for (int got = 0; got < expected;) {
		if (twr_recv(initiator_cfg.pan_id, RTLS_MSG_REPORT, deadline, &hdr, &msg,
			     sizeof(msg), NULL) != 0) {
			break;
		}

		idx = anchor_index(hdr.src);
		if (hdr.dst != initiator_cfg.tag_id || msg.round != round_seq || idx < 0 ||
		    !resp[idx].ok) {
			continue;
		}
		resp[idx].ok = false; /* consome: ignora REPORT duplicado */
		got++;

		rtls_twr_tag_intervals(&iv, poll_tx, resp[idx].rx_ts, final_tx, msg.reply_b,
				       msg.round_b);
		distance = (float)rtls_twr_tof_to_m(rtls_twr_ds_tof(&iv));
		if (distance < RANGE_MIN_M || distance > RANGE_MAX_M) {
			initiator_stats.anchors[idx].invalid++;
			continue;
		}

		range = &out->ranges[out->count++];
		range->anchor_id = hdr.src;
		range->distance_raw_m = distance;
		range->distance_m = correct_bias(distance);
		range->rx_power_cdbm = resp[idx].rx_power_cdbm;
		range->resp_fp_power_cdbm = resp[idx].fp_power_cdbm;
		range->fp_power_cdbm = msg.fp_power_cdbm;
		for (int k = 0; k < 3; k++) {
			range->anchor_pos_mm[k] = msg.pos_mm[k];
		}
		initiator_stats.anchors[idx].range_ok++;
	}
}

int rtls_twr_initiator_run_round(struct rtls_range_set *out)
{
	const uint8_t n = initiator_cfg.n_anchors;
	const uint32_t start_cycles = k_cycle_get_32();
	struct resp_slot resp[RTLS_MAX_ANCHORS_PER_ROUND] = {0};
	struct dw1000_tx_opts opts = {
		.rx_after_tx = true,
		.rx_delay_uus = rtls_us_to_uus(TWR_REPLY_US - MIN(TWR_REPLY_US, TWR_RX_GUARD_US)),
	};
	struct rtls_msg_poll poll = {
		.type = RTLS_MSG_POLL,
		.round = round_seq,
		.n_anchors = n,
	};
	const struct rtls_msg_final final = {
		.type = RTLS_MSG_FINAL,
		.round = round_seq,
	};
	uint64_t poll_tx, final_tx;
	uint32_t elapsed_us;
	int received;
	int err;

	if (out == NULL || n == 0) {
		return -EINVAL;
	}

	out->round = round_seq;
	out->count = 0;
	out->t_ms = k_uptime_get();
	initiator_stats.rounds++;

	for (int i = 0; i < n; i++) {
		poll.anchors[i] = initiator_cfg.anchors[i];
	}

	err = twr_send(initiator_cfg.pan_id, initiator_cfg.tag_id, RTLS_ADDR_BROADCAST, &poll,
		       sizeof(poll), &opts, &poll_tx);
	if (err) {
		goto done;
	}

	received = collect_responses(resp, n);
	if (received == 0) {
		err = -ENODATA;
		goto done;
	}

	/* FINAL no slot seguinte ao da última âncora, medido a partir do POLL. */
	opts.delayed_ts = rtls_twr_slot_ts(poll_tx, TWR_REPLY_US, TWR_SLOT_US, n);
	err = twr_send(initiator_cfg.pan_id, initiator_cfg.tag_id, RTLS_ADDR_BROADCAST, &final,
		       sizeof(final), &opts, &final_tx);
	if (err) {
		if (err == -ETIME) {
			initiator_stats.late_tx++;
		}
		goto done;
	}

	collect_reports(resp, n, received, poll_tx, final_tx, out);
	err = (out->count > 0) ? 0 : -ENODATA;
	if (out->count == n) {
		initiator_stats.rounds_complete++;
	}

done:
	dw1000_idle();
	round_seq++;

	elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - start_cycles);
	initiator_stats.last_round_us = elapsed_us;
	initiator_stats.max_round_us = MAX(initiator_stats.max_round_us, elapsed_us);
	return err;
}

void rtls_twr_initiator_get_stats(struct rtls_twr_initiator_stats *stats, bool reset)
{
	*stats = initiator_stats;
	if (reset) {
		memset(&initiator_stats, 0, sizeof(initiator_stats));
	}
}
