/*
 * Watchdog da thread main (M8). Só a main alimenta: se o laço do rádio
 * travar, o nó reinicia em CONFIG_RTLS_WATCHDOG_TIMEOUT_MS.
 */

#ifndef RTLS_WATCHDOG_H_
#define RTLS_WATCHDOG_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registra no log a causa do último reset (watchdog, pino, software, energia)
 * e liga o watchdog. Depois disso ele não desliga mais até o próximo reset.
 */
int rtls_watchdog_init(void);

/** Alimenta o watchdog; chame a cada volta do laço da thread main. */
void rtls_watchdog_feed(void);

/** Teste do T8.2: a próxima alimentação trava a thread main de propósito. */
void rtls_watchdog_hang(void);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_WATCHDOG_H_ */
