/*
 * LEDs de status comuns aos três papéis.
 *
 *   led1 (verde)    heartbeat periódico: firmware vivo
 *   led3 (azul)     atividade UWB: alterna a cada evento relevante
 *   led0 (vermelho) erro persistente
 */

#ifndef RTLS_STATUS_LED_H_
#define RTLS_STATUS_LED_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configura os LEDs e inicia o heartbeat. */
int rtls_status_led_init(void);

/** Acende/apaga o LED de erro. */
void rtls_status_led_error(bool on);

/** Alterna o LED de atividade UWB. */
void rtls_status_led_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_STATUS_LED_H_ */
