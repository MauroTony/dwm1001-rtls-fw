/*
 * GPIO de debug para o analisador lógico (zephyr,user/debug-gpios; P0.15,
 * pino 3 do conector RPi da DWM1001-DEV). Na tag, fica alto durante a
 * rodada; no gateway, pulsa a cada POSITION recebido (T5.2, M3).
 * Sem CONFIG_RTLS_DEBUG_GPIO, as funções não fazem nada.
 */

#ifndef RTLS_DEBUG_GPIO_H_
#define RTLS_DEBUG_GPIO_H_

#include <stdbool.h>

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_RTLS_DEBUG_GPIO)

int rtls_debug_gpio_init(void);
void rtls_debug_gpio_set(bool on);

#else

static inline int rtls_debug_gpio_init(void)
{
	return 0;
}

static inline void rtls_debug_gpio_set(bool on)
{
	ARG_UNUSED(on);
}

#endif

/** Pulso curto (~1 µs): marca um instante. */
static inline void rtls_debug_gpio_pulse(void)
{
	rtls_debug_gpio_set(true);
	rtls_debug_gpio_set(false);
}

#ifdef __cplusplus
}
#endif

#endif /* RTLS_DEBUG_GPIO_H_ */
