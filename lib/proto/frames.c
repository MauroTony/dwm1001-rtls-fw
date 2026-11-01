#include <errno.h>
#include <string.h>

#include <rtls/frames.h>

size_t rtls_msg_size(uint8_t type)
{
	switch (type) {
	case RTLS_MSG_BEACON:
		return sizeof(struct rtls_msg_beacon);
	case RTLS_MSG_POLL:
		return sizeof(struct rtls_msg_poll);
	case RTLS_MSG_RESP:
		return sizeof(struct rtls_msg_resp);
	case RTLS_MSG_FINAL:
		return sizeof(struct rtls_msg_final);
	case RTLS_MSG_REPORT:
		return sizeof(struct rtls_msg_report);
	case RTLS_MSG_POSITION:
		return sizeof(struct rtls_msg_position);
	case RTLS_MSG_PING:
		return sizeof(struct rtls_msg_ping);
	default:
		return 0;
	}
}

int rtls_frame_build(uint8_t *buf, size_t size, const struct rtls_mac_hdr *hdr,
		     const void *payload, size_t payload_len)
{
	const size_t len = sizeof(*hdr) + payload_len;

	if (buf == NULL || hdr == NULL || payload == NULL || payload_len == 0) {
		return -EINVAL;
	}
	if (len + RTLS_FCS_LEN > RTLS_FRAME_MAX_LEN || len > size) {
		return -ENOMEM;
	}

	memcpy(buf, hdr, sizeof(*hdr));
	memcpy(buf + sizeof(*hdr), payload, payload_len);
	return (int)len;
}

int rtls_frame_parse(const uint8_t *frame, size_t len, uint16_t pan_id,
		     struct rtls_mac_hdr *hdr, const uint8_t **payload, size_t *payload_len)
{
	size_t min_len;

	if (frame == NULL || hdr == NULL || payload == NULL || payload_len == NULL ||
	    len <= sizeof(*hdr)) {
		return -EBADMSG;
	}

	memcpy(hdr, frame, sizeof(*hdr));
	if (hdr->fc != RTLS_FC_DATA_SHORT || hdr->pan_id != pan_id) {
		return -EBADMSG;
	}

	*payload = frame + sizeof(*hdr);
	*payload_len = len - sizeof(*hdr);

	min_len = rtls_msg_size((*payload)[0]);
	if (min_len == 0 || *payload_len < min_len) {
		return -EBADMSG;
	}

	return (*payload)[0];
}

int rtls_round_tracker_update(struct rtls_round_tracker *trk, uint8_t round, uint32_t t_ms)
{
	int lost = 0;

	if (trk->valid) {
		if (t_ms == trk->last_t_ms) {
			return -EALREADY;
		}
		/* t_ms voltou: a tag reiniciou e a sequência recomeça */
		if (t_ms > trk->last_t_ms) {
			lost = (uint8_t)(round - trk->last_round - 1U);
		}
	}

	trk->valid = true;
	trk->last_round = round;
	trk->last_t_ms = t_ms;
	return lost;
}
