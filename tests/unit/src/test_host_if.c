/*
 * Testes do codec do protocolo SPI com o host (docs/05-interface-spi-gateway.md,
 * seções 2 e 3): T6.0 em docs/08, seção 9.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <rtls/frames.h>
#include <rtls/host_if.h>

static const struct rtls_host_if_info info = {
	.fw_version = {0, 6, 1},
	.gateway_id = 0x0001,
	.capacity = 16,
	.superframe_ms = 100,
};

static struct rtls_tag_record records[20];
static uint8_t rsp[RTLS_HOST_IF_MAX_XFER];

static void fill_records(void)
{
	for (int i = 0; i < ARRAY_SIZE(records); i++) {
		records[i] = (struct rtls_tag_record){
			.tag_id = 0x1001 + i,
			.round = (uint8_t)(10 * i),
			.n_ranges = 4,
			.flags = RTLS_POS_FLAG_VALID,
			.rssi_dbm = -80,
			.age_ms = (uint16_t)(5 * i),
			.pos_mm = {1000 * i, -250 * i, 1000},
			.std_mm = {30, 35, 0},
		};
	}
}

static struct rtls_host_if_view view_of(size_t count, uint8_t snapshot)
{
	return (struct rtls_host_if_view){
		.info = &info,
		.records = records,
		.count = count,
		.snapshot = snapshot,
	};
}

/* Request de TAGS_GET como o host ESP32 manda: TLV + 0xFF até 16 bytes. */
static size_t tags_get(const struct rtls_host_if_view *view, uint8_t page,
		       struct rtls_host_if_event *ev)
{
	uint8_t req[16];

	memset(req, 0xFF, sizeof(req));
	req[0] = RTLS_HOST_IF_REQ_TAGS_GET;
	req[1] = 1;
	req[2] = page;
	return rtls_host_if_handle(req, sizeof(req), view, rsp, ev);
}

/* Confere envelope, TLV 0xB1, cabeçalho da página, registros e CRC. */
static void check_page(size_t rsp_len, uint8_t page, uint8_t n_pages, uint8_t count,
		       uint8_t snapshot, size_t first)
{
	const uint8_t *v = &rsp[5];
	const size_t value_len = 4 + 26 * count + 2;

	zassert_equal(rsp_len, 5 + value_len);
	zassert_equal(rsp[0], RTLS_HOST_IF_RSP_ENVELOPE);
	zassert_equal(rsp[1], 1);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_OK);
	zassert_equal(rsp[3], RTLS_HOST_IF_RSP_TAGS);
	zassert_equal(rsp[4], value_len);
	zassert_equal(v[0], page);
	zassert_equal(v[1], n_pages);
	zassert_equal(v[2], count);
	zassert_equal(v[3], snapshot);
	zassert_mem_equal(&v[4], &records[first], 26 * count);
	zassert_equal(sys_get_le16(&v[value_len - 2]), rtls_host_if_crc(v, value_len - 2));
}

/* T6.0 */
ZTEST(host_if, test_crc_known_vector)
{
	/* CRC-16/CCITT-FALSE do texto padrão "123456789" */
	zassert_equal(rtls_host_if_crc((const uint8_t *)"123456789", 9), 0x29B1);
}

/* T6.0 */
ZTEST(host_if, test_page_count)
{
	zassert_equal(rtls_host_if_n_pages(0), 1);
	zassert_equal(rtls_host_if_n_pages(1), 1);
	zassert_equal(rtls_host_if_n_pages(9), 1);
	zassert_equal(rtls_host_if_n_pages(10), 2);
	zassert_equal(rtls_host_if_n_pages(19), 3);
	zassert_equal(rtls_host_if_n_pages(255 * 9 + 1), 255);
}

/* T6.0 */
ZTEST(host_if, test_info_get)
{
	const struct rtls_host_if_view view = view_of(3, 7);
	const uint8_t req[] = {RTLS_HOST_IF_REQ_INFO_GET, 0, 0xFF, 0xFF};
	struct rtls_host_if_event ev;
	const uint8_t expected[] = {
		0x40, 0x01, 0x00,             /* envelope OK */
		0xB0, 11,                     /* TLV INFO */
		1,    0,    6,    1,          /* protocolo, firmware 0.6.1 */
		0x01, 0x00,                   /* ID do gateway */
		16,   3,                      /* capacidade, tags ativas */
		100,  0,                      /* superquadro (ms) */
		9,                            /* registros por página */
	};

	zassert_equal(rtls_host_if_handle(req, sizeof(req), &view, rsp, &ev), sizeof(expected));
	zassert_mem_equal(rsp, expected, sizeof(expected));
	zassert_equal(ev.drdy_enable, -1);
	zassert_false(ev.last_page);
}

/* T6.0: 20 tags em 3 páginas, a última marcada */
ZTEST(host_if, test_tags_pages)
{
	const struct rtls_host_if_view view = view_of(20, 42);
	struct rtls_host_if_event ev;
	size_t len;

	fill_records();

	len = tags_get(&view, 0, &ev);
	check_page(len, 0, 3, 9, 42, 0);
	zassert_equal(len, 245, "página cheia deve ter 245 B");
	zassert_false(ev.last_page);

	len = tags_get(&view, 1, &ev);
	check_page(len, 1, 3, 9, 42, 9);
	zassert_false(ev.last_page);

	len = tags_get(&view, 2, &ev);
	check_page(len, 2, 3, 2, 42, 18);
	zassert_true(ev.last_page);

	/* página fora da faixa */
	zassert_equal(tags_get(&view, 3, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_INVALID);
	zassert_false(ev.last_page);
}

/* T6.0: tabela vazia ainda tem uma página, sem registros */
ZTEST(host_if, test_tags_empty)
{
	const struct rtls_host_if_view view = view_of(0, 1);
	struct rtls_host_if_event ev;

	check_page(tags_get(&view, 0, &ev), 0, 1, 0, 1, 0);
	zassert_true(ev.last_page);

	zassert_equal(tags_get(&view, 1, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_INVALID);
}

/* T6.0 */
ZTEST(host_if, test_drdy_cfg_set)
{
	const struct rtls_host_if_view view = view_of(0, 0);
	const uint8_t off[] = {RTLS_HOST_IF_REQ_DRDY_CFG_SET, 1, 0};
	const uint8_t bad[] = {RTLS_HOST_IF_REQ_DRDY_CFG_SET, 1, 2};
	struct rtls_host_if_event ev;

	zassert_equal(rtls_host_if_handle(off, sizeof(off), &view, rsp, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_OK);
	zassert_equal(ev.drdy_enable, 0);

	zassert_equal(rtls_host_if_handle(bad, sizeof(bad), &view, rsp, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_INVALID);
	zassert_equal(ev.drdy_enable, -1);
}

/* T6.0: NOP, transação vazia, opcode desconhecido e TLV truncado */
ZTEST(host_if, test_errors_and_nop)
{
	const struct rtls_host_if_view view = view_of(0, 0);
	const uint8_t nop[] = {0xFF};
	const uint8_t unknown[] = {0x55, 0};
	const uint8_t truncated[] = {RTLS_HOST_IF_REQ_TAGS_GET, 1};
	const uint8_t info_with_value[] = {RTLS_HOST_IF_REQ_INFO_GET, 1, 0};
	struct rtls_host_if_event ev;

	zassert_equal(rtls_host_if_handle(nop, sizeof(nop), &view, rsp, &ev), 0);
	zassert_equal(rtls_host_if_handle(nop, 0, &view, rsp, &ev), 0);

	zassert_equal(rtls_host_if_handle(unknown, sizeof(unknown), &view, rsp, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_UNKNOWN);

	zassert_equal(rtls_host_if_handle(truncated, sizeof(truncated), &view, rsp, &ev), 3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_INVALID);

	zassert_equal(rtls_host_if_handle(info_with_value, sizeof(info_with_value), &view, rsp,
					  &ev),
		      3);
	zassert_equal(rsp[2], RTLS_HOST_IF_ST_INVALID);
}

ZTEST_SUITE(host_if, NULL, NULL, NULL, NULL, NULL);
