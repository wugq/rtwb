/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 rtw8822b.c: chip-specific MAC setup, BB/RF
 * init (phy_set_param), TRX path mode and channel switching; plus
 * rtw_set_channel_mac() and rtw_update_channel() from mac.c/main.c and
 * rtw_bf_phy_init() from bf.c.  The functions from rtw8822b_phy_rfe_init()
 * on are copied verbatim except where marked "rtwb:".
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#include "rtw88_phy.h"

#define WLAN_SLOT_TIME		0x09
#define WLAN_PIFS_TIME		0x19
#define WLAN_SIFS_CCK_CONT_TX	0xA
#define WLAN_SIFS_OFDM_CONT_TX	0xE
#define WLAN_SIFS_CCK_TRX	0x10
#define WLAN_SIFS_OFDM_TRX	0x10
#define WLAN_VO_TXOP_LIMIT	0x186 /* unit : 32us */
#define WLAN_VI_TXOP_LIMIT	0x3BC /* unit : 32us */
#define WLAN_RDG_NAV		0x05
#define WLAN_TXOP_NAV		0x1B
#define WLAN_CCK_RX_TSF		0x30
#define WLAN_OFDM_RX_TSF	0x30
#define WLAN_TBTT_PROHIBIT	0x04 /* unit : 32us */
#define WLAN_TBTT_HOLD_TIME	0x064 /* unit : 32us */
#define WLAN_DRV_EARLY_INT	0x04
#define WLAN_BCN_DMA_TIME	0x02

#define WLAN_RX_FILTER0		0x0FFFFFFF
#define WLAN_RX_FILTER2		0xFFFF
#define WLAN_RCR_CFG		0xE400220E
#define WLAN_RXPKT_MAX_SZ	12288
#define WLAN_RXPKT_MAX_SZ_512	(WLAN_RXPKT_MAX_SZ >> 9)

#define WLAN_AMPDU_MAX_TIME		0x70
#define WLAN_RTS_LEN_TH			0xFF
#define WLAN_RTS_TX_TIME_TH		0x08
#define WLAN_MAX_AGG_PKT_LIMIT		0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT	0x20
#define FAST_EDCA_VO_TH		0x06
#define FAST_EDCA_VI_TH		0x06
#define FAST_EDCA_BE_TH		0x06
#define FAST_EDCA_BK_TH		0x06
#define WLAN_BAR_RETRY_LIMIT		0x01
#define WLAN_RA_TRY_RATE_AGG_LIMIT	0x08

#define WLAN_TX_FUNC_CFG1		0x30
#define WLAN_TX_FUNC_CFG2		0x30
#define WLAN_MAC_OPT_NORM_FUNC1		0x98
#define WLAN_MAC_OPT_LB_FUNC1		0x80
#define WLAN_MAC_OPT_FUNC2		0xb0810041

#define WLAN_SIFS_CFG	(WLAN_SIFS_CCK_CONT_TX | \
			(WLAN_SIFS_OFDM_CONT_TX << BIT_SHIFT_SIFS_OFDM_CTX) | \
			(WLAN_SIFS_CCK_TRX << BIT_SHIFT_SIFS_CCK_TRX) | \
			(WLAN_SIFS_OFDM_TRX << BIT_SHIFT_SIFS_OFDM_TRX))

#define WLAN_TBTT_TIME	(WLAN_TBTT_PROHIBIT |\
			(WLAN_TBTT_HOLD_TIME << BIT_SHIFT_TBTT_HOLD_TIME_AP))

#define WLAN_NAV_CFG		(WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG		(WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)

int
rtw8822b_mac_init(struct rtwb_softc *sc)
{
	uint32_t value32;

	/* protocol configuration */
	rtwb_setbits_1(sc, REG_SW_AMPDU_BURST_MODE_CTRL, BIT_PRE_TX_CMD, 0);
	rtwb_write_1(sc, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
	rtwb_setbits_1(sc, REG_TX_HANG_CTRL, 0, BIT_EN_EOF_V1);
	value32 = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
		  (WLAN_MAX_AGG_PKT_LIMIT << 16) |
		  (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
	rtwb_write_4(sc, REG_PROT_MODE_CTRL, value32);
	rtwb_write_2(sc, REG_BAR_MODE_CTRL + 2,
		    WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
	rtwb_write_1(sc, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
	rtwb_write_1(sc, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
	rtwb_write_1(sc, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
	rtwb_write_1(sc, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
	/* EDCA configuration */
	rtwb_setbits_1(sc, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0, 0);
	rtwb_write_2(sc, REG_TXPAUSE, 0x0000);
	rtwb_write_1(sc, REG_SLOT, WLAN_SLOT_TIME);
	rtwb_write_1(sc, REG_PIFS, WLAN_PIFS_TIME);
	rtwb_write_4(sc, REG_SIFS, WLAN_SIFS_CFG);
	rtwb_write_2(sc, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
	rtwb_write_2(sc, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
	rtwb_write_4(sc, REG_RD_NAV_NXT, WLAN_NAV_CFG);
	rtwb_write_2(sc, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
	/* Set beacon cotnrol - enable TSF and other related functions */
	rtwb_setbits_1(sc, REG_BCN_CTRL, 0, BIT_EN_BCN_FUNCTION);
	/* Set send beacon related registers */
	rtwb_write_4(sc, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
	rtwb_write_1(sc, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
	rtwb_write_1(sc, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
	rtwb_setbits_1(sc, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8, 0);
	/* WMAC configuration */
	rtwb_write_4(sc, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtwb_write_2(sc, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtwb_write_4(sc, REG_RCR, WLAN_RCR_CFG);
	rtwb_write_1(sc, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
	rtwb_write_1(sc, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
	rtwb_write_1(sc, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
	rtwb_write_4(sc, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
	rtwb_write_1(sc, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);
	rtwb_setbits_1(sc, REG_SND_PTCL_CTRL, 0, BIT_DIS_CHK_VHTSIGB_CRC);

	return (0);
}

static void rtw8822b_phy_rfe_init(struct rtw_dev *rtwdev)
{
	/* chip top mux */
	rtw_write32_mask(rtwdev, 0x64, BIT(29) | BIT(28), 0x3);
	rtw_write32_mask(rtwdev, 0x4c, BIT(26) | BIT(25), 0x0);
	rtw_write32_mask(rtwdev, 0x40, BIT(2), 0x1);

	/* from s0 or s1 */
	rtw_write32_mask(rtwdev, 0x1990, 0x3f, 0x30);
	rtw_write32_mask(rtwdev, 0x1990, (BIT(11) | BIT(10)), 0x3);

	/* input or output */
	rtw_write32_mask(rtwdev, 0x974, 0x3f, 0x3f);
	rtw_write32_mask(rtwdev, 0x974, (BIT(11) | BIT(10)), 0x3);
}

void rtw_set_channel_mac(struct rtw_dev *rtwdev, u8 channel, u8 bw,
			 u8 primary_ch_idx)
{
	u8 txsc40 = 0, txsc20 = 0;
	u32 value32;
	u8 value8;

	txsc20 = primary_ch_idx;
	if (bw == RTW_CHANNEL_WIDTH_80) {
		if (txsc20 == RTW_SC_20_UPPER || txsc20 == RTW_SC_20_UPMOST)
			txsc40 = RTW_SC_40_UPPER;
		else
			txsc40 = RTW_SC_40_LOWER;
	}
	rtw_write8(rtwdev, REG_DATA_SC,
		   BIT_TXSC_20M(txsc20) | BIT_TXSC_40M(txsc40));

	value32 = rtw_read32(rtwdev, REG_WMAC_TRXPTCL_CTL);
	value32 &= ~BIT_RFMOD;
	switch (bw) {
	case RTW_CHANNEL_WIDTH_80:
		value32 |= BIT_RFMOD_80M;
		break;
	case RTW_CHANNEL_WIDTH_40:
		value32 |= BIT_RFMOD_40M;
		break;
	case RTW_CHANNEL_WIDTH_20:
	default:
		break;
	}
	rtw_write32(rtwdev, REG_WMAC_TRXPTCL_CTL, value32);

	if (rtw_chip_wcpu_8051(rtwdev))
		return;

	value32 = rtw_read32(rtwdev, REG_AFE_CTRL1) & ~(BIT_MAC_CLK_SEL);
	value32 |= (MAC_CLK_HW_DEF_80M << BIT_SHIFT_MAC_CLK_SEL);
	rtw_write32(rtwdev, REG_AFE_CTRL1, value32);

	rtw_write8(rtwdev, REG_USTIME_TSF, MAC_CLK_SPEED);
	rtw_write8(rtwdev, REG_USTIME_EDCA, MAC_CLK_SPEED);

	value8 = rtw_read8(rtwdev, REG_CCK_CHECK);
	value8 = value8 & ~BIT_CHECK_CCK_EN;
	if (IS_CH_5G_BAND(channel))
		value8 |= BIT_CHECK_CCK_EN;
	rtw_write8(rtwdev, REG_CCK_CHECK, value8);
}


static void rtw8822b_set_channel_rfe_efem(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_hal *hal = &rtwdev->hal;

	if (IS_CH_2G_BAND(channel)) {
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x705770);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x57);
		rtw_write32s_mask(rtwdev, REG_RFECTL, BIT(4), 0);
	} else {
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x177517);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x75);
		rtw_write32s_mask(rtwdev, REG_RFECTL, BIT(5), 0);
	}

	rtw_write32s_mask(rtwdev, REG_RFEINV, BIT(11) | BIT(10) | 0x3f, 0x0);

	if (hal->antenna_rx == BB_PATH_AB ||
	    hal->antenna_tx == BB_PATH_AB) {
		/* 2TX or 2RX */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa501);
	} else if (hal->antenna_rx == hal->antenna_tx) {
		/* TXA+RXA or TXB+RXB */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa500);
	} else {
		/* TXB+RXA or TXA+RXB */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa005);
	}
}

static void rtw8822b_set_channel_rfe_ifem(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_hal *hal = &rtwdev->hal;

	if (IS_CH_2G_BAND(channel)) {
		/* signal source */
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x745774);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x57);
	} else {
		/* signal source */
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x477547);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x75);
	}

	rtw_write32s_mask(rtwdev, REG_RFEINV, BIT(11) | BIT(10) | 0x3f, 0x0);

	if (IS_CH_2G_BAND(channel)) {
		if (hal->antenna_rx == BB_PATH_AB ||
		    hal->antenna_tx == BB_PATH_AB) {
			/* 2TX or 2RX */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa501);
		} else if (hal->antenna_rx == hal->antenna_tx) {
			/* TXA+RXA or TXB+RXB */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa500);
		} else {
			/* TXB+RXA or TXA+RXB */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa005);
		}
	} else {
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa5a5);
	}
}

enum {
	CCUT_IDX_1R_2G,
	CCUT_IDX_2R_2G,
	CCUT_IDX_1R_5G,
	CCUT_IDX_2R_5G,
	CCUT_IDX_NR,
};

struct cca_ccut {
	u32 reg82c[CCUT_IDX_NR];
	u32 reg830[CCUT_IDX_NR];
	u32 reg838[CCUT_IDX_NR];
};

static const struct cca_ccut cca_ifem_ccut = {
	{0x75C97010, 0x75C97010, 0x75C97010, 0x75C97010}, /*Reg82C*/
	{0x79a0eaaa, 0x79A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /*Reg830*/
	{0x87765541, 0x87746341, 0x87765541, 0x87746341}, /*Reg838*/
};

static const struct cca_ccut cca_efem_ccut = {
	{0x75B86010, 0x75B76010, 0x75B86010, 0x75B76010}, /*Reg82C*/
	{0x79A0EAA8, 0x79A0EAAC, 0x79A0EAA8, 0x79a0eaaa}, /*Reg830*/
	{0x87766451, 0x87766431, 0x87766451, 0x87766431}, /*Reg838*/
};

static const struct cca_ccut cca_ifem_ccut_ext = {
	{0x75da8010, 0x75da8010, 0x75da8010, 0x75da8010}, /*Reg82C*/
	{0x79a0eaaa, 0x97A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /*Reg830*/
	{0x87765541, 0x86666341, 0x87765561, 0x86666361}, /*Reg838*/
};

static void rtw8822b_get_cca_val(const struct cca_ccut *cca_ccut, u8 col,
				 u32 *reg82c, u32 *reg830, u32 *reg838)
{
	*reg82c = cca_ccut->reg82c[col];
	*reg830 = cca_ccut->reg830[col];
	*reg838 = cca_ccut->reg838[col];
}

struct rtw8822b_rfe_info {
	const struct cca_ccut *cca_ccut_2g;
	const struct cca_ccut *cca_ccut_5g;
	enum rtw_rfe_fem fem;
	bool ifem_ext;
	void (*rtw_set_channel_rfe)(struct rtw_dev *rtwdev, u8 channel);
};

#define I2GE5G_CCUT(set_ch) {						\
	.cca_ccut_2g = &cca_ifem_ccut,					\
	.cca_ccut_5g = &cca_efem_ccut,					\
	.fem = RTW_RFE_IFEM2G_EFEM5G,					\
	.ifem_ext = false,						\
	.rtw_set_channel_rfe = &rtw8822b_set_channel_rfe_ ## set_ch,	\
	}
#define IFEM_EXT_CCUT(set_ch) {						\
	.cca_ccut_2g = &cca_ifem_ccut_ext,				\
	.cca_ccut_5g = &cca_ifem_ccut_ext,				\
	.fem = RTW_RFE_IFEM,						\
	.ifem_ext = true,						\
	.rtw_set_channel_rfe = &rtw8822b_set_channel_rfe_ ## set_ch,	\
	}

static const struct rtw8822b_rfe_info rtw8822b_rfe_info[] = {
	[2] = I2GE5G_CCUT(efem),
	[3] = IFEM_EXT_CCUT(ifem),
	[5] = IFEM_EXT_CCUT(ifem),
};

static void rtw8822b_set_channel_cca(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				     const struct rtw8822b_rfe_info *rfe_info)
{
	struct rtw_hal *hal = &rtwdev->hal;
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct cca_ccut *cca_ccut;
	u8 col;
	u32 reg82c, reg830, reg838;
	bool is_efem_cca = false, is_ifem_cca = false, is_rfe_type = false;

	if (IS_CH_2G_BAND(channel)) {
		cca_ccut = rfe_info->cca_ccut_2g;

		if (hal->antenna_rx == BB_PATH_A ||
		    hal->antenna_rx == BB_PATH_B)
			col = CCUT_IDX_1R_2G;
		else
			col = CCUT_IDX_2R_2G;
	} else {
		cca_ccut = rfe_info->cca_ccut_5g;

		if (hal->antenna_rx == BB_PATH_A ||
		    hal->antenna_rx == BB_PATH_B)
			col = CCUT_IDX_1R_5G;
		else
			col = CCUT_IDX_2R_5G;
	}

	rtw8822b_get_cca_val(cca_ccut, col, &reg82c, &reg830, &reg838);

	switch (rfe_info->fem) {
	case RTW_RFE_IFEM:
	default:
		is_ifem_cca = true;
		if (rfe_info->ifem_ext)
			is_rfe_type = true;
		break;
	case RTW_RFE_EFEM:
		is_efem_cca = true;
		break;
	case RTW_RFE_IFEM2G_EFEM5G:
		if (IS_CH_2G_BAND(channel))
			is_ifem_cca = true;
		else
			is_efem_cca = true;
		break;
	}

	if (is_ifem_cca) {
		if ((hal->cut_version == RTW_CHIP_VER_CUT_B &&
		     (col == CCUT_IDX_2R_2G || col == CCUT_IDX_2R_5G) &&
		     bw == RTW_CHANNEL_WIDTH_40) ||
		    (!is_rfe_type && col == CCUT_IDX_2R_5G &&
		     bw == RTW_CHANNEL_WIDTH_40) ||
		    (efuse->rfe_option == 5 && col == CCUT_IDX_2R_5G))
			reg830 = 0x79a0ea28;
	}

	rtw_write32_mask(rtwdev, REG_CCASEL, MASKDWORD, reg82c);
	rtw_write32_mask(rtwdev, REG_PDMFTH, MASKDWORD, reg830);
	rtw_write32_mask(rtwdev, REG_CCA2ND, MASKDWORD, reg838);

	if (is_efem_cca && !(hal->cut_version == RTW_CHIP_VER_CUT_B))
		rtw_write32_mask(rtwdev, REG_L1WT, MASKDWORD, 0x9194b2b9);

	if (bw == RTW_CHANNEL_WIDTH_20 && IS_CH_5G_BAND_MID(channel))
		rtw_write32_mask(rtwdev, REG_CCA2ND, 0xf0, 0x4);
}

static const u8 low_band[15] = {0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff, 0x6,
				0x5, 0x0, 0x0, 0x7, 0x6, 0x6};
static const u8 middle_band[23] = {0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff, 0x0,
				   0x0, 0x7, 0x6, 0x6, 0x5, 0x0, 0xff, 0x7, 0x6,
				   0x6, 0x5, 0x0, 0x0, 0x7};
static const u8 high_band[15] = {0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff, 0x0,
				 0x7, 0x7, 0x6, 0x5, 0x5, 0x0};

static void rtw8822b_set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
#define RF18_BAND_MASK		(BIT(16) | BIT(9) | BIT(8))
#define RF18_BAND_2G		(0)
#define RF18_BAND_5G		(BIT(16) | BIT(8))
#define RF18_CHANNEL_MASK	(MASKBYTE0)
#define RF18_RFSI_MASK		(BIT(18) | BIT(17))
#define RF18_RFSI_GE_CH80	(BIT(17))
#define RF18_RFSI_GT_CH144	(BIT(18))
#define RF18_BW_MASK		(BIT(11) | BIT(10))
#define RF18_BW_20M		(BIT(11) | BIT(10))
#define RF18_BW_40M		(BIT(11))
#define RF18_BW_80M		(BIT(10))
#define RFBE_MASK		(BIT(17) | BIT(16) | BIT(15))

	struct rtw_hal *hal = &rtwdev->hal;
	u32 rf_reg18, rf_reg_be;

	rf_reg18 = rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

	rf_reg18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
		      RF18_BW_MASK);

	rf_reg18 |= (IS_CH_2G_BAND(channel) ? RF18_BAND_2G : RF18_BAND_5G);
	rf_reg18 |= (channel & RF18_CHANNEL_MASK);
	if (channel > 144)
		rf_reg18 |= RF18_RFSI_GT_CH144;
	else if (channel >= 80)
		rf_reg18 |= RF18_RFSI_GE_CH80;

	switch (bw) {
	case RTW_CHANNEL_WIDTH_5:
	case RTW_CHANNEL_WIDTH_10:
	case RTW_CHANNEL_WIDTH_20:
	default:
		rf_reg18 |= RF18_BW_20M;
		break;
	case RTW_CHANNEL_WIDTH_40:
		rf_reg18 |= RF18_BW_40M;
		break;
	case RTW_CHANNEL_WIDTH_80:
		rf_reg18 |= RF18_BW_80M;
		break;
	}

	if (IS_CH_2G_BAND(channel))
		rf_reg_be = 0x0;
	else if (IS_CH_5G_BAND_1(channel) || IS_CH_5G_BAND_2(channel))
		rf_reg_be = low_band[(channel - 36) >> 1];
	else if (IS_CH_5G_BAND_3(channel))
		rf_reg_be = middle_band[(channel - 100) >> 1];
	else if (IS_CH_5G_BAND_4(channel))
		rf_reg_be = high_band[(channel - 149) >> 1];
	else
		goto err;

	rtw_write_rf(rtwdev, RF_PATH_A, RF_MALSEL, RFBE_MASK, rf_reg_be);

	/* need to set 0xdf[18]=1 before writing RF18 when channel 144 */
	if (channel == 144)
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(18), 0x1);
	else
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(18), 0x0);

	rtw_write_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK, rf_reg18);
	if (hal->rf_type > RF_1T1R)
		rtw_write_rf(rtwdev, RF_PATH_B, 0x18, RFREG_MASK, rf_reg18);

	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 0);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 1);

	return;

err:
	WARN_ON(1);
}

static void rtw8822b_toggle_igi(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 igi;

	igi = rtw_read32_mask(rtwdev, REG_RXIGI_A, 0x7f);
	rtw_write32_mask(rtwdev, REG_RXIGI_A, 0x7f, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI_A, 0x7f, igi);
	rtw_write32_mask(rtwdev, REG_RXIGI_B, 0x7f, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI_B, 0x7f, igi);

	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0, 0x0);
	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0,
			 hal->antenna_rx | (hal->antenna_rx << 4));
}

static void rtw8822b_set_channel_rxdfir(struct rtw_dev *rtwdev, u8 bw)
{
	if (bw == RTW_CHANNEL_WIDTH_40) {
		/* RX DFIR for BW40 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x0);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
	} else if (bw == RTW_CHANNEL_WIDTH_80) {
		/* RX DFIR for BW80 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x1);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
	} else {
		/* RX DFIR for BW20, BW10 and BW5*/
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x1);
	}
}

static void rtw8822b_set_channel_bb(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				    u8 primary_ch_idx)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	u8 rfe_option = efuse->rfe_option;
	u32 val32;

	if (IS_CH_2G_BAND(channel)) {
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x0);
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);

		rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x0);
		rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x96a);
		if (channel == 14) {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x00006577);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x0000);
		} else {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x384f6577);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x1525);
		}

		rtw_write32_mask(rtwdev, REG_RFEINV, 0x300, 0x2);
	} else if (IS_CH_5G_BAND(channel)) {
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x1);
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 34);

		if (IS_CH_5G_BAND_1(channel) || IS_CH_5G_BAND_2(channel))
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x1);
		else if (IS_CH_5G_BAND_3(channel))
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x2);
		else if (IS_CH_5G_BAND_4(channel))
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x3);

		if (IS_CH_5G_BAND_1(channel))
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x494);
		else if (IS_CH_5G_BAND_2(channel))
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x453);
		else if (channel >= 100 && channel <= 116)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x452);
		else if (channel >= 118 && channel <= 177)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x412);

		rtw_write32_mask(rtwdev, 0xcbc, 0x300, 0x1);
	}

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
	default:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFFCFFC00;
		val32 |= (RTW_CHANNEL_WIDTH_20);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_40:
		if (primary_ch_idx == RTW_SC_20_UPPER)
			rtw_write32_set(rtwdev, REG_RXSB, BIT(4));
		else
			rtw_write32_clr(rtwdev, REG_RXSB, BIT(4));

		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFF3FF300;
		val32 |= (((primary_ch_idx & 0xf) << 2) | RTW_CHANNEL_WIDTH_40);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_80:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFCEFCF00;
		val32 |= (((primary_ch_idx & 0xf) << 2) | RTW_CHANNEL_WIDTH_80);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);

		if (rfe_option == 2 || rfe_option == 3) {
			rtw_write32_mask(rtwdev, REG_L1PKWT, 0x0000f000, 0x6);
			rtw_write32_mask(rtwdev, REG_ADC40, BIT(10), 0x1);
		}
		break;
	case RTW_CHANNEL_WIDTH_5:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xEFEEFE00;
		val32 |= ((BIT(6) | RTW_CHANNEL_WIDTH_20));
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_10:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xEFFEFF00;
		val32 |= ((BIT(7) | RTW_CHANNEL_WIDTH_20));
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	}
}

void rtw8822b_set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				 u8 primary_chan_idx)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct rtw8822b_rfe_info *rfe_info;

	if (WARN(efuse->rfe_option >= ARRAY_SIZE(rtw8822b_rfe_info),
		 "rfe_option %d is out of boundary\n", efuse->rfe_option))
		return;

	rfe_info = &rtw8822b_rfe_info[efuse->rfe_option];

	rtw8822b_set_channel_bb(rtwdev, channel, bw, primary_chan_idx);
	rtw_set_channel_mac(rtwdev, channel, bw, primary_chan_idx);
	rtw8822b_set_channel_rf(rtwdev, channel, bw);
	rtw8822b_set_channel_rxdfir(rtwdev, bw);
	rtw8822b_toggle_igi(rtwdev);
	rtw8822b_set_channel_cca(rtwdev, channel, bw, rfe_info);
	(*rfe_info->rtw_set_channel_rfe)(rtwdev, channel);
}


static void rtw8822b_config_trx_mode(struct rtw_dev *rtwdev, u8 tx_path,
				     u8 rx_path, bool is_tx2_path)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct rtw8822b_rfe_info *rfe_info;
	u8 ch = rtwdev->hal.current_channel;
	u8 tx_path_sel, rx_path_sel;
	int counter;

	if (WARN(efuse->rfe_option >= ARRAY_SIZE(rtw8822b_rfe_info),
		 "rfe_option %d is out of boundary\n", efuse->rfe_option))
		return;

	rfe_info = &rtw8822b_rfe_info[efuse->rfe_option];

	if ((tx_path | rx_path) & BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_AGCTR_A, MASKLWORD, 0x3231);
	else
		rtw_write32_mask(rtwdev, REG_AGCTR_A, MASKLWORD, 0x1111);

	if ((tx_path | rx_path) & BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_AGCTR_B, MASKLWORD, 0x3231);
	else
		rtw_write32_mask(rtwdev, REG_AGCTR_B, MASKLWORD, 0x1111);

	rtw_write32_mask(rtwdev, REG_CDDTXP, (BIT(19) | BIT(18)), 0x3);
	rtw_write32_mask(rtwdev, REG_TXPSEL, (BIT(29) | BIT(28)), 0x1);
	rtw_write32_mask(rtwdev, REG_TXPSEL, BIT(30), 0x1);

	if (tx_path & BB_PATH_A) {
		rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x001);
		rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0x8);
	} else if (tx_path & BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x002);
		rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0x4);
	}

	if (tx_path == BB_PATH_A || tx_path == BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_TXPSEL1, 0xfff0, 0x01);
	else
		rtw_write32_mask(rtwdev, REG_TXPSEL1, 0xfff0, 0x43);

	tx_path_sel = (tx_path << 4) | tx_path;
	rtw_write32_mask(rtwdev, REG_TXPSEL, MASKBYTE0, tx_path_sel);

	if (tx_path != BB_PATH_A && tx_path != BB_PATH_B) {
		if (is_tx2_path || rtwdev->mp_mode) {
			rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x043);
			rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0xc);
		}
	}

	rtw_write32_mask(rtwdev, REG_RXDESC, BIT(22), 0x0);
	rtw_write32_mask(rtwdev, REG_RXDESC, BIT(18), 0x0);

	if (rx_path & BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_ADCINI, 0x0f000000, 0x0);
	else if (rx_path & BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_ADCINI, 0x0f000000, 0x5);

	rx_path_sel = (rx_path << 4) | rx_path;
	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0, rx_path_sel);

	if (rx_path == BB_PATH_A || rx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_ANTWT, BIT(16), 0x0);
		rtw_write32_mask(rtwdev, REG_HTSTFWT, BIT(28), 0x0);
		rtw_write32_mask(rtwdev, REG_MRC, BIT(23), 0x0);
	} else {
		rtw_write32_mask(rtwdev, REG_ANTWT, BIT(16), 0x1);
		rtw_write32_mask(rtwdev, REG_HTSTFWT, BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_MRC, BIT(23), 0x1);
	}

	for (counter = 100; counter > 0; counter--) {
		u32 rf_reg33;

		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);

		udelay(2);
		rf_reg33 = rtw_read_rf(rtwdev, RF_PATH_A, 0x33, RFREG_MASK);

		if (rf_reg33 == 0x00001)
			break;
	}

	if (WARN(counter <= 0, "write RF mode table fail\n"))
		return;

	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWD1, RFREG_MASK, 0x00034);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWD0, RFREG_MASK, 0x4080c);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);

	rtw8822b_toggle_igi(rtwdev);
	rtw8822b_set_channel_cca(rtwdev, 1, RTW_CHANNEL_WIDTH_20, rfe_info);
	(*rfe_info->rtw_set_channel_rfe)(rtwdev, ch);
}

void rtw_bf_phy_init(struct rtw_dev *rtwdev)
{
	u8 tmp8;
	u32 tmp32;
	u8 retry_limit = 0xA;
	u8 ndpa_rate = 0x10;
	u8 ack_policy = 3;

	tmp32 = rtw_read32(rtwdev, REG_MU_TX_CTL);
	/* Enable P1 aggr new packet according to P0 transfer time */
	tmp32 |= BIT_MU_P1_WAIT_STATE_EN;
	/* MU Retry Limit */
	tmp32 &= ~BIT_MASK_R_MU_RL;
	tmp32 |= (retry_limit << BIT_SHIFT_R_MU_RL) & BIT_MASK_R_MU_RL;
	/* Disable Tx MU-MIMO until sounding done */
	tmp32 &= ~BIT_EN_MU_MIMO;
	/* Clear validity of MU STAs */
	tmp32 &= ~BIT_MASK_R_MU_TABLE_VALID;
	rtw_write32(rtwdev, REG_MU_TX_CTL, tmp32);

	/* MU-MIMO Option as default value */
	tmp8 = ack_policy << BIT_SHIFT_WMAC_TXMU_ACKPOLICY;
	tmp8 |= BIT_WMAC_TXMU_ACKPOLICY_EN;
	rtw_write8(rtwdev, REG_WMAC_MU_BF_OPTION, tmp8);

	/* MU-MIMO Control as default value */
	rtw_write16(rtwdev, REG_WMAC_MU_BF_CTL, 0);
	/* Set MU NDPA rate & BW source */
	rtw_write32_set(rtwdev, REG_TXBF_CTRL, BIT_USE_NDPA_PARAMETER);
	/* Set NDPA Rate */
	rtw_write8(rtwdev, REG_NDPA_OPT_CTRL, ndpa_rate);

	rtw_write32_mask(rtwdev, REG_BBPSF_CTRL, BIT_MASK_CSI_RATE,
			 DESC_RATE6M);
}

static void rtw8822b_phy_bf_init(struct rtw_dev *rtwdev)
{
	rtw_bf_phy_init(rtwdev);
	/* Grouping bitmap parameters */
	rtw_write32(rtwdev, 0x1C94, 0xAFFFAFFF);
}

void rtw8822b_phy_set_param(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 crystal_cap;
	bool is_tx2_path;

	/* power on BB/RF domain */
	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN,
		       BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtw_write8_set(rtwdev, REG_RF_CTRL,
		       BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	rtw_write32_set(rtwdev, REG_WLRF1, BIT_WLRF1_BBRF_EN);

	/* pre init before header files config */
	rtw_write32_clr(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

	rtw_phy_load_tables(rtwdev);

	crystal_cap = rtwdev->efuse.crystal_cap & 0x3F;
	rtw_write32_mask(rtwdev, 0x24, 0x7e000000, crystal_cap);
	rtw_write32_mask(rtwdev, 0x28, 0x7e, crystal_cap);

	/* post init after header files config */
	rtw_write32_set(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

	is_tx2_path = false;
	rtw8822b_config_trx_mode(rtwdev, hal->antenna_tx, hal->antenna_rx,
				 is_tx2_path);
	/*
	 * rtwb: rtw_phy_init() and rtw8822b_pwrtrack_init() only set up
	 * dynamic-mechanism state (DIG, CCK PD, CFO, power tracking), which
	 * is not ported yet.
	 */

	rtw8822b_phy_rfe_init(rtwdev);

	rtw8822b_phy_bf_init(rtwdev);
}

/*
 * rtw_update_channel() (main.c).  rtwb: channel numbers instead of
 * frequencies for the primary/center comparison (same ordering within a
 * band); no SAR band.
 */
void rtw_update_channel(struct rtw_dev *rtwdev, u8 center_channel,
			u8 primary_channel, u8 band, u8 bandwidth)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 *cch_by_bw = hal->cch_by_bw;
	u8 primary_channel_idx;

	/* assign the center channel used while 20M bw is selected */
	cch_by_bw[RTW_CHANNEL_WIDTH_20] = primary_channel;

	/* assign the center channel used while current bw is selected */
	cch_by_bw[bandwidth] = center_channel;

	switch (bandwidth) {
	case RTW_CHANNEL_WIDTH_20:
	default:
		primary_channel_idx = RTW_SC_DONT_CARE;
		break;
	case RTW_CHANNEL_WIDTH_40:
		if (primary_channel > center_channel)
			primary_channel_idx = RTW_SC_20_UPPER;
		else
			primary_channel_idx = RTW_SC_20_LOWER;
		break;
	case RTW_CHANNEL_WIDTH_80:
		if (primary_channel > center_channel) {
			if (primary_channel - center_channel == 2)
				primary_channel_idx = RTW_SC_20_UPPER;
			else
				primary_channel_idx = RTW_SC_20_UPMOST;
			cch_by_bw[RTW_CHANNEL_WIDTH_40] = center_channel + 4;
		} else {
			if (center_channel - primary_channel == 2)
				primary_channel_idx = RTW_SC_20_LOWER;
			else
				primary_channel_idx = RTW_SC_20_LOWEST;
			cch_by_bw[RTW_CHANNEL_WIDTH_40] = center_channel - 4;
		}
		break;
	}

	hal->current_primary_channel_index = primary_channel_idx;
	hal->current_band_width = bandwidth;
	hal->primary_channel = primary_channel;
	hal->current_channel = center_channel;
	hal->current_band_type = band;
}

/*
 * query_phy_status() with its page 0 (CCK) and page 1 (OFDM/HT/VHT)
 * helpers.  rtwb: only the RX power and bandwidth are kept; EVM, SNR and
 * CFO feed rtw88's dynamic mechanisms, which are not ported yet.
 */
#define GET_PHY_STAT_P0_PWDB(p)		((le32dec((p) + 0) >> 8) & 0xff)
#define GET_PHY_STAT_P1_PWDB_A(p)	((le32dec((p) + 0) >> 8) & 0xff)
#define GET_PHY_STAT_P1_PWDB_B(p)	((le32dec((p) + 0) >> 16) & 0xff)
#define GET_PHY_STAT_P1_RF_MODE(p)	((le32dec((p) + 12) >> 28) & 0x3)
#define GET_PHY_STAT_P1_L_RXSC(p)	((le32dec((p) + 4) >> 8) & 0xf)
#define GET_PHY_STAT_P1_HT_RXSC(p)	((le32dec((p) + 4) >> 12) & 0xf)

static void query_phy_status_page0(struct rtw_dev *rtwdev, const u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	s8 min_rx_power = -120;
	u8 pwdb = GET_PHY_STAT_P0_PWDB(phy_status);

	/* 8822B uses only 1 antenna to RX CCK rates */
	pkt_stat->rx_power[RF_PATH_A] = pwdb - 110;
	pkt_stat->bw = RTW_CHANNEL_WIDTH_20;
	pkt_stat->signal_power = MAX(pkt_stat->rx_power[RF_PATH_A],
				     min_rx_power);
}

static void query_phy_status_page1(struct rtw_dev *rtwdev, const u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 rxsc, bw;
	s8 min_rx_power = -120;

	if (pkt_stat->rate > DESC_RATE11M && pkt_stat->rate < DESC_RATEMCS0)
		rxsc = GET_PHY_STAT_P1_L_RXSC(phy_status);
	else
		rxsc = GET_PHY_STAT_P1_HT_RXSC(phy_status);

	if (rxsc >= 1 && rxsc <= 8)
		bw = RTW_CHANNEL_WIDTH_20;
	else if (rxsc >= 9 && rxsc <= 12)
		bw = RTW_CHANNEL_WIDTH_40;
	else if (rxsc >= 13)
		bw = RTW_CHANNEL_WIDTH_80;
	else
		bw = GET_PHY_STAT_P1_RF_MODE(phy_status);

	pkt_stat->rx_power[RF_PATH_A] = GET_PHY_STAT_P1_PWDB_A(phy_status) - 110;
	pkt_stat->rx_power[RF_PATH_B] = GET_PHY_STAT_P1_PWDB_B(phy_status) - 110;
	pkt_stat->bw = bw;
	pkt_stat->signal_power = MAX(MAX(pkt_stat->rx_power[RF_PATH_A],
				     pkt_stat->rx_power[RF_PATH_B]),
				     min_rx_power);
}

void rtw8822b_query_phy_status(struct rtw_dev *rtwdev, const u8 *phy_status,
			       struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 page;

	page = *phy_status & 0xf;

	switch (page) {
	case 0:
		query_phy_status_page0(rtwdev, phy_status, pkt_stat);
		break;
	case 1:
		query_phy_status_page1(rtwdev, phy_status, pkt_stat);
		break;
	default:
		rtw_warn(rtwdev, "unused phy status page (%d)\n", page);
		return;
	}
}
