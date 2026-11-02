#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <rtls/debug_gpio.h>

LOG_MODULE_REGISTER(rtls_dbg, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), debug_gpios),
	     "defina zephyr,user/debug-gpios no overlay do app");

static const struct gpio_dt_spec dbg = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), debug_gpios);
static bool dbg_ready;

int rtls_debug_gpio_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&dbg)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&dbg, GPIO_OUTPUT_INACTIVE);
	if (err == 0) {
		dbg_ready = true;
		LOG_INF("GPIO de debug: P0.%u", dbg.pin);
	}
	return err;
}

void rtls_debug_gpio_set(bool on)
{
	if (dbg_ready) {
		(void)gpio_pin_set_dt(&dbg, on);
	}
}
