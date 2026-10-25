/*
 * Driver DW1000 próprio do RTLS.
 *
 * Referências: DW1000 User Manual v2.18 (mapa de registradores, sequência de
 * inicialização e tabelas de ajuste) e DW1000 Datasheet.
 *
 * Modelo de execução: a ISR do pino de IRQ só libera um semáforo. Quem
 * chamou dw1000_transmit()/dw1000_receive() (a thread dona do rádio) lê o
 * SYS_STATUS pelo SPI e trata o evento no próprio contexto. Assim não há
 * thread extra nem SPI concorrente.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rtls/dw1000.h>

#include "dw1000_regs.h"

LOG_MODULE_REGISTER(rtls_dw1000, CONFIG_RTLS_DW1000_LOG_LEVEL);

#define DW_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(decawave_dw1000)

/* Cabeçalho da transação SPI (User Manual, seção 2.2.1.2). */
#define DW_SPI_WRITE     BIT(7)
#define DW_SPI_SUB_INDEX BIT(6)
#define DW_SPI_EXT_INDEX BIT(7)
#define DW_SPI_REG_MASK  0x3F

/* Prazos de segurança para o fim de um TX (o atraso programado é de poucos ms). */
#define DW_TX_TIMEOUT         K_MSEC(5)
#define DW_DELAYED_TX_TIMEOUT K_MSEC(50)

/* RXPACC é corrigido quando não satura (User Manual 4.7.2, SFD padrão a 6,8 Mbps). */
#define DW_RXPACC_SFD_CORRECTION 5

/*
 * Valores de ajuste para canal 5, PRF 64 MHz, preâmbulo 128 (PAC 8,
 * código 9), 6,8 Mbps e SFD padrão (User Manual, seção 2.5.5 e cap. 7).
 */
#define DW_CH5_FS_PLLCFG   0x0800041DU
#define DW_CH5_FS_PLLTUNE  0xBEU
#define DW_CH5_RF_RXCTRLH  0xD8U
#define DW_CH5_RF_TXCTRL   0x001E3FE0U
#define DW_CH5_TC_PGDELAY  0xC0U
#define DW_CH5_TX_POWER    0x25456585U /* potência "smart" para PRF 64 */
#define DW_PRF64_AGC_TUNE1 0x889BU
#define DW_AGC_TUNE2_VAL   0x2502A907U
#define DW_6M8_DRX_TUNE0B  0x0001U /* 6,8 Mbps, SFD padrão */
#define DW_PRF64_DRX_TUNE1A 0x008DU
#define DW_6M8_DRX_TUNE1B  0x0020U /* preâmbulo de 128 a 1024 */
#define DW_PAC8_PRF64_DRX_TUNE2 0x313B006BU
#define DW_PLEN128_DRX_TUNE4H 0x0028U
#define DW_SFD_TIMEOUT     (128 + 1 + 8 - 8) /* preâmbulo + 1 + SFD - PAC */
#define DW_LDE_CFG1_VAL    0x6DU   /* NTM = 13, PMULT = 3 */
#define DW_PRF64_LDE_CFG2  0x0607U
#define DW_PCODE9_LDE_REPC 0x28F4U
#define DW_PCODE           9U
#define DW_CHANNEL         5U

#define DW_TX_FCTRL_BASE                                                                          \
	(DW_TX_FCTRL_TXBR_6M8 | DW_TX_FCTRL_TR | DW_TX_FCTRL_PRF_64M | DW_TX_FCTRL_PLEN_128)

static const struct spi_dt_spec dw_spi =
	SPI_DT_SPEC_GET(DW_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec dw_irq = GPIO_DT_SPEC_GET(DW_NODE, int_gpios);
static const struct gpio_dt_spec dw_rst = GPIO_DT_SPEC_GET(DW_NODE, reset_gpios);

/* Mesma configuração do barramento, mas com clock baixo (antes do PLL travar). */
static struct spi_dt_spec dw_spi_slow;
static const struct spi_dt_spec *dw_bus;

static struct gpio_callback dw_irq_cb;
K_SEM_DEFINE(dw_irq_sem, 0, 1);

static bool dw_rx_on;
static struct dw1000_stats dw_stats;

/* ------------------------------------------------------------------------- */
/* Acesso a registradores                                                     */

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

static int dw_read(uint8_t reg, uint16_t offset, void *data, size_t len)
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

static int dw_write(uint8_t reg, uint16_t offset, const void *data, size_t len)
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

static uint16_t dw_read16(uint8_t reg, uint16_t offset)
{
	uint8_t raw[2] = {0};

	(void)dw_read(reg, offset, raw, sizeof(raw));
	return sys_get_le16(raw);
}

static uint32_t dw_read32(uint8_t reg, uint16_t offset)
{
	uint8_t raw[4] = {0};

	(void)dw_read(reg, offset, raw, sizeof(raw));
	return sys_get_le32(raw);
}

static uint64_t dw_read40(uint8_t reg, uint16_t offset)
{
	uint8_t raw[5] = {0};

	(void)dw_read(reg, offset, raw, sizeof(raw));
	return sys_get_le40(raw);
}

static void dw_write8(uint8_t reg, uint16_t offset, uint8_t value)
{
	(void)dw_write(reg, offset, &value, sizeof(value));
}

static void dw_write16(uint8_t reg, uint16_t offset, uint16_t value)
{
	uint8_t raw[2];

	sys_put_le16(value, raw);
	(void)dw_write(reg, offset, raw, sizeof(raw));
}

static void dw_write32(uint8_t reg, uint16_t offset, uint32_t value)
{
	uint8_t raw[4];

	sys_put_le32(value, raw);
	(void)dw_write(reg, offset, raw, sizeof(raw));
}

/* ------------------------------------------------------------------------- */
/* Relógios, OTP e LDE                                                         */

enum dw_clocks {
	DW_CLK_ALL_SEQ,   /* relógios automáticos (PLL), estado normal */
	DW_CLK_FORCE_XTI, /* relógio XTI de 19,2 MHz (cristal de 38,4 MHz / 2), na inicialização */
	DW_CLK_FORCE_LDE, /* configuração exigida para carregar o microcódigo LDE */
};

static void dw_set_clocks(enum dw_clocks mode)
{
	uint8_t reg[2] = {0};

	(void)dw_read(DW_REG_PMSC, DW_PMSC_CTRL0, reg, sizeof(reg));

	switch (mode) {
	case DW_CLK_FORCE_XTI:
		reg[0] = 0x01 | (reg[0] & 0xFC);
		break;
	case DW_CLK_FORCE_LDE:
		reg[0] = 0x01;
		reg[1] = 0x03;
		break;
	case DW_CLK_ALL_SEQ:
	default:
		reg[0] = 0x00;
		reg[1] &= 0xFE;
		break;
	}

	(void)dw_write(DW_REG_PMSC, DW_PMSC_CTRL0, reg, sizeof(reg));
}

static uint32_t dw_otp_read(uint16_t address)
{
	dw_write16(DW_REG_OTP_IF, DW_OTP_ADDR, address);
	dw_write8(DW_REG_OTP_IF, DW_OTP_CTRL, DW_OTP_CTRL_OTPRDEN | DW_OTP_CTRL_OTPREAD);
	dw_write8(DW_REG_OTP_IF, DW_OTP_CTRL, 0);
	return dw_read32(DW_REG_OTP_IF, DW_OTP_RDAT);
}

static void dw_load_lde(void)
{
	dw_set_clocks(DW_CLK_FORCE_LDE);
	dw_write16(DW_REG_OTP_IF, DW_OTP_CTRL, DW_OTP_CTRL_LDELOAD);
	k_busy_wait(150);
	dw_set_clocks(DW_CLK_ALL_SEQ);
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

	/* Tempo para o oscilador estabilizar e o chip chegar ao estado INIT. */
	k_sleep(K_MSEC(5));
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Eventos                                                                    */

static void dw_irq_handler(const struct device *port, struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_sem_give(&dw_irq_sem);
}

/*
 * Espera algum bit de @p mask em SYS_STATUS. O status é lido antes de dormir,
 * então uma borda de IRQ perdida só custa a espera até o próximo evento.
 */
static int dw_wait_status(uint32_t mask, k_timeout_t timeout, uint32_t *status)
{
	const k_timepoint_t end = sys_timepoint_calc(timeout);
	uint32_t st;

	while (true) {
		st = dw_read32(DW_REG_SYS_STATUS, 0);
		if (st & mask) {
			*status = st;
			return 0;
		}

		if (k_sem_take(&dw_irq_sem, sys_timepoint_timeout(end)) != 0) {
			st = dw_read32(DW_REG_SYS_STATUS, 0);
			if (st & mask) {
				*status = st;
				return 0;
			}
			return -EAGAIN;
		}
	}
}

static void dw_trx_off(void)
{
	dw_write8(DW_REG_SYS_CTRL, 0, DW_SYS_CTRL_TRXOFF);
	dw_write32(DW_REG_SYS_STATUS, 0,
		   DW_SYS_STATUS_ALL_TX | DW_SYS_STATUS_ALL_RX_GOOD | DW_SYS_STATUS_ALL_RX_ERR |
			   DW_SYS_STATUS_ALL_RX_TO | DW_SYS_STATUS_HPDWARN);
	k_sem_reset(&dw_irq_sem);
	dw_rx_on = false;
}

/* 10*log10(numerador / N^2) - A, em centésimos de dBm (User Manual 4.7.1/4.7.2). */
static int16_t dw_power_cdbm(float numerator, uint16_t n)
{
	float dbm;

	if (n == 0 || numerator <= 0.0f) {
		return INT16_MIN;
	}

	dbm = 10.0f * log10f(numerator / ((float)n * (float)n)) - DW_RX_POWER_A_PRF64;
	return (int16_t)CLAMP(dbm * 100.0f, (float)INT16_MIN, (float)INT16_MAX);
}

/* ------------------------------------------------------------------------- */
/* API pública                                                                */

int dw1000_read_dev_id(uint32_t *dev_id)
{
	uint8_t raw[4];
	int err = dw_read(DW_REG_DEV_ID, 0, raw, sizeof(raw));

	if (err == 0) {
		*dev_id = sys_get_le32(raw);
	}
	return err;
}

int dw1000_init(void)
{
	uint32_t dev_id = 0;
	uint32_t ldotune, part_id, otp_antdly;
	uint8_t xtal_trim;
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
	if (err || dev_id != DW1000_DEV_ID) {
		LOG_ERR("DEV_ID inesperado 0x%08x (esperado 0x%08x, spi %d)", dev_id,
			DW1000_DEV_ID, err);
		return -ENODEV;
	}

	/* Inicialização no cristal (User Manual 2.5.5 e 6.3). */
	dw_set_clocks(DW_CLK_FORCE_XTI);
	dw_write8(DW_REG_EXT_SYNC, DW_EC_CTRL, DW_EC_CTRL_PLLLCK);

	ldotune = dw_otp_read(DW_OTP_ADDR_LDOTUNE);
	if ((ldotune & 0xFF) != 0) {
		dw_write8(DW_REG_OTP_IF, DW_OTP_SF, DW_OTP_SF_LDO_KICK);
	}
	part_id = dw_otp_read(DW_OTP_ADDR_PARTID);
	otp_antdly = dw_otp_read(DW_OTP_ADDR_ANTDLY);
	xtal_trim = dw_otp_read(DW_OTP_ADDR_XTALTRIM) & DW_FS_XTALT_MASK;
	if (xtal_trim == 0) {
		xtal_trim = 0x10; /* placa sem calibração de fábrica: meio da faixa */
	}
	dw_write8(DW_REG_FS_CTRL, DW_FS_XTALT, DW_FS_XTALT_RESERVED | xtal_trim);

	dw_load_lde();
	dw_write8(DW_REG_AON, DW_AON_CFG1, 0x00);

	/* Relógios automáticos: o PLL trava e o SPI pode ir ao clock máximo. */
	k_busy_wait(100);
	if ((dw_read32(DW_REG_SYS_STATUS, 0) & DW_SYS_STATUS_CPLOCK) == 0) {
		LOG_WRN("PLL do DW1000 nao travou");
	}
	dw_bus = &dw_spi;
	err = dw1000_read_dev_id(&dev_id);
	if (err || dev_id != DW1000_DEV_ID) {
		LOG_ERR("DEV_ID 0x%08x no clock rapido (%u Hz)", dev_id, dw_spi.config.frequency);
		return -EIO;
	}

	err = gpio_pin_configure_dt(&dw_irq, GPIO_INPUT);
	if (err == 0) {
		gpio_init_callback(&dw_irq_cb, dw_irq_handler, BIT(dw_irq.pin));
		err = gpio_add_callback_dt(&dw_irq, &dw_irq_cb);
	}
	if (err == 0) {
		err = gpio_pin_interrupt_configure_dt(&dw_irq, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (err) {
		LOG_ERR("falha ao configurar a IRQ (%d)", err);
		return err;
	}

	/* Só os eventos que a API espera geram IRQ. */
	dw_write32(DW_REG_SYS_MASK, 0, DW_SYS_STATUS_TXFRS | DW_SYS_STATUS_RXFCG);

	(void)dw1000_set_antenna_delay(CONFIG_RTLS_DW1000_TX_ANT_DLY,
				       CONFIG_RTLS_DW1000_RX_ANT_DLY);
	dw_trx_off();

	LOG_INF("DW1000 DEV_ID 0x%08x part 0x%08x xtal %u ldo 0x%02x otp_antdly 0x%08x", dev_id,
		part_id, xtal_trim, ldotune & 0xFF, otp_antdly);
	return 0;
}

int dw1000_configure(const struct dw1000_phy_config *cfg)
{
	uint32_t sys_cfg;

	if (cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->channel != DW_CHANNEL || cfg->prf_mhz != 64 || cfg->preamble_len != 128 ||
	    cfg->pac_size != 8 || cfg->preamble_code != DW_PCODE ||
	    cfg->data_rate_kbps != 6800) {
		return -ENOTSUP;
	}

	dw_trx_off();

	/*
	 * PHR padrão, sem buffer duplo, IRQ ativa em nível alto, RX religa
	 * sozinho após erro, filtragem de quadros de dados ligada.
	 */
	sys_cfg = dw_read32(DW_REG_SYS_CFG, 0);
	sys_cfg &= ~(DW_SYS_CFG_PHR_MODE | DW_SYS_CFG_RXM110K | DW_SYS_CFG_DIS_STXP |
		     DW_SYS_CFG_RXWTOE);
	sys_cfg |= DW_SYS_CFG_HIRQ_POL | DW_SYS_CFG_DIS_DRXB | DW_SYS_CFG_RXAUTR |
		   DW_SYS_CFG_FFEN | DW_SYS_CFG_FFAD;
	dw_write32(DW_REG_SYS_CFG, 0, sys_cfg);

	dw_write32(DW_REG_PANADR, 0, (uint32_t)cfg->short_addr | ((uint32_t)cfg->pan_id << 16));

	/* Algoritmo de detecção do primeiro caminho (LDE). */
	dw_write8(DW_REG_LDE_IF, DW_LDE_CFG1, DW_LDE_CFG1_VAL);
	dw_write16(DW_REG_LDE_IF, DW_LDE_CFG2, DW_PRF64_LDE_CFG2);
	dw_write16(DW_REG_LDE_IF, DW_LDE_REPC, DW_PCODE9_LDE_REPC);

	/* Sintetizador e RF do canal 5. */
	dw_write32(DW_REG_FS_CTRL, DW_FS_PLLCFG, DW_CH5_FS_PLLCFG);
	dw_write8(DW_REG_FS_CTRL, DW_FS_PLLTUNE, DW_CH5_FS_PLLTUNE);
	dw_write8(DW_REG_RF_CONF, DW_RF_RXCTRLH, DW_CH5_RF_RXCTRLH);
	dw_write32(DW_REG_RF_CONF, DW_RF_TXCTRL, DW_CH5_RF_TXCTRL);
	dw_write8(DW_REG_TX_CAL, DW_TC_PGDELAY, DW_CH5_TC_PGDELAY);
	dw_write32(DW_REG_TX_POWER, 0, DW_CH5_TX_POWER);

	/* Receptor digital e AGC. */
	dw_write16(DW_REG_DRX_CONF, DW_DRX_TUNE0B, DW_6M8_DRX_TUNE0B);
	dw_write16(DW_REG_DRX_CONF, DW_DRX_TUNE1A, DW_PRF64_DRX_TUNE1A);
	dw_write16(DW_REG_DRX_CONF, DW_DRX_TUNE1B, DW_6M8_DRX_TUNE1B);
	dw_write32(DW_REG_DRX_CONF, DW_DRX_TUNE2, DW_PAC8_PRF64_DRX_TUNE2);
	dw_write16(DW_REG_DRX_CONF, DW_DRX_TUNE4H, DW_PLEN128_DRX_TUNE4H);
	dw_write16(DW_REG_DRX_CONF, DW_DRX_SFDTOC, DW_SFD_TIMEOUT);
	dw_write16(DW_REG_AGC_CTRL, DW_AGC_TUNE1, DW_PRF64_AGC_TUNE1);
	dw_write32(DW_REG_AGC_CTRL, DW_AGC_TUNE2, DW_AGC_TUNE2_VAL);

	/* Canal, PRF e códigos de preâmbulo de TX/RX. */
	dw_write32(DW_REG_CHAN_CTRL, 0,
		   (DW_CHANNEL << DW_CHAN_CTRL_TX_CHAN_SHIFT) |
			   (DW_CHANNEL << DW_CHAN_CTRL_RX_CHAN_SHIFT) | DW_CHAN_CTRL_RXPRF_64M |
			   (DW_PCODE << DW_CHAN_CTRL_TX_PCODE_SHIFT) |
			   (DW_PCODE << DW_CHAN_CTRL_RX_PCODE_SHIFT));

	LOG_INF("PHY: canal %u, PRF %u MHz, preambulo %u (codigo %u), %u kbps, PAN 0x%04x, "
		"addr 0x%04x",
		cfg->channel, cfg->prf_mhz, cfg->preamble_len, cfg->preamble_code,
		cfg->data_rate_kbps, cfg->pan_id, cfg->short_addr);
	return 0;
}

int dw1000_set_antenna_delay(uint16_t tx_ticks, uint16_t rx_ticks)
{
	dw_write16(DW_REG_TX_ANTD, 0, tx_ticks);
	dw_write16(DW_REG_LDE_IF, DW_LDE_RXANTD, rx_ticks);
	return 0;
}

int dw1000_transmit(const void *frame, size_t len, const struct dw1000_tx_opts *opts,
		    uint64_t *tx_ts)
{
	const bool delayed = (opts != NULL) && (opts->delayed_ts != 0);
	const bool w4r = (opts != NULL) && opts->rx_after_tx;
	uint8_t ctrl = DW_SYS_CTRL_TXSTRT;
	uint32_t status;

	if (frame == NULL || len == 0 || len > DW1000_MAX_FRAME_LEN) {
		return -EINVAL;
	}

	if (dw_rx_on) {
		dw_trx_off();
	} else {
		dw_write32(DW_REG_SYS_STATUS, 0, DW_SYS_STATUS_ALL_TX | DW_SYS_STATUS_HPDWARN);
	}

	(void)dw_write(DW_REG_TX_BUFFER, 0, frame, len);
	dw_write32(DW_REG_TX_FCTRL, 0, DW_TX_FCTRL_BASE | (uint32_t)(len + 2));

	if (w4r) {
		/* W4R_TIM: 20 bits em UWB µs; ACK_TIM (byte 3) fica zerado. */
		dw_write32(DW_REG_ACK_RESP_T, 0, opts->rx_delay_uus & 0xFFFFFU);
		ctrl |= DW_SYS_CTRL_WAIT4RESP;
	}

	if (delayed) {
		uint8_t dx_time[5];

		/* O DW1000 ignora os 9 bits menos significativos do instante. */
		sys_put_le40(opts->delayed_ts & DW1000_TS_MASK, dx_time);
		(void)dw_write(DW_REG_DX_TIME, 0, dx_time, sizeof(dx_time));
		ctrl |= DW_SYS_CTRL_TXDLYS;
	}

	k_sem_reset(&dw_irq_sem);
	dw_write8(DW_REG_SYS_CTRL, 0, ctrl);

	if (delayed && (dw_read16(DW_REG_SYS_STATUS, 3) & DW_SYS_STATUS_HI16_TXERR)) {
		/* Instante já passou (ou não dá tempo de ligar o TX). */
		dw_trx_off();
		dw_stats.tx_late++;
		return -ETIME;
	}

	if (dw_wait_status(DW_SYS_STATUS_TXFRS, delayed ? DW_DELAYED_TX_TIMEOUT : DW_TX_TIMEOUT,
			   &status) != 0) {
		LOG_WRN("TX nao terminou (status 0x%08x)", dw_read32(DW_REG_SYS_STATUS, 0));
		dw_trx_off();
		return -EIO;
	}
	dw_write32(DW_REG_SYS_STATUS, 0, DW_SYS_STATUS_ALL_TX);

	if (tx_ts != NULL) {
		*tx_ts = dw_read40(DW_REG_TX_TIME, 0);
	}

	dw_rx_on = w4r;
	dw_stats.tx_ok++;
	return 0;
}

int dw1000_receive(void *buf, size_t size, struct dw1000_rx_info *info, k_timeout_t timeout)
{
	uint32_t status, finfo;
	size_t len;

	if (!dw_rx_on) {
		dw_write32(DW_REG_SYS_STATUS, 0,
			   DW_SYS_STATUS_ALL_RX_GOOD | DW_SYS_STATUS_ALL_RX_ERR |
				   DW_SYS_STATUS_ALL_RX_TO);
		k_sem_reset(&dw_irq_sem);
		dw_write16(DW_REG_SYS_CTRL, 0, DW_SYS_CTRL_RXENAB);
		dw_rx_on = true;
	}

	if (dw_wait_status(DW_SYS_STATUS_RXFCG, timeout, &status) != 0) {
		dw_trx_off();
		dw_stats.rx_timeout++;
		return -EAGAIN;
	}

	/* O receptor desliga sozinho depois de um quadro bom. */
	dw_rx_on = false;

	finfo = dw_read32(DW_REG_RX_FINFO, 0);
	len = finfo & DW_RX_FINFO_RXFLEN_MASK;
	len = (len >= 2) ? len - 2 : 0;

	if (len > size) {
		dw_write32(DW_REG_SYS_STATUS, 0, DW_SYS_STATUS_ALL_RX_GOOD);
		dw_stats.rx_too_big++;
		return -EMSGSIZE;
	}

	(void)dw_read(DW_REG_RX_BUFFER, 0, buf, len);

	if (info != NULL) {
		uint8_t rx_time[9];
		uint8_t fqual[8];
		uint16_t rxpacc = (finfo >> DW_RX_FINFO_RXPACC_SHIFT) & DW_RX_FINFO_RXPACC_MASK;
		float f1, f2, f3, cir;

		(void)dw_read(DW_REG_RX_TIME, 0, rx_time, sizeof(rx_time));
		(void)dw_read(DW_REG_RX_FQUAL, 0, fqual, sizeof(fqual));

		if (rxpacc == dw_read16(DW_REG_DRX_CONF, DW_DRX_RXPACC_NOSAT) &&
		    rxpacc > DW_RXPACC_SFD_CORRECTION) {
			rxpacc -= DW_RXPACC_SFD_CORRECTION;
		}

		f1 = sys_get_le16(&rx_time[DW_RX_TIME_FP_AMPL1]);
		f2 = sys_get_le16(&fqual[2]);
		f3 = sys_get_le16(&fqual[4]);
		cir = sys_get_le16(&fqual[6]);

		info->len = (uint16_t)len;
		info->rx_ts = sys_get_le40(rx_time);
		info->rx_power_cdbm = dw_power_cdbm(cir * 131072.0f, rxpacc);
		info->fp_power_cdbm = dw_power_cdbm(f1 * f1 + f2 * f2 + f3 * f3, rxpacc);
	}

	dw_write32(DW_REG_SYS_STATUS, 0, DW_SYS_STATUS_ALL_RX_GOOD);
	dw_stats.rx_ok++;
	return (int)len;
}

uint64_t dw1000_sys_time(void)
{
	return dw_read40(DW_REG_SYS_TIME, 0);
}

void dw1000_idle(void)
{
	dw_trx_off();
}

void dw1000_get_stats(struct dw1000_stats *stats)
{
	*stats = dw_stats;
}
