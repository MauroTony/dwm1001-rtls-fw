/*
 * Testes do codec de quadros (docs/03-protocolo-uwb.md, seção 2).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <rtls/frames.h>

#define PAN 0xDECA

ZTEST(frames, test_build_and_parse_position)
{
	const struct rtls_msg_position msg = {
		.type = RTLS_MSG_POSITION,
		.round = 42,
		.n_ranges = 4,
		.flags = RTLS_POS_FLAG_VALID,
		.t_ms = 123456,
		.pos_mm = {1500, -250, 1000},
		.std_mm = {30, 35, 0},
	};
	struct rtls_mac_hdr hdr;
	struct rtls_mac_hdr parsed;
	struct rtls_msg_position out;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	const uint8_t *payload;
	size_t payload_len;
	int len;

	rtls_mac_hdr_init(&hdr, 7, PAN, RTLS_ADDR_GATEWAY, 0x1001);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, &msg, sizeof(msg));
	zassert_equal(len, 9 + 26);

	/* Frame Control no ar: 0x41 0x88. */
	zassert_equal(frame[0], 0x41);
	zassert_equal(frame[1], 0x88);

	zassert_equal(rtls_frame_parse(frame, len, PAN, &parsed, &payload, &payload_len),
		      RTLS_MSG_POSITION);
	zassert_equal(parsed.src, 0x1001);
	zassert_equal(parsed.dst, RTLS_ADDR_GATEWAY);
	zassert_equal(parsed.seq, 7);
	zassert_equal(payload_len, sizeof(out));

	memcpy(&out, payload, sizeof(out));
	zassert_mem_equal(&out, &msg, sizeof(msg));
}

ZTEST(frames, test_parse_rejects_wrong_pan_and_short_payload)
{
	const struct rtls_msg_report report = {.type = RTLS_MSG_REPORT};
	struct rtls_mac_hdr hdr;
	struct rtls_mac_hdr parsed;
	uint8_t frame[RTLS_FRAME_MAX_LEN];
	const uint8_t *payload;
	size_t payload_len;
	int len;

	rtls_mac_hdr_init(&hdr, 0, PAN, 0x1001, 0xA001);
	len = rtls_frame_build(frame, sizeof(frame), &hdr, &report, sizeof(report));
	zassert_true(len > 0);

	zassert_equal(rtls_frame_parse(frame, len, 0x1234, &parsed, &payload, &payload_len),
		      -EBADMSG, "PAN diferente deve ser rejeitado");
	zassert_equal(rtls_frame_parse(frame, len - 1, PAN, &parsed, &payload, &payload_len),
		      -EBADMSG, "payload menor que o tipo deve ser rejeitado");
}

ZTEST(frames, test_build_rejects_oversized_frame)
{
	uint8_t big[RTLS_FRAME_MAX_LEN] = {RTLS_MSG_POSITION};
	struct rtls_mac_hdr hdr;
	uint8_t frame[RTLS_FRAME_MAX_LEN];

	rtls_mac_hdr_init(&hdr, 0, PAN, RTLS_ADDR_BROADCAST, 0x1001);
	zassert_equal(rtls_frame_build(frame, sizeof(frame), &hdr, big, sizeof(big)), -ENOMEM);
}

ZTEST(frames, test_message_sizes)
{
	zassert_equal(rtls_msg_size(RTLS_MSG_POLL), 11);
	zassert_equal(rtls_msg_size(RTLS_MSG_REPORT), 24);
	zassert_equal(rtls_msg_size(RTLS_MSG_POSITION), 26);
	zassert_equal(rtls_msg_size(RTLS_MSG_PING), 5);
	zassert_equal(rtls_msg_size(0x99), 0);

	/* O maior quadro (POSITION) cabe com folga no limite de 127 B. */
	zassert_true(sizeof(struct rtls_mac_hdr) + sizeof(struct rtls_msg_position) +
			     RTLS_FCS_LEN <= RTLS_FRAME_MAX_LEN);
}

/* T5.0: lacunas de round no gateway */
ZTEST(frames, test_round_tracker)
{
	struct rtls_round_tracker trk = {0};

	/* primeiro quadro e consecutivos: sem perda */
	zassert_equal(rtls_round_tracker_update(&trk, 10, 1000), 0);
	zassert_equal(rtls_round_tracker_update(&trk, 11, 1100), 0);

	/* rodadas 12 e 13 sem POSITION */
	zassert_equal(rtls_round_tracker_update(&trk, 14, 1400), 2);

	/* repetido: ignorado, sem mexer na sequência */
	zassert_equal(rtls_round_tracker_update(&trk, 14, 1400), -EALREADY);
	zassert_equal(rtls_round_tracker_update(&trk, 15, 1500), 0);

	/* volta do contador de 8 bits: 254 -> 1 perde 255 e 0 */
	trk = (struct rtls_round_tracker){0};
	zassert_equal(rtls_round_tracker_update(&trk, 254, 50000), 0);
	zassert_equal(rtls_round_tracker_update(&trk, 1, 50300), 2);

	/* tag reiniciou (t_ms voltou): recomeça sem contar perdas */
	zassert_equal(rtls_round_tracker_update(&trk, 0, 120), 0);
	zassert_equal(rtls_round_tracker_update(&trk, 1, 220), 0);
}

ZTEST_SUITE(frames, NULL, NULL, NULL, NULL, NULL);
