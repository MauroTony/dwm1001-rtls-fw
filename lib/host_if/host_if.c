#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rtls/host_if.h>

LOG_MODULE_REGISTER(rtls_host_if, CONFIG_RTLS_HOST_IF_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(rtls_host_spi), "defina o chosen rtls,host-spi no overlay");

static const struct device *const host_spi = DEVICE_DT_GET(DT_CHOSEN(rtls_host_spi));
static const struct gpio_dt_spec drdy = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), drdy_gpios);

int rtls_host_if_init(void)
{
	int err;

	if (!device_is_ready(host_spi) || !gpio_is_ready_dt(&drdy)) {
		LOG_ERR("SPIS/DRDY indisponiveis");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&drdy, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	/*
	 * TODO(M6): thread host_if com laço de spi_transceive() em modo
	 * escravo (SPI_OP_MODE_SLAVE), máquina de estados
	 * IDLE -> REQ -> META -> DATA (docs/05-interface-spi-gateway.md, seção 2).
	 */
	LOG_INF("host_if pronto (SPIS %s)", host_spi->name);
	return 0;
}

int rtls_host_if_publish(const struct rtls_tag_record *records, size_t count)
{
	ARG_UNUSED(records);
	ARG_UNUSED(count);

	/* TODO(M6): copiar o snapshot sob mutex, paginar, calcular CRC e subir o DRDY. */
	return -ENOSYS;
}
