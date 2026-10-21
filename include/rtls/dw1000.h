/*
 * API do driver DW1000 próprio do RTLS.
 *
 * Driver singleton ligado ao nó "decawave,dw1000" do devicetree da placa.
 * Todas as funções devem ser chamadas pela thread dona do rádio (ver
 * docs/02-arquitetura.md, seção 2); nenhuma é segura em contexto de ISR.
 *
 * Estado: M0 implementa reset + probe (DEV_ID). O restante entra no M1.
 */

#ifndef RTLS_DW1000_H_
#define RTLS_DW1000_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Valor esperado do registrador DEV_ID (RIDTAG 0xDECA, modelo 0x01, ver 3, rev 0). */
#define DW1000_DEV_ID 0xDECA0130U

/** Máscara dos 40 bits válidos de um timestamp do DW1000. */
#define DW1000_TS_MASK 0xFFFFFFFFFFULL

/** Ticks por "UWB microsecond" (512/499,2 µs), unidade usada nos atrasos. */
#define DW1000_UUS_TO_TICKS 65536ULL

/** Parâmetros da camada física (docs/03-protocolo-uwb.md, seção 1). */
struct dw1000_phy_config {
	uint8_t channel;        /**< canal UWB (1-5, 7) */
	uint8_t prf_mhz;        /**< 16 ou 64 */
	uint16_t preamble_len;  /**< 64, 128, 256, 512, 1024, 1536, 2048 ou 4096 símbolos */
	uint8_t pac_size;       /**< 8, 16, 32 ou 64 */
	uint8_t preamble_code;  /**< código de preâmbulo (TX e RX) */
	uint16_t data_rate_kbps; /**< 110, 850 ou 6800 */
	uint16_t pan_id;        /**< PAN para a filtragem de quadros */
	uint16_t short_addr;    /**< endereço curto do nó */
};

/** Configuração padrão do projeto: canal 5, PRF 64, preâmbulo 128 (código 9), 6,8 Mbps. */
#define DW1000_PHY_CONFIG_DEFAULT(_pan, _addr)                                                    \
	{                                                                                          \
		.channel = 5, .prf_mhz = 64, .preamble_len = 128, .pac_size = 8,                   \
		.preamble_code = 9, .data_rate_kbps = 6800, .pan_id = (_pan),                      \
		.short_addr = (_addr),                                                             \
	}

/** Opções de transmissão. */
struct dw1000_tx_opts {
	/** Instante de TX (ticks, 40 bits) para TX atrasado; 0 = imediato. */
	uint64_t delayed_ts;
	/** Liga o RX automaticamente após o TX (wait-for-response). */
	bool rx_after_tx;
	/** Atraso entre o fim do TX e o RX ligar, em UWB µs (rx_after_tx). */
	uint32_t rx_delay_uus;
};

/** Metadados de um quadro recebido. */
struct dw1000_rx_info {
	uint16_t len;           /**< bytes do quadro sem o FCS */
	uint64_t rx_ts;         /**< timestamp de RX (ticks, 40 bits, já sem atraso de antena) */
	int16_t rx_power_cdbm;  /**< potência total estimada (centésimos de dBm) */
	int16_t fp_power_cdbm;  /**< potência do primeiro caminho (centésimos de dBm) */
};

/**
 * Reseta o DW1000, confere o DEV_ID e aplica a configuração base.
 *
 * @retval 0 sucesso
 * @retval -ENODEV barramento indisponível ou DEV_ID inesperado
 */
int dw1000_init(void);

/** Lê o registrador DEV_ID (útil para diagnóstico). */
int dw1000_read_dev_id(uint32_t *dev_id);

/** Aplica a configuração de PHY e da filtragem de quadros. TODO(M1) */
int dw1000_configure(const struct dw1000_phy_config *cfg);

/** Define os atrasos de antena (ticks). TODO(M1) */
int dw1000_set_antenna_delay(uint16_t tx_ticks, uint16_t rx_ticks);

/**
 * Transmite um quadro e bloqueia até o fim do TX.
 *
 * @param frame quadro sem FCS (o DW1000 acrescenta os 2 bytes)
 * @param len   tamanho de @p frame
 * @param opts  opções (NULL = TX imediato, sem RX depois)
 * @param tx_ts timestamp real de TX (ticks), pode ser NULL
 *
 * @retval -ETIME o instante do TX atrasado já tinha passado (HPDWARN)
 *
 * TODO(M1)
 */
int dw1000_transmit(const void *frame, size_t len, const struct dw1000_tx_opts *opts,
		    uint64_t *tx_ts);

/**
 * Liga o RX (se ainda não estiver ligado) e bloqueia até receber um quadro
 * válido, ocorrer erro ou estourar @p timeout.
 *
 * @return tamanho do quadro (>= 0) ou erro negativo (-EAGAIN no timeout)
 *
 * TODO(M1)
 */
int dw1000_receive(void *buf, size_t size, struct dw1000_rx_info *info, k_timeout_t timeout);

/** Lê o relógio de sistema do DW1000 (ticks, 40 bits). TODO(M1) */
uint64_t dw1000_sys_time(void);

/** Desliga TX/RX imediatamente (TRXOFF). TODO(M1) */
void dw1000_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* RTLS_DW1000_H_ */
