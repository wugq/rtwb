/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BB/RF/MAC register definitions used by the PHY code, copied from Linux
 * rtw88 reg.h, rtw8822b.h and main.h.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#ifndef _RTW88_REG_PHY_H_
#define _RTW88_REG_PHY_H_









#define BIT_MAC_CLK_SEL		(BIT(20) | BIT(21))
#define BIT_RFMOD		(BIT(7) | BIT(8))
#define BIT_RFMOD_40M		BIT(7)
#define BIT_RFMOD_80M		BIT(8)
#define BIT_SHIFT_MAC_CLK_SEL	20
#define MAC_CLK_HW_DEF_80M	0
#define MAC_CLK_SPEED		80
#define REG_AFE_CTRL1		0x0024
#define REG_CCK_CHECK		0x0454
#define REG_DATA_SC		0x0483
#define REG_USTIME_EDCA		0x0638
#define REG_USTIME_TSF		0x055C
#define REG_WMAC_TRXPTCL_CTL	0x0668

#define BIT_CHECK_CCK_EN	BIT(7)
#define	MASKBYTE1		0xff00
#define REG_RFECTL	0xcb8
#define REG_RFESEL0	0xcb0
#define REG_RFESEL8	0xcb4

#define	MASKLWORD		0x0000ffff
#define REG_RFEINV	0xcbc
#define REG_TRSW	0xca0

#define	MASKBYTE0		0xff
#define	MASKDWORD		0xffffffff
#define REG_CCA2ND		0x0838
#define REG_CCASEL		0x082C
#define REG_L1WT	0x83c
#define REG_PDMFTH		0x0830
#define RF_LUTDBG	0xdf
#define RF_MALSEL	0xbe
#define RFREG_MASK			0xfffff

#define REG_ACBB0	0x948
#define REG_ACBBRXFIR	0x94c
#define REG_RXIGI_A		0x0c50
#define REG_RXIGI_B		0x0e50
#define REG_RXPSEL		0x0808
#define REG_TXDFIR	0xc20
#define RF_XTALX2	0xb8

#define BIT_EN_MU_MIMO				BIT(7)
#define BIT_MASK_CSI_RATE (BIT_MASK_CSI_RATE_VAL << BIT_SHIFT_CSI_RATE)
#define BIT_MASK_R_MU_RL (R_MU_RL << BIT_SHIFT_R_MU_RL)
#define BIT_MASK_R_MU_TABLE_VALID	0x3f
#define BIT_MU_P1_WAIT_STATE_EN			BIT(16)
#define BIT_RX_PSEL_RST		(BIT(28) | BIT(29))
#define BIT_SHIFT_R_MU_RL		12
#define BIT_SHIFT_WMAC_TXMU_ACKPOLICY	4
#define BIT_TXSC_20M(x)							       \
	(((x) & BIT_MASK_TXSC_20M) << BIT_SHIFT_TXSC_20M)
#define BIT_TXSC_40M(x)							       \
	(((x) & BIT_MASK_TXSC_40M) << BIT_SHIFT_TXSC_40M)
#define BIT_USE_NDPA_PARAMETER			BIT(30)
#define BIT_WMAC_TXMU_ACKPOLICY_EN		BIT(6)
#define REG_ACGG2TBL	0x958
#define REG_ADC160		0x08C4
#define REG_ADC40	0x8c8
#define REG_ADCCLK		0x08AC
#define REG_ADCINI	0xa04
#define REG_AGCTR_A	0xc08
#define REG_AGCTR_B	0xe08
#define REG_ANTWT	0x1904
#define REG_BBPSF_CTRL		0x06DC
#define REG_CDDTXP	0x93c
#define REG_CLKTRK		0x0860
#define REG_ENTXCCK	0xa80
#define REG_HTSTFWT	0x800
#define REG_L1PKWT	0x840
#define REG_MRC		0x850
#define REG_MU_TX_CTL			0x14C0
#define REG_NDPA_OPT_CTRL	0x045F
#define REG_RXCCAMSK	0x814
#define REG_RXDESC	0xa2c
#define REG_RXSB		0x0a00
#define REG_TXBF_CTRL		0x042C
#define REG_TXPSEL		0x080C
#define REG_TXPSEL1	0x940
#define REG_TXSF2	0xa24
#define REG_TXSF6	0xa28
#define REG_WMAC_MU_BF_CTL		0x1680
#define REG_WMAC_MU_BF_OPTION		0x167C
#define RF_LUTWA	0x33
#define RF_LUTWD0	0x3f
#define RF_LUTWD1	0x3e
#define RF_LUTWE	0xef

#define BIT_MASK_CSI_RATE_VAL		0x3F
#define BIT_MASK_TXSC_20M	0xf
#define BIT_MASK_TXSC_40M	0xf
#define BIT_SHIFT_CSI_RATE		24
#define BIT_SHIFT_TXSC_20M	0
#define BIT_SHIFT_TXSC_40M	4
#define R_MU_RL				0xf

#define BIT_BTGP_JTAG_EN	BIT(24)
#define BIT_BTGP_SPI_EN		BIT(20)
#define BIT_BT_INT_EN		BIT(15)
#define BIT_BT_PTA_EN		BIT(5)
#define BIT_DBG_GNT_WL_BT	BIT(27)
#define BIT_DPDT_SEL_EN		BIT(23)
#define BIT_DPDT_WL_SEL		BIT(24)
#define BIT_GNT_BT_POLARITY	BIT(12)
#define BIT_LED1DIS		BIT(15)
#define BIT_LTE_COEX_EN		BIT(7)
#define BIT_LTE_MUX_CTRL_PATH	BIT(26)
#define BIT_MASK_RFE_INV89	GENMASK(1, 0)
#define BIT_MASK_RFE_SEL89	GENMASK(7, 0)
#define BIT_MASK_SAMPLE_RATE	GENMASK(5, 0)
#define BIT_PO_BT_PTA_PINS	BIT(9)
#define BIT_PTA_EDCCA_EN	BIT(5)
#define BIT_PTA_WL_TX_EN	BIT(4)
#define BIT_RFE_BUF_EN		BIT(3)
#define BIT_SW_DPDT_SEL_DATA	BIT(0)
#define LTE_BT_TRX_CTRL	0xa4
#define LTE_COEX_CTRL	0x38
#define LTE_WL_TRX_CTRL	0xa0
#define REG_BT_COEX_BRK_TABLE	0x06C8
#define REG_BT_COEX_TABLE0	0x06C0
#define REG_BT_COEX_TABLE1	0x06C4
#define REG_BT_COEX_TABLE_H	0x06CC
#define REG_BT_COEX_V2		0x0762
#define REG_BT_STAT_CTRL	0x0778
#define REG_BT_TDMA_TIME	0x0790
#define REG_QUEUE_CTRL		0x04C6
#define REG_RFE_CTRL8		0x0cb4
#define REG_RFE_CTRL_E		0x0974
#define REG_RFE_INV16		0x0cbe
#define REG_RFE_INV8		0x0cbd
#define REG_RFESEL_CTRL	0x1990
#define REG_SYS_SDIO_CTRL	0x0070
#define REG_WIFI_BT_INFO	0x00AA







#endif /* _RTW88_REG_PHY_H_ */
