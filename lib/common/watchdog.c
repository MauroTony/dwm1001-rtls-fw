#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <rtls/watchdog.h>

LOG_MODULE_REGISTER(rtls_wdt, LOG_LEVEL_INF);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;
static atomic_t hang_requested;

/* Causa do último reset, para o T8.2 (watchdog) e o T8.3 (nenhum reset em 1 h). */
static void log_reset_cause(void)
{
	uint32_t cause;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		return;
	}

	if (cause & RESET_WATCHDOG) {
		LOG_WRN("reset anterior: WATCHDOG");
	} else if (cause & RESET_SOFTWARE) {
		LOG_INF("reset anterior: software (rtls reboot ou J-Link)");
	} else if (cause & RESET_PIN) {
		LOG_INF("reset anterior: pino de reset");
	} else if (cause & RESET_CPU_LOCKUP) {
		LOG_WRN("reset anterior: CPU travada (lockup)");
	} else if (cause & RESET_DEBUG) {
		LOG_INF("reset anterior: depurador");
	} else {
		LOG_INF("reset anterior: energia (0x%08x)", cause);
	}
	(void)hwinfo_clear_reset_cause();
}

int rtls_watchdog_init(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = CONFIG_RTLS_WATCHDOG_TIMEOUT_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int err;

	log_reset_cause();

	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog indisponivel");
		return -ENODEV;
	}

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		return wdt_channel;
	}

	/* Parado pelo depurador, não dispara. */
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err) {
		wdt_channel = -1;
		return err;
	}

	LOG_INF("watchdog: %d ms", CONFIG_RTLS_WATCHDOG_TIMEOUT_MS);
	return 0;
}

void rtls_watchdog_feed(void)
{
	if (atomic_get(&hang_requested)) {
		/* Travamento de propósito: sem ceder a CPU e sem alimentar. */
		for (;;) {
			k_busy_wait(1000);
		}
	}

	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}

void rtls_watchdog_hang(void)
{
	atomic_set(&hang_requested, 1);
}
