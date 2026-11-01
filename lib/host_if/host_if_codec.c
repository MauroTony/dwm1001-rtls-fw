/*
 * Codec do protocolo SPI com o host (docs/05-interface-spi-gateway.md,
 * seções 2 e 3). Puro: monta respostas a partir de um snapshot, sem SPI.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <rtls/host_if.h>

/* Envelope [0x40][0x01][status] + cabeçalho [tipo][len] do TLV de dados. */
#define ENVELOPE_LEN 3
#define TLV_HDR_LEN  2
/* Cabeçalho do value do 0xB1: page, n_pages, count, snapshot. */
#define TAGS_HDR_LEN 4
#define CRC_LEN      2

uint16_t rtls_host_if_crc(const uint8_t *data, size_t len)
{
	return crc16_itu_t(0xFFFF, data, len);
}

uint8_t rtls_host_if_n_pages(size_t count)
{
	const size_t pages = DIV_ROUND_UP(count, RTLS_HOST_IF_RECORDS_PER_PAGE);

	return (uint8_t)CLAMP(pages, 1, UINT8_MAX);
}

static size_t put_envelope(uint8_t *rsp, uint8_t status)
{
	rsp[0] = RTLS_HOST_IF_RSP_ENVELOPE;
	rsp[1] = 1;
	rsp[2] = status;
	return ENVELOPE_LEN;
}

static size_t build_info(const struct rtls_host_if_view *view, uint8_t *rsp)
{
	const struct rtls_host_if_info *info = view->info;
	uint8_t *v = rsp + ENVELOPE_LEN + TLV_HDR_LEN;

	put_envelope(rsp, RTLS_HOST_IF_ST_OK);
	rsp[ENVELOPE_LEN] = RTLS_HOST_IF_RSP_INFO;
	rsp[ENVELOPE_LEN + 1] = RTLS_HOST_IF_INFO_LEN;

	v[0] = RTLS_HOST_IF_PROTO_VERSION;
	memcpy(&v[1], info->fw_version, 3);
	sys_put_le16(info->gateway_id, &v[4]);
	v[6] = info->capacity;
	v[7] = (uint8_t)MIN(view->count, UINT8_MAX);
	sys_put_le16(info->superframe_ms, &v[8]);
	v[10] = RTLS_HOST_IF_RECORDS_PER_PAGE;

	return ENVELOPE_LEN + TLV_HDR_LEN + RTLS_HOST_IF_INFO_LEN;
}

static size_t build_tags(const struct rtls_host_if_view *view, uint8_t page, uint8_t *rsp,
			 struct rtls_host_if_event *ev)
{
	const uint8_t n_pages = rtls_host_if_n_pages(view->count);
	const size_t first = (size_t)page * RTLS_HOST_IF_RECORDS_PER_PAGE;
	uint8_t *v = rsp + ENVELOPE_LEN + TLV_HDR_LEN;
	size_t count, value_len;

	if (page >= n_pages) {
		return put_envelope(rsp, RTLS_HOST_IF_ST_INVALID);
	}

	count = (view->count > first) ? MIN(view->count - first, RTLS_HOST_IF_RECORDS_PER_PAGE) : 0;
	value_len = TAGS_HDR_LEN + count * sizeof(struct rtls_tag_record) + CRC_LEN;

	put_envelope(rsp, RTLS_HOST_IF_ST_OK);
	rsp[ENVELOPE_LEN] = RTLS_HOST_IF_RSP_TAGS;
	rsp[ENVELOPE_LEN + 1] = (uint8_t)value_len;

	v[0] = page;
	v[1] = n_pages;
	v[2] = (uint8_t)count;
	v[3] = view->snapshot;
	/* O registro já é o formato do barramento (packed, little-endian). */
	if (count > 0) {
		memcpy(&v[TAGS_HDR_LEN], &view->records[first],
		       count * sizeof(struct rtls_tag_record));
	}
	sys_put_le16(rtls_host_if_crc(v, value_len - CRC_LEN), &v[value_len - CRC_LEN]);

	ev->last_page = (page == n_pages - 1);
	return ENVELOPE_LEN + TLV_HDR_LEN + value_len;
}

size_t rtls_host_if_handle(const uint8_t *req, size_t req_len, const struct rtls_host_if_view *view,
			   uint8_t *rsp, struct rtls_host_if_event *ev)
{
	uint8_t type, len;
	const uint8_t *value;

	ev->drdy_enable = -1;
	ev->last_page = false;

	/* Transação vazia ou NOP (inclui a recuperação com 3 x 0xFF): sem resposta. */
	if (req_len == 0 || req[0] == RTLS_HOST_IF_REQ_NOP) {
		return 0;
	}

	/* TLV truncado; o que vier depois do TLV (preenchimento 0xFF) é ignorado. */
	if (req_len < TLV_HDR_LEN || req_len < (size_t)TLV_HDR_LEN + req[1]) {
		return put_envelope(rsp, RTLS_HOST_IF_ST_INVALID);
	}
	type = req[0];
	len = req[1];
	value = &req[TLV_HDR_LEN];

	switch (type) {
	case RTLS_HOST_IF_REQ_INFO_GET:
		if (len != 0) {
			break;
		}
		return build_info(view, rsp);

	case RTLS_HOST_IF_REQ_TAGS_GET:
		if (len != 1) {
			break;
		}
		return build_tags(view, value[0], rsp, ev);

	case RTLS_HOST_IF_REQ_DRDY_CFG_SET:
		if (len != 1 || value[0] > 1) {
			break;
		}
		ev->drdy_enable = (int8_t)value[0];
		return put_envelope(rsp, RTLS_HOST_IF_ST_OK);

	default:
		return put_envelope(rsp, RTLS_HOST_IF_ST_UNKNOWN);
	}

	return put_envelope(rsp, RTLS_HOST_IF_ST_INVALID);
}
