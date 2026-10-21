/*
 * Mapa de registradores do DW1000 usado pelo driver (somente o necessário).
 * Fonte: DW1000 User Manual v2.18, capítulo 7 (Register Map) e seção 2.5.5
 * (configurações padrão que devem ser alteradas).
 */

#ifndef DW1000_REGS_H_
#define DW1000_REGS_H_

#include <zephyr/sys/util.h>

/* Identificadores dos arquivos de registradores. */
#define DW_REG_DEV_ID     0x00
#define DW_REG_PANADR     0x03
#define DW_REG_SYS_CFG    0x04
#define DW_REG_SYS_TIME   0x06
#define DW_REG_TX_FCTRL   0x08
#define DW_REG_TX_BUFFER  0x09
#define DW_REG_DX_TIME    0x0A
#define DW_REG_SYS_CTRL   0x0D
#define DW_REG_SYS_MASK   0x0E
#define DW_REG_SYS_STATUS 0x0F
#define DW_REG_RX_FINFO   0x10
#define DW_REG_RX_BUFFER  0x11
#define DW_REG_RX_FQUAL   0x12
#define DW_REG_RX_TIME    0x15
#define DW_REG_TX_TIME    0x17
#define DW_REG_TX_ANTD    0x18
#define DW_REG_ACK_RESP_T 0x1A
#define DW_REG_TX_POWER   0x1E
#define DW_REG_CHAN_CTRL  0x1F
#define DW_REG_AGC_CTRL   0x23
#define DW_REG_EXT_SYNC   0x24
#define DW_REG_DRX_CONF   0x27
#define DW_REG_RF_CONF    0x28
#define DW_REG_TX_CAL     0x2A
#define DW_REG_FS_CTRL    0x2B
#define DW_REG_AON        0x2C
#define DW_REG_OTP_IF     0x2D
#define DW_REG_LDE_IF     0x2E
#define DW_REG_PMSC       0x36

/* Sub-endereços. */
#define DW_AGC_TUNE1        0x04
#define DW_AGC_TUNE2        0x0C
#define DW_EC_CTRL          0x00
#define DW_DRX_TUNE0B       0x02
#define DW_DRX_TUNE1A       0x04
#define DW_DRX_TUNE1B       0x06
#define DW_DRX_TUNE2        0x08
#define DW_DRX_SFDTOC       0x20
#define DW_DRX_TUNE4H       0x26
#define DW_DRX_RXPACC_NOSAT 0x2C
#define DW_RF_RXCTRLH       0x0B
#define DW_RF_TXCTRL        0x0C
#define DW_TC_PGDELAY       0x0B
#define DW_FS_PLLCFG        0x07
#define DW_FS_PLLTUNE       0x0B
#define DW_FS_XTALT         0x0E
#define DW_AON_CFG1         0x0A
#define DW_OTP_ADDR         0x04
#define DW_OTP_CTRL         0x06
#define DW_OTP_RDAT         0x0A
#define DW_OTP_SF           0x12
#define DW_LDE_CFG1         0x0806
#define DW_LDE_RXANTD       0x1804
#define DW_LDE_CFG2         0x1806
#define DW_LDE_REPC         0x2804
#define DW_PMSC_CTRL0       0x00
#define DW_RX_TIME_FP_AMPL1 0x07

/* SYS_CFG */
#define DW_SYS_CFG_FFEN      BIT(0)
#define DW_SYS_CFG_FFAD      BIT(3)
#define DW_SYS_CFG_HIRQ_POL  BIT(9)
#define DW_SYS_CFG_DIS_DRXB  BIT(12)
#define DW_SYS_CFG_PHR_MODE  (BIT(16) | BIT(17))
#define DW_SYS_CFG_DIS_STXP  BIT(18)
#define DW_SYS_CFG_RXM110K   BIT(22)
#define DW_SYS_CFG_RXWTOE    BIT(28)
#define DW_SYS_CFG_RXAUTR    BIT(29)

/* SYS_CTRL */
#define DW_SYS_CTRL_TXSTRT    BIT(1)
#define DW_SYS_CTRL_TXDLYS    BIT(2)
#define DW_SYS_CTRL_TRXOFF    BIT(6)
#define DW_SYS_CTRL_WAIT4RESP BIT(7)
#define DW_SYS_CTRL_RXENAB    BIT(8)

/* SYS_STATUS / SYS_MASK (mesmas posições) */
#define DW_SYS_STATUS_CPLOCK  BIT(1)
#define DW_SYS_STATUS_AAT     BIT(3)
#define DW_SYS_STATUS_TXFRB   BIT(4)
#define DW_SYS_STATUS_TXPRS   BIT(5)
#define DW_SYS_STATUS_TXPHS   BIT(6)
#define DW_SYS_STATUS_TXFRS   BIT(7)
#define DW_SYS_STATUS_RXPRD   BIT(8)
#define DW_SYS_STATUS_RXSFDD  BIT(9)
#define DW_SYS_STATUS_LDEDONE BIT(10)
#define DW_SYS_STATUS_RXPHD   BIT(11)
#define DW_SYS_STATUS_RXPHE   BIT(12)
#define DW_SYS_STATUS_RXDFR   BIT(13)
#define DW_SYS_STATUS_RXFCG   BIT(14)
#define DW_SYS_STATUS_RXFCE   BIT(15)
#define DW_SYS_STATUS_RXRFSL  BIT(16)
#define DW_SYS_STATUS_RXRFTO  BIT(17)
#define DW_SYS_STATUS_LDEERR  BIT(18)
#define DW_SYS_STATUS_RXPTO   BIT(21)
#define DW_SYS_STATUS_RXSFDTO BIT(26)
#define DW_SYS_STATUS_HPDWARN BIT(27)
#define DW_SYS_STATUS_AFFREJ  BIT(29)

#define DW_SYS_STATUS_ALL_TX                                                                      \
	(DW_SYS_STATUS_AAT | DW_SYS_STATUS_TXFRB | DW_SYS_STATUS_TXPRS | DW_SYS_STATUS_TXPHS |    \
	 DW_SYS_STATUS_TXFRS)
#define DW_SYS_STATUS_ALL_RX_GOOD                                                                 \
	(DW_SYS_STATUS_RXDFR | DW_SYS_STATUS_RXFCG | DW_SYS_STATUS_RXPRD | DW_SYS_STATUS_RXSFDD |  \
	 DW_SYS_STATUS_RXPHD | DW_SYS_STATUS_LDEDONE)
#define DW_SYS_STATUS_ALL_RX_ERR                                                                  \
	(DW_SYS_STATUS_RXPHE | DW_SYS_STATUS_RXFCE | DW_SYS_STATUS_RXRFSL | DW_SYS_STATUS_RXSFDTO | \
	 DW_SYS_STATUS_AFFREJ | DW_SYS_STATUS_LDEERR)
#define DW_SYS_STATUS_ALL_RX_TO (DW_SYS_STATUS_RXRFTO | DW_SYS_STATUS_RXPTO)

/* Leitura de 16 bits no offset 3 de SYS_STATUS: HPDWARN (bit 27) e TXPUTE (bit 34). */
#define DW_SYS_STATUS_HI16_TXERR 0x0408

/* TX_FCTRL */
#define DW_TX_FCTRL_TXBR_6M8 (2U << 13)
#define DW_TX_FCTRL_TR       BIT(15)
#define DW_TX_FCTRL_PRF_64M  (2U << 16)
#define DW_TX_FCTRL_PLEN_128 (0x5U << 18) /* TXPSR = 01, PE = 01 */

/* CHAN_CTRL */
#define DW_CHAN_CTRL_TX_CHAN_SHIFT  0
#define DW_CHAN_CTRL_RX_CHAN_SHIFT  4
#define DW_CHAN_CTRL_RXPRF_64M      (2U << 18)
#define DW_CHAN_CTRL_TX_PCODE_SHIFT 22
#define DW_CHAN_CTRL_RX_PCODE_SHIFT 27

/* RX_FINFO */
#define DW_RX_FINFO_RXFLEN_MASK  0x3FFU
#define DW_RX_FINFO_RXPACC_SHIFT 20
#define DW_RX_FINFO_RXPACC_MASK  0xFFFU

/* EC_CTRL */
#define DW_EC_CTRL_PLLLCK BIT(2)

/* OTP */
#define DW_OTP_CTRL_OTPRDEN  BIT(0)
#define DW_OTP_CTRL_OTPREAD  BIT(1)
#define DW_OTP_CTRL_LDELOAD  BIT(15)
#define DW_OTP_SF_LDO_KICK   BIT(1)
#define DW_OTP_ADDR_LDOTUNE  0x04
#define DW_OTP_ADDR_PARTID   0x06
#define DW_OTP_ADDR_ANTDLY   0x1C
#define DW_OTP_ADDR_XTALTRIM 0x1E

/* FS_XTALT: bits 7:5 reservados devem valer 011. */
#define DW_FS_XTALT_RESERVED 0x60
#define DW_FS_XTALT_MASK     0x1F

/* Constante A da estimativa de potência (User Manual 4.7.2), PRF de 64 MHz. */
#define DW_RX_POWER_A_PRF64 121.74f

#endif /* DW1000_REGS_H_ */
