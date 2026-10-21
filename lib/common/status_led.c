#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <rtls/status_led.h>

static const struct gpio_dt_spec led_error = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static const struct gpio_dt_spec led_heartbeat = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0});
static const struct gpio_dt_spec led_activity = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led3), gpios, {0});

static void heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (led_heartbeat.port != NULL) {
		gpio_pin_toggle_dt(&led_heartbeat);
	}
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry, NULL);

static int led_setup(const struct gpio_dt_spec *led)
{
	if (led->port == NULL) {
		return 0;
	}
	if (!gpio_is_ready_dt(led)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
}

int rtls_status_led_init(void)
{
	int err;

	err = led_setup(&led_error);
	if (err == 0) {
		err = led_setup(&led_heartbeat);
	}
	if (err == 0) {
		err = led_setup(&led_activity);
	}
	if (err) {
		return err;
	}

	k_timer_start(&heartbeat_timer, K_MSEC(CONFIG_RTLS_STATUS_LED_HEARTBEAT_MS),
		      K_MSEC(CONFIG_RTLS_STATUS_LED_HEARTBEAT_MS));
	return 0;
}

void rtls_status_led_error(bool on)
{
	if (led_error.port != NULL) {
		gpio_pin_set_dt(&led_error, on ? 1 : 0);
	}
}

void rtls_status_led_activity(void)
{
	if (led_activity.port != NULL) {
		gpio_pin_toggle_dt(&led_activity);
	}
}
