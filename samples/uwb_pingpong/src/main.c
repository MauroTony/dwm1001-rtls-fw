/*
 * Bancada do M1: ping-pong UWB entre duas placas.
 *
 * - ping: a cada PINGPONG_PERIOD_MS envia um PING (seq) com RX automático
 *   depois do TX e espera o PONG com o mesmo seq. Mede a taxa de perda (PER)
 *   e a potência recebida.
 * - pong: responde cada PING com TX atrasado em rx_ts + PINGPONG_REPLY_US,
 *   exercitando o mesmo caminho que as âncoras usam no DS-TWR.
 *
 * Critério do M1: PER < 1 % a 3 m por 10 min e nenhum TX atrasado perdido
 * (docs/07-bancada.md).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/dw1000.h>
#include <rtls/frames.h>
#include <rtls/status_led.h>
#include <rtls/twr_math.h>

LOG_MODULE_REGISTER(pingpong, LOG_LEVEL_INF);

#define PAN_ID    0xDECA
#define ADDR_PING 0x0F01
#define ADDR_PONG 0x0F02

#if defined(CONFIG_PINGPONG_ROLE_PING)
#define MY_ADDR   ADDR_PING
#define PEER_ADDR ADDR_PONG
#else
#define MY_ADDR   ADDR_PONG
#define PEER_ADDR ADDR_PING
#endif

/* Espera do pong: atraso programado + tempo no ar + folga. */
#define PONG_TIMEOUT K_USEC(CONFIG_PINGPONG_REPLY_US + 3000)

static uint8_t mac_seq;

static int send_ping(uint32_t seq, const struct dw1000_tx_opts *opts)
{
	const struct rtls_msg_ping msg = {.type = RTLS_MSG_PING, .seq = seq};
	struct rtls_mac_hdr hdr;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	int len;

	rtls_mac_hdr_init(&hdr, mac_seq++, PAN_ID, PEER_ADDR, MY_ADDR);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, &msg, sizeof(msg));
	if (len < 0) {
		return len;
	}
	return dw1000_transmit(frame, len, opts, NULL);
}

/* Recebe um PING do par; devolve o seq ou erro negativo. */
static int64_t recv_ping(k_timeout_t timeout, struct dw1000_rx_info *info)
{
	const k_timepoint_t deadline = sys_timepoint_calc(timeout);
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	struct rtls_msg_ping msg;
	struct rtls_mac_hdr hdr;
	const uint8_t *payload;
	size_t payload_len;
	int len;

	while (true) {
		len = dw1000_receive(frame, sizeof(frame), info, sys_timepoint_timeout(deadline));
		if (len < 0) {
			if (len == -EAGAIN || sys_timepoint_expired(deadline)) {
				return -EAGAIN;
			}
			continue;
		}
		if (rtls_frame_parse(frame, len, PAN_ID, &hdr, &payload, &payload_len) ==
			    RTLS_MSG_PING &&
		    hdr.src == PEER_ADDR) {
			memcpy(&msg, payload, sizeof(msg));
			return msg.seq;
		}
	}
}

#if defined(CONFIG_PINGPONG_ROLE_PING)

static void run(void)
{
	const struct dw1000_tx_opts opts = {
		.rx_after_tx = true,
		.rx_delay_uus = rtls_us_to_uus(CONFIG_PINGPONG_REPLY_US / 2),
	};
	struct dw1000_rx_info info;
	uint32_t sent = 0, received = 0, tx_errors = 0;
	int32_t power_sum = 0;
	int64_t seq;
	int err;

	while (true) {
		err = send_ping(sent, &opts);
		if (err) {
			tx_errors++;
		} else {
			seq = recv_ping(PONG_TIMEOUT, &info);
			if (seq == (int64_t)sent) {
				received++;
				power_sum += info.rx_power_cdbm;
				rtls_status_led_activity();
			}
		}
		sent++;

		if (sent % CONFIG_PINGPONG_REPORT_EVERY == 0) {
			LOG_INF("ping: enviados %u, pongs %u, PER %u.%02u%%, RX medio %d cdBm, "
				"erros de TX %u",
				sent, received, 100U * (sent - received) / sent,
				(10000U * (sent - received) / sent) % 100,
				received ? power_sum / (int32_t)received : 0, tx_errors);
		}

		dw1000_idle();
		k_sleep(K_MSEC(CONFIG_PINGPONG_PERIOD_MS));
	}
}

#else /* CONFIG_PINGPONG_ROLE_PONG */

static void run(void)
{
	struct dw1000_tx_opts opts = {0};
	struct dw1000_rx_info info;
	struct dw1000_stats st;
	uint32_t answered = 0;
	int64_t seq;

	while (true) {
		seq = recv_ping(K_FOREVER, &info);
		if (seq < 0) {
			continue;
		}

		opts.delayed_ts = rtls_twr_slot_ts(info.rx_ts, CONFIG_PINGPONG_REPLY_US, 0, 0);
		if (send_ping((uint32_t)seq, &opts) == 0) {
			answered++;
			rtls_status_led_activity();
		}

		if (answered > 0 && answered % CONFIG_PINGPONG_REPORT_EVERY == 0) {
			dw1000_get_stats(&st);
			LOG_INF("pong: respondidos %u, TX atrasado perdido %u, RX ultimo %d cdBm "
				"(1o caminho %d)",
				answered, st.tx_late, info.rx_power_cdbm, info.fp_power_cdbm);
		}
	}
}

#endif

int main(void)
{
	const struct dw1000_phy_config phy = DW1000_PHY_CONFIG_DEFAULT(PAN_ID, MY_ADDR);
	int err;

	LOG_INF("UWB ping-pong: %s (0x%04x)",
		IS_ENABLED(CONFIG_PINGPONG_ROLE_PING) ? "ping" : "pong", MY_ADDR);

	(void)rtls_status_led_init();

	err = dw1000_init();
	if (err == 0) {
		err = dw1000_configure(&phy);
	}
	if (err) {
		LOG_ERR("DW1000 indisponivel (%d)", err);
		rtls_status_led_error(true);
		return 0;
	}

	run();
	return 0;
}
