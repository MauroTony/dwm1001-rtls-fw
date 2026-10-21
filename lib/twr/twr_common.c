#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "twr_internal.h"

LOG_MODULE_REGISTER(rtls_twr, CONFIG_RTLS_TWR_LOG_LEVEL);

static uint8_t mac_seq;

int twr_send(uint16_t pan_id, uint16_t src, uint16_t dst, const void *msg, size_t msg_len,
	     const struct dw1000_tx_opts *opts, uint64_t *tx_ts)
{
	struct rtls_mac_hdr hdr;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	int len;

	rtls_mac_hdr_init(&hdr, mac_seq++, pan_id, dst, src);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, msg, msg_len);
	if (len < 0) {
		return len;
	}

	return dw1000_transmit(frame, (size_t)len, opts, tx_ts);
}

int twr_recv(uint16_t pan_id, uint8_t type, k_timepoint_t deadline, struct rtls_mac_hdr *hdr,
	     void *msg, size_t msg_size, struct dw1000_rx_info *info)
{
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	const uint8_t *payload;
	size_t payload_len;
	int len;

	while (true) {
		len = dw1000_receive(frame, sizeof(frame), info, sys_timepoint_timeout(deadline));
		if (len == -EAGAIN) {
			return -EAGAIN;
		}
		if (len < 0) {
			if (sys_timepoint_expired(deadline)) {
				return -EAGAIN;
			}
			continue;
		}

		if (rtls_frame_parse(frame, (size_t)len, pan_id, hdr, &payload, &payload_len) !=
		    type) {
			LOG_DBG("quadro descartado (%d B, esperado tipo 0x%02x)", len, type);
			continue;
		}

		memcpy(msg, payload, MIN(payload_len, msg_size));
		return 0;
	}
}
