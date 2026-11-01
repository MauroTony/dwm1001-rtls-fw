/*
 * Firmware da TAG.
 *
 * A cada ciclo mede a distância até as âncoras por DS-TWR, estima a posição
 * com o EKF e envia o resultado ao gateway. Fluxo: docs/02-arquitetura.md,
 * seção 4.1. O ciclo é próprio (CONFIG_RTLS_TAG_UPDATE_RATE_HZ) ou, com
 * CONFIG_RTLS_TAG_TDMA, o slot da tag no superquadro do gateway (M7).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/loc.h>
#include <rtls/status_led.h>
#include <rtls/tdma.h>
#include <rtls/twr.h>
#include <rtls/twr_math.h>

LOG_MODULE_REGISTER(tag, LOG_LEVEL_INF);

#define CYCLE_MS (1000 / CONFIG_RTLS_TAG_UPDATE_RATE_HZ)

/* TDMA: o RX abre esse tanto antes do BEACON previsto e fecha esse tanto depois. */
#define TDMA_BEACON_GUARD_US 1000
/* TDMA: sem sincronismo, escuta em janelas desse tamanho (entre elas, o resumo). */
#define TDMA_SEARCH_US 1000000

static struct rtls_loc_ctx loc;
static uint8_t mac_seq;
static int64_t next_stats_ms;

/* Sincronismo com o superquadro do gateway (CONFIG_RTLS_TAG_TDMA). */
static struct rtls_tdma_sync tdma;
static struct {
	uint32_t beacons;
	uint32_t missed;
	uint32_t lost_sync; /**< vezes que parou de transmitir */
	uint32_t late_slot; /**< o slot já tinha passado quando foi calculado */
} tdma_stats;

/* Tempo de rtls_loc_update() desde o último resumo (T4.9). */
static uint32_t loc_last_us;
static uint32_t loc_max_us;

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

		printk("R,%u,%u,%04x,%d,%d,%d,%d\n", set->round, (uint32_t)set->t_ms, r->anchor_id,
		       m_to_mm(r->distance_m), r->rx_power_cdbm, r->fp_power_cdbm,
		       m_to_mm(r->distance_raw_m));
	}
}

/* Uma linha CSV por posição estimada (mm; flags = RTLS_POS_FLAG_*). */
static void print_position(const struct rtls_position *pos, uint8_t round)
{
	if (!IS_ENABLED(CONFIG_RTLS_TAG_CSV_OUTPUT)) {
		return;
	}

	printk("L,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u\n", round, (uint32_t)pos->t_ms,
	       m_to_mm(pos->pos_m[0]), m_to_mm(pos->pos_m[1]), m_to_mm(pos->pos_m[2]),
	       std_to_mm(pos->std_m[0]), std_to_mm(pos->std_m[1]), std_to_mm(pos->std_m[2]),
	       pos->n_ranges, pos->flags);
}

/* rtls_loc_update() com o tempo de execução medido pela API de timing. */
static int timed_loc_update(const struct rtls_range_set *set, struct rtls_position *pos)
{
	timing_t t0, t1;
	int err;

	t0 = timing_counter_get();
	err = rtls_loc_update(&loc, set, pos);
	t1 = timing_counter_get();

	loc_last_us = (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&t0, &t1)) / 1000U);
	loc_max_us = MAX(loc_max_us, loc_last_us);
	return err;
}

static void log_stats(void)
{
	struct rtls_twr_initiator_stats st;
	struct rtls_loc_stats ls;
	uint32_t pct;

	if (IS_ENABLED(CONFIG_RTLS_TAG_TDMA)) {
		LOG_INF("TDMA: %s, %u BEACONs, %u perdidos, %u vezes sem sincronismo, "
			"%u slots atrasados",
			tdma.synced ? "sincronizado" : "SEM SINCRONISMO", tdma_stats.beacons,
			tdma_stats.missed, tdma_stats.lost_sync, tdma_stats.late_slot);
		memset(&tdma_stats, 0, sizeof(tdma_stats));
	}

	rtls_twr_initiator_get_stats(&st, true);
	rtls_loc_get_stats(&loc, &ls, true);
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
	LOG_INF("LOC: %u posicoes, %u aceitas, %u rejeitadas, %u reinicios, "
		"EKF ultimo %u us, max %u us",
		ls.updates, ls.accepted, ls.rejected, ls.resets, loc_last_us, loc_max_us);
	loc_max_us = 0;
}

static void maybe_log_stats(void)
{
	if (CONFIG_RTLS_TAG_STATS_PERIOD_S > 0 && k_uptime_get() >= next_stats_ms) {
		log_stats();
		next_stats_ms += CONFIG_RTLS_TAG_STATS_PERIOD_S * 1000;
	}
}

/* Uma rodada completa: DS-TWR, CSV, EKF e POSITION. */
static void run_cycle(void)
{
	struct rtls_range_set ranges;
	struct rtls_position pos;

	if (rtls_twr_initiator_run_round(&ranges) != 0) {
		return;
	}

	rtls_status_led_activity();
	print_ranges(&ranges);

	/* -EAGAIN: sem posição até ter distâncias para a multilateração */
	if (timed_loc_update(&ranges, &pos) == 0) {
		print_position(&pos, ranges.round);
		(void)send_position(&pos, ranges.round);
	}
}

/* Ciclo próprio a CONFIG_RTLS_TAG_UPDATE_RATE_HZ, sem gateway (M3-M6). */
static void free_loop(void)
{
	int64_t next_ms = k_uptime_get();

	while (true) {
		next_ms += CYCLE_MS;
		run_cycle();
		maybe_log_stats();
		k_sleep(K_TIMEOUT_ABS_MS(next_ms));
	}
}

static int64_t now_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static void sleep_until_us(int64_t t_us)
{
	k_sleep(K_TIMEOUT_ABS_TICKS(k_us_to_ticks_ceil64(MAX(t_us, 0))));
}

/*
 * Instante local (µs) em que o quadro começou a chegar: o RX_TIME do DW1000
 * comparado com o relógio dele agora. Tira a latência ISR -> thread da
 * medida; sobra a resolução do tick do kernel (~30 µs).
 */
static int64_t frame_start_us(const struct dw1000_rx_info *info)
{
	const uint64_t age = rtls_dw_ts_diff(dw1000_sys_time(), info->rx_ts);

	return now_us() - (int64_t)rtls_dw_ticks_to_us(age);
}

/* Escuta até @p until_us; true se um BEACON do gateway (re)sincronizou a tag. */
static bool wait_beacon(int64_t until_us)
{
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	struct dw1000_rx_info info;
	struct rtls_msg_beacon msg;
	struct rtls_mac_hdr hdr;
	const uint8_t *payload;
	size_t payload_len;

	while (now_us() < until_us) {
		const int len = dw1000_receive(frame, sizeof(frame), &info,
					       K_TIMEOUT_ABS_TICKS(k_us_to_ticks_ceil64(until_us)));

		if (len == -EAGAIN) {
			break;
		}
		/* POLL e FINAL de outras tags também chegam (broadcast): ignora. */
		if (len <= 0 ||
		    rtls_frame_parse(frame, (size_t)len, CONFIG_RTLS_PAN_ID, &hdr, &payload,
				     &payload_len) != RTLS_MSG_BEACON ||
		    hdr.src != RTLS_ADDR_GATEWAY) {
			continue;
		}

		memcpy(&msg, payload, sizeof(msg));
		if (rtls_tdma_on_beacon(&tdma, &msg, frame_start_us(&info)) == 0) {
			tdma_stats.beacons++;
			return true;
		}
	}

	return false;
}

/*
 * TDMA (M7): a rodada só sai no slot k = RTLS_NODE_ID - 0x1001 do superquadro
 * anunciado pelo BEACON. Sem BEACON por RTLS_TDMA_MAX_MISSED superquadros, a
 * tag para de transmitir e fica escutando até sincronizar de novo.
 */
static void tdma_loop(void)
{
	const int slot = rtls_tdma_slot_of(CONFIG_RTLS_NODE_ID);
	bool range_warned = false;

	LOG_INF("TDMA: slot %d; procurando o BEACON do gateway", slot);
	while (true) {
		int64_t t_us;

		if (!tdma.synced) {
			/* Sem sincronismo a tag não transmite: só escuta. */
			if (wait_beacon(now_us() + TDMA_SEARCH_US)) {
				LOG_INF("TDMA: sincronizado no superquadro %u (%u slots de %u us)",
					tdma.superframe, tdma.n_slots, tdma.slot_us);
			}
			maybe_log_stats();
			continue;
		}

		if (rtls_tdma_slot_start(&tdma, slot, &t_us) == -ERANGE) {
			if (!range_warned) {
				LOG_ERR("TDMA: slot %d fora do superquadro (%u slots); a tag nao "
					"transmite",
					slot, tdma.n_slots);
				range_warned = true;
			}
		} else if (t_us < now_us()) {
			tdma_stats.late_slot++;
		} else {
			sleep_until_us(t_us);
			run_cycle();
		}
		maybe_log_stats();

		/* Próximo BEACON: a janela de RX abre um pouco antes do previsto. */
		t_us = rtls_tdma_next_beacon(&tdma);
		sleep_until_us(t_us - TDMA_BEACON_GUARD_US);
		if (!wait_beacon(t_us + TDMA_BEACON_GUARD_US)) {
			tdma_stats.missed++;
			rtls_tdma_on_missed(&tdma);
			if (!tdma.synced) {
				tdma_stats.lost_sync++;
				LOG_WRN("TDMA: %d BEACONs perdidos seguidos; a tag para de transmitir",
					RTLS_TDMA_MAX_MISSED);
			}
		}
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
	int err;

	if (IS_ENABLED(CONFIG_RTLS_TAG_TDMA)) {
		LOG_INF("RTLS tag 0x%04x: TDMA (slot do BEACON), %d ancoras a partir de 0x%04x",
			CONFIG_RTLS_NODE_ID, CONFIG_RTLS_TAG_NUM_ANCHORS,
			CONFIG_RTLS_TAG_ANCHOR_BASE_ID);
	} else {
		LOG_INF("RTLS tag 0x%04x: %d Hz, %d ancoras a partir de 0x%04x",
			CONFIG_RTLS_NODE_ID, CONFIG_RTLS_TAG_UPDATE_RATE_HZ,
			CONFIG_RTLS_TAG_NUM_ANCHORS, CONFIG_RTLS_TAG_ANCHOR_BASE_ID);
	}

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

	timing_init();
	timing_start();
	LOG_INF("LOC %dD: sigma_r %d mm, sigma_a %d mm/s2, gating %d/100, z fixo (2D) %d mm",
		loc_cfg.dim, CONFIG_RTLS_LOC_RANGE_STD_MM, CONFIG_RTLS_LOC_ACCEL_STD_MMS2,
		CONFIG_RTLS_LOC_GATE_CHI2_X100, CONFIG_RTLS_LOC_TAG_HEIGHT_MM);

	next_stats_ms = k_uptime_get() + CONFIG_RTLS_TAG_STATS_PERIOD_S * 1000;
	if (IS_ENABLED(CONFIG_RTLS_TAG_TDMA)) {
		tdma_loop();
	} else {
		free_loop();
	}

	return 0;
}
