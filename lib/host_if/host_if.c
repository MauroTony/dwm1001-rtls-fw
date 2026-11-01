/*
 * Transporte da interface com o host: SPIS + DRDY (docs/05-interface-spi-gateway.md).
 *
 * A thread host_if faz um laço de spi_transceive() em modo escravo. Cada
 * chamada é uma transação (um pulso de CS) e avança a máquina de estados:
 *
 *   IDLE --request--> META --[SIZE][NUM]--> DATA --resposta--> IDLE
 *
 * Fora dessas transações o SPIS não tem buffers e devolve o def-char 0x00,
 * que o host lê como "processando". Três transações de 1 byte 0xFF levam
 * qualquer estado de volta ao IDLE: META e DATA consomem uma cada, e no
 * IDLE o NOP não gera resposta.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/host_if.h>

LOG_MODULE_REGISTER(rtls_host_if, CONFIG_RTLS_HOST_IF_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(rtls_host_spi), "defina o chosen rtls,host-spi no overlay");

enum xfer_state {
	ST_IDLE,
	ST_META,
	ST_DATA,
};

static const struct device *const host_spi = DEVICE_DT_GET(DT_CHOSEN(rtls_host_spi));
static const struct gpio_dt_spec drdy = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), drdy_gpios);

/* Modo 0 (CPOL = CPHA = 0), MSB primeiro, 8 bits. */
static const struct spi_config spis_cfg = {
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};

/* Snapshot publicado pela thread main; protegido por snap_lock. */
static K_MUTEX_DEFINE(snap_lock);
static struct rtls_tag_record snap[CONFIG_RTLS_HOST_IF_MAX_RECORDS];
static size_t snap_count;
static uint8_t snap_seq;
static bool snap_unread;
static bool drdy_enabled = true;
static bool drdy_ready; /* pino configurado por rtls_host_if_init() */
static struct rtls_host_if_info gw_info;

/* Buffers do EasyDMA: só a thread host_if usa. */
static uint8_t rx_buf[RTLS_HOST_IF_MAX_XFER];
static uint8_t rsp_buf[RTLS_HOST_IF_MAX_XFER];
static uint8_t meta_buf[2];

static K_THREAD_STACK_DEFINE(host_if_stack, CONFIG_RTLS_HOST_IF_STACK_SIZE);
static struct k_thread host_if_thread_data;

/* Chame com snap_lock. Sem init (host_if indisponível), o pino fica intocado. */
static void update_drdy(void)
{
	if (drdy_ready) {
		(void)gpio_pin_set_dt(&drdy, drdy_enabled && snap_unread);
	}
}

/* Igual a menos do age_ms, que muda a cada snapshot. */
static bool same_record(const struct rtls_tag_record *a, const struct rtls_tag_record *b)
{
	const size_t head = offsetof(struct rtls_tag_record, age_ms);
	const size_t tail = offsetof(struct rtls_tag_record, pos_mm);

	return memcmp(a, b, head) == 0 &&
	       memcmp((const uint8_t *)a + tail, (const uint8_t *)b + tail, sizeof(*a) - tail) == 0;
}

int rtls_host_if_publish(const struct rtls_tag_record *records, size_t count)
{
	const size_t n = MIN(count, ARRAY_SIZE(snap));
	bool changed;

	k_mutex_lock(&snap_lock, K_FOREVER);
	changed = (n != snap_count);
	for (size_t i = 0; i < n; i++) {
		if (!changed && !same_record(&snap[i], &records[i])) {
			changed = true;
		}
		snap[i] = records[i];
	}
	snap_count = n;
	if (changed) {
		snap_seq++;
		snap_unread = true;
		update_drdy();
	}
	k_mutex_unlock(&snap_lock);

	return (n < count) ? -ENOMEM : 0;
}

/* Monta a resposta de um request a partir do snapshot atual. */
static size_t handle_request(const uint8_t *req, size_t len, uint8_t *rsp_seq, bool *last_page)
{
	struct rtls_host_if_event ev;
	struct rtls_host_if_view view;
	size_t rsp_len;

	k_mutex_lock(&snap_lock, K_FOREVER);
	view = (struct rtls_host_if_view){
		.info = &gw_info,
		.records = snap,
		.count = snap_count,
		.snapshot = snap_seq,
	};
	rsp_len = rtls_host_if_handle(req, len, &view, rsp_buf, &ev);
	if (ev.drdy_enable >= 0) {
		drdy_enabled = (ev.drdy_enable != 0);
		update_drdy();
	}
	*rsp_seq = snap_seq;
	*last_page = ev.last_page;
	k_mutex_unlock(&snap_lock);

	if (rsp_len > 0) {
		LOG_DBG("request 0x%02x (%zu B) -> resposta %zu B", req[0], len, rsp_len);
	}
	return rsp_len;
}

/* O host leu a última página do snapshot @p seq: o DRDY desce se não houver outro. */
static void mark_read(uint8_t seq)
{
	k_mutex_lock(&snap_lock, K_FOREVER);
	if (seq == snap_seq) {
		snap_unread = false;
		update_drdy();
	}
	k_mutex_unlock(&snap_lock);
}

/* Uma transação: arma os buffers e espera o CS subir. Retorna os bytes recebidos. */
static int xfer(const uint8_t *tx, size_t tx_len)
{
	const struct spi_buf txb = {.buf = (void *)tx, .len = tx_len};
	const struct spi_buf rxb = {.buf = rx_buf, .len = sizeof(rx_buf)};
	const struct spi_buf_set txs = {.buffers = &txb, .count = 1};
	const struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};

	return spi_transceive(host_spi, &spis_cfg, (tx_len > 0) ? &txs : NULL, &rxs);
}

static void host_if_thread(void *p1, void *p2, void *p3)
{
	enum xfer_state state = ST_IDLE;
	size_t rsp_len = 0;
	uint8_t rsp_seq = 0;
	bool last_page = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int n = 0;

		switch (state) {
		case ST_IDLE:
			/* Sem dados para o host: o MISO sai com o ORC (0xFF). */
			n = xfer(NULL, 0);
			if (n > 0) {
				rsp_len = handle_request(rx_buf, (size_t)n, &rsp_seq, &last_page);
				if (rsp_len > 0) {
					meta_buf[0] = (uint8_t)rsp_len;
					meta_buf[1] = 1; /* toda resposta cabe numa transação */
					state = ST_META;
				}
			}
			break;

		case ST_META:
			/* Qualquer transação consome o meta-header, até um NOP. */
			n = xfer(meta_buf, sizeof(meta_buf));
			if (n > 0) {
				state = ST_DATA;
			}
			break;

		case ST_DATA:
			n = xfer(rsp_buf, rsp_len);
			if (n > 0) {
				/* Só conta como lida se o host leu a resposta inteira. */
				if (last_page && (size_t)n >= rsp_len) {
					mark_read(rsp_seq);
				}
				state = ST_IDLE;
			}
			break;
		}

		if (n < 0) {
			LOG_ERR("spi_transceive: %d", n);
			k_sleep(K_MSEC(10));
		}
	}
}

int rtls_host_if_init(const struct rtls_host_if_info *info)
{
	int err;

	if (info == NULL) {
		return -EINVAL;
	}
	if (!device_is_ready(host_spi) || !gpio_is_ready_dt(&drdy)) {
		LOG_ERR("SPIS/DRDY indisponiveis");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&drdy, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	k_mutex_lock(&snap_lock, K_FOREVER);
	drdy_ready = true;
	update_drdy();
	k_mutex_unlock(&snap_lock);

	gw_info = *info;
	k_thread_create(&host_if_thread_data, host_if_stack, K_THREAD_STACK_SIZEOF(host_if_stack),
			host_if_thread, NULL, NULL, NULL, CONFIG_RTLS_HOST_IF_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&host_if_thread_data, "host_if");

	LOG_INF("host_if pronto (SPIS %s, protocolo v%d, %d registros por pagina)",
		host_spi->name, RTLS_HOST_IF_PROTO_VERSION, RTLS_HOST_IF_RECORDS_PER_PAGE);
	return 0;
}
