/*
 * Driver DW1000 próprio do RTLS.
 *
 * Referências: DW1000 User Manual v2.18 (mapa de registradores e sequência
 * de inicialização) e o driver IEEE 802.15.4 do Zephyr
 * (drivers/ieee802154/ieee802154_dw1000*.{c,h}, Apache-2.0), usado só como
 * referência de valores de ajuste.
 *
 * M0: reset, acesso SPI a registradores e probe do DEV_ID.
 * M1: configuração completa, IRQ, TX/RX, timestamps (marcados TODO(M1)).
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rtls/dw1000.h>

LOG_MODULE_REGISTER(rtls_dw1000, CONFIG_RTLS_DW1000_LOG_LEVEL);

#define DW_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(decawave_dw1000)

/* Identificadores de registrador (User Manual, cap. 7). */
#define DW_REG_DEV_ID 0x00

/* Cabeçalho da transação SPI (User Manual, seção 2.2.1.2). */
#define DW_SPI_WRITE      BIT(7)
#define DW_SPI_SUB_INDEX  BIT(6)
#define DW_SPI_EXT_INDEX  BIT(7)
#define DW_SPI_REG_MASK   0x3F

static const struct spi_dt_spec dw_spi =
	SPI_DT_SPEC_GET(DW_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec dw_irq = GPIO_DT_SPEC_GET(DW_NODE, int_gpios);
static const struct gpio_dt_spec dw_rst = GPIO_DT_SPEC_GET(DW_NODE, reset_gpios);

/* Mesma configuração do barramento, mas com clock baixo (antes do PLL travar). */
static struct spi_dt_spec dw_spi_slow;
static const struct spi_dt_spec *dw_bus;

static size_t dw_build_header(uint8_t *hdr, bool write, uint8_t reg, uint16_t offset)
{
	hdr[0] = (write ? DW_SPI_WRITE : 0) | (reg & DW_SPI_REG_MASK);

	if (offset == 0) {
		return 1;
	}

	hdr[0] |= DW_SPI_SUB_INDEX;
	if (offset < 0x80) {
		hdr[1] = (uint8_t)offset;
		return 2;
	}

	hdr[1] = DW_SPI_EXT_INDEX | (offset & 0x7F);
	hdr[2] = (uint8_t)(offset >> 7);
	return 3;
}

static int dw_reg_read(uint8_t reg, uint16_t offset, void *data, size_t len)
{
	uint8_t hdr[3];
	const size_t hdr_len = dw_build_header(hdr, false, reg, offset);
	const struct spi_buf tx_buf = {.buf = hdr, .len = hdr_len};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	struct spi_buf rx_bufs[2] = {
		{.buf = NULL, .len = hdr_len},
		{.buf = data, .len = len},
	};
	const struct spi_buf_set rx = {.buffers = rx_bufs, .count = ARRAY_SIZE(rx_bufs)};

	return spi_transceive_dt(dw_bus, &tx, &rx);
}

static __maybe_unused int dw_reg_write(uint8_t reg, uint16_t offset, const void *data,
					size_t len)
{
	uint8_t hdr[3];
	const size_t hdr_len = dw_build_header(hdr, true, reg, offset);
	const struct spi_buf tx_bufs[2] = {
		{.buf = hdr, .len = hdr_len},
		{.buf = (void *)data, .len = len},
	};
	const struct spi_buf_set tx = {.buffers = tx_bufs, .count = ARRAY_SIZE(tx_bufs)};

	return spi_write_dt(dw_bus, &tx);
}

static int dw_hard_reset(void)
{
	int err;

	/* RSTn é dreno aberto: pulsa em nível baixo e depois solta (entrada). */
	err = gpio_pin_configure_dt(&dw_rst, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}
	k_sleep(K_MSEC(1));

	err = gpio_pin_configure_dt(&dw_rst, GPIO_INPUT);
	if (err) {
		return err;
	}

	/* Tempo para o oscilador estabilizar e o chip chegar ao estado IDLE. */
	k_sleep(K_MSEC(5));
	return 0;
}

int dw1000_read_dev_id(uint32_t *dev_id)
{
	uint8_t raw[4];
	int err = dw_reg_read(DW_REG_DEV_ID, 0, raw, sizeof(raw));

	if (err == 0) {
		*dev_id = sys_get_le32(raw);
	}
	return err;
}

int dw1000_init(void)
{
	uint32_t dev_id = 0;
	int err;

	if (!spi_is_ready_dt(&dw_spi) || !gpio_is_ready_dt(&dw_irq) ||
	    !gpio_is_ready_dt(&dw_rst)) {
		LOG_ERR("SPI/GPIO do DW1000 indisponiveis");
		return -ENODEV;
	}

	dw_spi_slow = dw_spi;
	dw_spi_slow.config.frequency =
		MIN((uint32_t)CONFIG_RTLS_DW1000_SPI_FREQ_SLOW, dw_spi.config.frequency);
	dw_bus = &dw_spi_slow;

	err = dw_hard_reset();
	if (err) {
		LOG_ERR("falha no reset (%d)", err);
		return err;
	}

	err = dw1000_read_dev_id(&dev_id);
	if (err) {
		LOG_ERR("falha de SPI ao ler DEV_ID (%d)", err);
		return -ENODEV;
	}

	if (dev_id != DW1000_DEV_ID) {
		LOG_ERR("DEV_ID inesperado 0x%08x (esperado 0x%08x)", dev_id, DW1000_DEV_ID);
		return -ENODEV;
	}

	LOG_INF("DW1000 DEV_ID 0x%08x", dev_id);

	/*
	 * TODO(M1): sequência de inicialização (User Manual, seção 2.5.5 e
	 * cap. 7): clocks/PLL, carga do microcódigo LDE, leitura da OTP (XTAL
	 * trim), ajustes padrão obrigatórios, máscara de IRQ e callback do
	 * GPIO de IRQ -> thread cooperativa. Só depois do PLL travado:
	 *     dw_bus = &dw_spi;
	 */

	return 0;
}

int dw1000_configure(const struct dw1000_phy_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENOSYS; /* TODO(M1) */
}

int dw1000_set_antenna_delay(uint16_t tx_ticks, uint16_t rx_ticks)
{
	ARG_UNUSED(tx_ticks);
	ARG_UNUSED(rx_ticks);
	return -ENOSYS; /* TODO(M1) */
}

int dw1000_transmit(const void *frame, size_t len, const struct dw1000_tx_opts *opts,
		    uint64_t *tx_ts)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(len);
	ARG_UNUSED(opts);
	ARG_UNUSED(tx_ts);
	return -ENOSYS; /* TODO(M1) */
}

int dw1000_receive(void *buf, size_t size, struct dw1000_rx_info *info, k_timeout_t timeout)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	ARG_UNUSED(info);
	ARG_UNUSED(timeout);
	return -ENOSYS; /* TODO(M1) */
}

uint64_t dw1000_sys_time(void)
{
	return 0; /* TODO(M1) */
}

void dw1000_idle(void)
{
	/* TODO(M1) */
}
