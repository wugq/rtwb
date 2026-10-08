/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 coex.c and rtw8822b.c: the WLAN/BT coexistence
 * setup done at power-on, for the "Wi-Fi only" case
 * (rtw_coex_power_on_setting() + rtw_coex_init_hw_config(wifi_only)):
 * the WLAN grant is forced high, the BT grant low, and the antenna switch
 * points to the WLAN side.  The functions below are copied verbatim except
 * where marked "rtwb:".
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * rtwb: the run-time coexistence mechanism (BT info, TDMA, PTA tables per
 * BT profile) is not ported, so Bluetooth cannot use the shared antenna
 * while the WLAN interface is up.
 */

#include "rtw88_phy.h"

enum coex_set_ant_phase {
	COEX_SET_ANT_INIT,
	COEX_SET_ANT_WONLY,
	COEX_SET_ANT_WOFF,
	COEX_SET_ANT_2G,
	COEX_SET_ANT_5G,
	COEX_SET_ANT_POWERON,
	COEX_SET_ANT_2G_WLBT,
	COEX_SET_ANT_2G_FREERUN,

	COEX_SET_ANT_MAX
};
enum coex_gnt_setup_state {
	COEX_GNT_SET_HW_PTA	= 0x0,
	COEX_GNT_SET_SW_LOW	= 0x1,
	COEX_GNT_SET_SW_HIGH	= 0x3,
};
enum coex_ext_ant_switch_pos_type {
	COEX_SWITCH_TO_BT,
	COEX_SWITCH_TO_WLG,
	COEX_SWITCH_TO_WLA,
	COEX_SWITCH_TO_NOCARE,
	COEX_SWITCH_TO_WLG_BT,

	COEX_SWITCH_TO_MAX
};
enum coex_ext_ant_switch_ctrl_type {
	COEX_SWITCH_CTRL_BY_BBSW,
	COEX_SWITCH_CTRL_BY_PTA,
	COEX_SWITCH_CTRL_BY_ANTDIV,
	COEX_SWITCH_CTRL_BY_MAC,
	COEX_SWITCH_CTRL_BY_BT,
	COEX_SWITCH_CTRL_BY_FW,

	COEX_SWITCH_CTRL_MAX
};
enum coex_wl_priority_mask {
	COEX_WLPRI_RX_RSP	= 2,
	COEX_WLPRI_TX_RSP	= 3,
	COEX_WLPRI_TX_BEACON	= 4,
	COEX_WLPRI_TX_OFDM	= 11,
	COEX_WLPRI_TX_CCK	= 12,
	COEX_WLPRI_TX_BEACONQ	= 27,
	COEX_WLPRI_RX_CCK	= 28,
	COEX_WLPRI_RX_OFDM	= 29,
	COEX_WLPRI_MAX
};

enum coex_wl2bt_scoreboard {
	COEX_SCBD_ACTIVE	= BIT(0),
	COEX_SCBD_ONOFF		= BIT(1),
	COEX_SCBD_SCAN		= BIT(2),
	COEX_SCBD_UNDERTEST	= BIT(3),
	COEX_SCBD_RXGAIN	= BIT(4),
	COEX_SCBD_BT_RFK	= BIT(5),
	COEX_SCBD_WLBUSY	= BIT(6),
	COEX_SCBD_EXTFEM	= BIT(8),
	COEX_SCBD_TDMA		= BIT(9),
	COEX_SCBD_FIX2M		= BIT(10),
	COEX_SCBD_ALL		= GENMASK(15, 0),
};

struct rtw_coex_rfe {
	bool ant_switch_exist;
	bool ant_switch_diversity;
	bool ant_switch_with_bt;
	u8 rfe_module_type;
	u8 ant_switch_polarity;

	/* true if WLG at BTG, else at WLAG */
	bool wlg_at_btg;
};

/* rtwb: the coex state rtw88 keeps in struct rtw_coex, as far as used */
static struct rtw_coex_rfe coex_rfe_state;
static u16 coex_score_board;

/* rtw_chip_efuse_info_setup(): share_ant = bt_setting & BIT(0) */
static bool rtw_efuse_share_ant(struct rtw_dev *rtwdev)
{
	return (rtwdev->efuse.bt_setting & BIT(0)) != 0;
}

static u32 rtw_coex_read_indirect_reg(struct rtw_dev *rtwdev, u16 addr)
{
	u32 val;

	if (!ltecoex_read_reg(rtwdev, addr, &val)) {
		rtw_err(rtwdev, "failed to read indirect register\n");
		return 0;
	}

	return val;
}

static void rtw_coex_write_indirect_reg(struct rtw_dev *rtwdev, u16 addr,
				 u32 mask, u32 val)
{
	u32 shift = __ffs(mask);
	u32 tmp;

	tmp = rtw_coex_read_indirect_reg(rtwdev, addr);
	tmp = (tmp & (~mask)) | ((val << shift) & mask);

	if (!ltecoex_reg_write(rtwdev, addr, tmp))
		rtw_err(rtwdev, "failed to write indirect register\n");
}

static void rtw_coex_write_scbd(struct rtw_dev *rtwdev, u16 bitpos, bool set)
{
	u16 val = 0x2;

	/* rtwb: the 8822B has scbd_support and no new_scbd10_def */
	val |= coex_score_board;

	/* for 8822b, scbd[10] is CQDDR on
	 * for 8822c, scbd[10] is no fix 2M
	 */
	if (bitpos & COEX_SCBD_FIX2M) {
		if (set)
			val &= ~COEX_SCBD_FIX2M;
		else
			val |= COEX_SCBD_FIX2M;
	} else {
		if (set)
			val |= bitpos;
		else
			val &= ~bitpos;
	}

	if (val != coex_score_board) {
		coex_score_board = val;
		val |= BIT_BT_INT_EN;
		rtw_write16(rtwdev, REG_WIFI_BT_INFO, val);
	}
}

static void rtw_coex_set_gnt_bt(struct rtw_dev *rtwdev, u8 state)
{
	/* rtwb: the 8822B has ltecoex_addr */
	rtw_coex_write_indirect_reg(rtwdev, LTE_COEX_CTRL, 0xc000, state);
	rtw_coex_write_indirect_reg(rtwdev, LTE_COEX_CTRL, 0x0c00, state);
}

static void rtw_coex_set_gnt_wl(struct rtw_dev *rtwdev, u8 state)
{
	/* rtwb: the 8822B has ltecoex_addr */
	rtw_coex_write_indirect_reg(rtwdev, LTE_COEX_CTRL, 0x3000, state);
	rtw_coex_write_indirect_reg(rtwdev, LTE_COEX_CTRL, 0x0300, state);
}

static void rtw_coex_coex_ctrl_owner(struct rtw_dev *rtwdev, bool wifi_control)
{
	/* rtwb: the 8822B has no btg_reg */
	if (wifi_control) {
		rtw_write8_set(rtwdev, REG_SYS_SDIO_CTRL + 3,
			       BIT_LTE_MUX_CTRL_PATH >> 24);
	} else {
		rtw_write8_clr(rtwdev, REG_SYS_SDIO_CTRL + 3,
			       BIT_LTE_MUX_CTRL_PATH >> 24);
	}
}

static void rtw_coex_set_wl_pri_mask(struct rtw_dev *rtwdev, u8 bitmap,
				     u8 data)
{
	u32 addr;

	addr = REG_BT_COEX_TABLE_H + (bitmap / 8);
	bitmap = bitmap % 8;

	rtw_write8_mask(rtwdev, addr, BIT(bitmap), data);
}

static void rtw_coex_set_table(struct rtw_dev *rtwdev, bool force, u32 table0,
			       u32 table1)
{
#define DEF_BRK_TABLE_VAL 0xf0ffffff
	/* If last tdma is wl slot toggle, force write table*/
	if (!force) {
		if (table0 == rtw_read32(rtwdev, REG_BT_COEX_TABLE0) &&
		    table1 == rtw_read32(rtwdev, REG_BT_COEX_TABLE1))
			return;
	}
	rtw_write32(rtwdev, REG_BT_COEX_TABLE0, table0);
	rtw_write32(rtwdev, REG_BT_COEX_TABLE1, table1);
	rtw_write32(rtwdev, REG_BT_COEX_BRK_TABLE, DEF_BRK_TABLE_VAL);

	rtw_dbg(rtwdev, RTW_DBG_COEX,
		"[BTCoex], %s(): 0x6c0 = %x, 0x6c4 = %x\n", __func__, table0,
		table1);
}

static void rtw8822b_coex_cfg_init(struct rtw_dev *rtwdev)
{
	/* enable TBTT nterrupt */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);

	/* BT report packet sample rate */
	/* 0x790[5:0]=0x5 */
	rtw_write8_mask(rtwdev, REG_BT_TDMA_TIME, BIT_MASK_SAMPLE_RATE, 0x5);

	/* enable BT counter statistics */
	rtw_write8(rtwdev, REG_BT_STAT_CTRL, 0x1);

	/* enable PTA (3-wire function form BT side) */
	rtw_write32_set(rtwdev, REG_GPIO_MUXCFG, BIT_BT_PTA_EN);
	rtw_write32_set(rtwdev, REG_GPIO_MUXCFG, BIT_PO_BT_PTA_PINS);

	/* enable PTA (tx/rx signal form WiFi side) */
	rtw_write8_set(rtwdev, REG_QUEUE_CTRL, BIT_PTA_WL_TX_EN);
	/* wl tx signal to PTA not case EDCCA */
	rtw_write8_clr(rtwdev, REG_QUEUE_CTRL, BIT_PTA_EDCCA_EN);
	/* GNT_BT=1 while select both */
	rtw_write16_set(rtwdev, REG_BT_COEX_V2, BIT_GNT_BT_POLARITY);
}

static void rtw8822b_coex_cfg_ant_switch(struct rtw_dev *rtwdev,
					 u8 ctrl_type, u8 pos_type)
{
	struct rtw_coex_rfe *coex_rfe = &coex_rfe_state;
	bool polarity_inverse;
	u8 regval = 0;

	if (coex_rfe->ant_switch_diversity &&
	    ctrl_type == COEX_SWITCH_CTRL_BY_BBSW)
		ctrl_type = COEX_SWITCH_CTRL_BY_ANTDIV;

	polarity_inverse = (coex_rfe->ant_switch_polarity == 1);

	switch (ctrl_type) {
	default:
	case COEX_SWITCH_CTRL_BY_BBSW:
		/* 0x4c[23] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x0);
		/* 0x4c[24] = 1 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 3, BIT_DPDT_WL_SEL >> 24, 0x1);
		/* BB SW, DPDT use RFE_ctrl8 and RFE_ctrl9 as ctrl pin */
		rtw_write8_mask(rtwdev, REG_RFE_CTRL8, BIT_MASK_RFE_SEL89, 0x77);

		if (pos_type == COEX_SWITCH_TO_WLG_BT) {
			if (coex_rfe->rfe_module_type != 0x4 &&
			    coex_rfe->rfe_module_type != 0x2)
				regval = 0x3;
			else
				regval = (!polarity_inverse ? 0x2 : 0x1);
		} else if (pos_type == COEX_SWITCH_TO_WLG) {
			regval = (!polarity_inverse ? 0x2 : 0x1);
		} else {
			regval = (!polarity_inverse ? 0x1 : 0x2);
		}

		rtw_write8_mask(rtwdev, REG_RFE_INV8, BIT_MASK_RFE_INV89, regval);
		break;
	case COEX_SWITCH_CTRL_BY_PTA:
		/* 0x4c[23] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x0);
		/* 0x4c[24] = 1 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 3, BIT_DPDT_WL_SEL >> 24, 0x1);
		/* PTA,  DPDT use RFE_ctrl8 and RFE_ctrl9 as ctrl pin */
		rtw_write8_mask(rtwdev, REG_RFE_CTRL8, BIT_MASK_RFE_SEL89, 0x66);

		regval = (!polarity_inverse ? 0x2 : 0x1);
		rtw_write8_mask(rtwdev, REG_RFE_INV8, BIT_MASK_RFE_INV89, regval);
		break;
	case COEX_SWITCH_CTRL_BY_ANTDIV:
		/* 0x4c[23] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x0);
		/* 0x4c[24] = 1 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 3, BIT_DPDT_WL_SEL >> 24, 0x1);
		rtw_write8_mask(rtwdev, REG_RFE_CTRL8, BIT_MASK_RFE_SEL89, 0x88);
		break;
	case COEX_SWITCH_CTRL_BY_MAC:
		/* 0x4c[23] = 1 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x1);

		regval = (!polarity_inverse ? 0x0 : 0x1);
		rtw_write8_mask(rtwdev, REG_PAD_CTRL1, BIT_SW_DPDT_SEL_DATA, regval);
		break;
	case COEX_SWITCH_CTRL_BY_FW:
		/* 0x4c[23] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x0);
		/* 0x4c[24] = 1 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 3, BIT_DPDT_WL_SEL >> 24, 0x1);
		break;
	case COEX_SWITCH_CTRL_BY_BT:
		/* 0x4c[23] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 2, BIT_DPDT_SEL_EN >> 16, 0x0);
		/* 0x4c[24] = 0 */
		rtw_write8_mask(rtwdev, REG_LED_CFG + 3, BIT_DPDT_WL_SEL >> 24, 0x0);
		break;
	}
}

static void rtw8822b_coex_cfg_gnt_debug(struct rtw_dev *rtwdev)
{
	rtw_write8_mask(rtwdev, REG_PAD_CTRL1 + 2, BIT_BTGP_SPI_EN >> 16, 0);
	rtw_write8_mask(rtwdev, REG_PAD_CTRL1 + 3, BIT_BTGP_JTAG_EN >> 24, 0);
	rtw_write8_mask(rtwdev, REG_GPIO_MUXCFG + 2, BIT_FSPI_EN >> 16, 0);
	rtw_write8_mask(rtwdev, REG_PAD_CTRL1 + 1, BIT_LED1DIS >> 8, 0);
	rtw_write8_mask(rtwdev, REG_SYS_SDIO_CTRL + 3, BIT_DBG_GNT_WL_BT >> 24, 0);
}

static void rtw8822b_coex_cfg_rfe_type(struct rtw_dev *rtwdev)
{
	struct rtw_coex_rfe *coex_rfe = &coex_rfe_state;
	bool is_ext_fem = false;

	coex_rfe->rfe_module_type = rtwdev->efuse.rfe_option;
	coex_rfe->ant_switch_polarity = 0;
	coex_rfe->ant_switch_diversity = false;
	if (coex_rfe->rfe_module_type == 0x12 ||
	    coex_rfe->rfe_module_type == 0x15 ||
	    coex_rfe->rfe_module_type == 0x16)
		coex_rfe->ant_switch_exist = false;
	else
		coex_rfe->ant_switch_exist = true;

	if (coex_rfe->rfe_module_type == 2 ||
	    coex_rfe->rfe_module_type == 4) {
		rtw_coex_write_scbd(rtwdev, COEX_SCBD_EXTFEM, true);
		is_ext_fem = true;
	} else {
		rtw_coex_write_scbd(rtwdev, COEX_SCBD_EXTFEM, false);
	}

	coex_rfe->wlg_at_btg = false;

	if (rtw_efuse_share_ant(rtwdev) &&
	    coex_rfe->ant_switch_exist && !is_ext_fem)
		coex_rfe->ant_switch_with_bt = true;
	else
		coex_rfe->ant_switch_with_bt = false;

	/* Ext switch buffer mux */
	rtw_write8(rtwdev, REG_RFE_CTRL_E, 0xff);
	rtw_write8_mask(rtwdev, REG_RFESEL_CTRL + 1, 0x3, 0x0);
	rtw_write8_mask(rtwdev, REG_RFE_INV16, BIT_RFE_BUF_EN, 0x0);

	/* Disable LTE Coex Function in WiFi side */
	rtw_coex_write_indirect_reg(rtwdev, LTE_COEX_CTRL, BIT_LTE_COEX_EN, 0);

	/* BTC_CTT_WL_VS_LTE */
	rtw_coex_write_indirect_reg(rtwdev, LTE_WL_TRX_CTRL, MASKLWORD, 0xffff);

	/* BTC_CTT_BT_VS_LTE */
	rtw_coex_write_indirect_reg(rtwdev, LTE_BT_TRX_CTRL, MASKLWORD, 0xffff);
}


/* table_sant_8822b[1] (Shared-Antenna Coex Table, case-1) */
#define COEX_TABLE_SANT_1_BT	0x55555555
#define COEX_TABLE_SANT_1_WL	0x55555555

/*
 * rtw_coex_power_on_setting() followed by
 * __rtw_coex_init_hw_config(rtwdev, wifi_only = true).
 * rtwb: coex variables, BT monitoring, WL slot extension and the TDMA H2C
 * (TDMA off is the firmware default) are left out; the power-on antenna
 * phase (path to BT) is skipped because the Wi-Fi-only phase below
 * overrides it immediately.
 */
void rtw_coex_init_wifi_only(struct rtw_dev *rtwdev)
{
	struct rtw_coex_rfe *coex_rfe = &coex_rfe_state;

	/* --- rtw_coex_power_on_setting() --- */

	/* enable BB, we can write 0x948 */
	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN,
		       BIT_FEN_BB_GLB_RST | BIT_FEN_BB_RSTB);

	rtw8822b_coex_cfg_rfe_type(rtwdev);

	/* rtw_coex_table(rtwdev, true, 1) */
	if (rtw_efuse_share_ant(rtwdev))
		rtw_coex_set_table(rtwdev, true, COEX_TABLE_SANT_1_BT,
				   COEX_TABLE_SANT_1_WL);
	/* red x issue */
	rtw_write8(rtwdev, 0xff1a, 0x0);
	rtw8822b_coex_cfg_gnt_debug(rtwdev);

	/* --- __rtw_coex_init_hw_config(rtwdev, true) --- */

	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);

	rtw8822b_coex_cfg_rfe_type(rtwdev);
	rtw8822b_coex_cfg_init(rtwdev);

	/* set Tx response = Hi-Pri (ex: Transmitting ACK,BA,CTS) */
	rtw_coex_set_wl_pri_mask(rtwdev, COEX_WLPRI_TX_RSP, 1);

	/* set Tx beacon = Hi-Pri */
	rtw_coex_set_wl_pri_mask(rtwdev, COEX_WLPRI_TX_BEACON, 1);

	/* set Tx beacon queue = Hi-Pri */
	rtw_coex_set_wl_pri_mask(rtwdev, COEX_WLPRI_TX_BEACONQ, 1);

	/* rtw_coex_set_ant_path(rtwdev, true, COEX_SET_ANT_WONLY) */
	/* set GNT_BT to SW Low */
	rtw_coex_set_gnt_bt(rtwdev, COEX_GNT_SET_SW_LOW);

	/* set GNT_WL to SW high */
	rtw_coex_set_gnt_wl(rtwdev, COEX_GNT_SET_SW_HIGH);

	/* set path control owner to wl at initial step */
	rtw_coex_coex_ctrl_owner(rtwdev, true);

	if (coex_rfe->ant_switch_exist)
		rtw8822b_coex_cfg_ant_switch(rtwdev, COEX_SWITCH_CTRL_BY_BBSW,
					     COEX_SWITCH_TO_WLG);

	rtw_coex_write_scbd(rtwdev, COEX_SCBD_ACTIVE | COEX_SCBD_ONOFF, true);

	/* PTA parameter: rtw_coex_table(rtwdev, true, 1) */
	if (rtw_efuse_share_ant(rtwdev))
		rtw_coex_set_table(rtwdev, true, COEX_TABLE_SANT_1_BT,
				   COEX_TABLE_SANT_1_WL);
}

/* table_sant_8822b[2], used by rtw_coex_action_coex_all_off() */
#define COEX_TABLE_SANT_2_BT	0x66555555
#define COEX_TABLE_SANT_2_WL	0x66555555

/*
 * Wi-Fi is going down: hand the antenna back to Bluetooth.  This is
 * rtw_coex_ips_notify(COEX_IPS_ENTER) -- scoreboard off, antenna path
 * COEX_SET_ANT_WOFF (path owner and antenna switch to BT), coex "all off"
 * table -- followed by rtw_coex_power_off_setting().  The TDMA and RF
 * parameters of coex_all_off are left out: the firmware is about to stop.
 * Call it while the MAC is still powered.
 */
void rtw_coex_wifi_off(struct rtw_dev *rtwdev)
{
	struct rtw_coex_rfe *coex_rfe = &coex_rfe_state;

	/* for lps off */
	rtw_coex_write_scbd(rtwdev, COEX_SCBD_ALL, false);

	/* rtw_coex_set_ant_path(rtwdev, true, COEX_SET_ANT_WOFF) */
	/* set path control owner to BT */
	rtw_coex_coex_ctrl_owner(rtwdev, false);
	if (coex_rfe->ant_switch_exist)
		rtw8822b_coex_cfg_ant_switch(rtwdev, COEX_SWITCH_CTRL_BY_BT,
					     COEX_SWITCH_TO_NOCARE);

	/* rtw_coex_action_coex_all_off(): rtw_coex_table(rtwdev, false, 2) */
	if (rtw_efuse_share_ant(rtwdev))
		rtw_coex_set_table(rtwdev, false, COEX_TABLE_SANT_2_BT,
				   COEX_TABLE_SANT_2_WL);

	/* rtw_coex_power_off_setting(): scoreboard cleared, BT interrupt on */
	rtw_write16(rtwdev, REG_WIFI_BT_INFO, BIT_BT_INT_EN);
	coex_score_board = 0;
}

/*
 * The runtime antenna setup, after rtw_coex_init_wifi_only().  That init
 * (COEX_SET_ANT_WONLY) forces GNT_BT low; rtw88 replaces it at once with
 * a runtime path chosen from the band and the Bluetooth status.  Without
 * a runtime path Bluetooth could still start its own transfers but never
 * got the antenna for its page scan windows, so devices could not connect
 * to us (a mouse waking up and reconnecting, for example).
 *
 * rtwb has no dynamic coexistence yet; this static setup was chosen by
 * measurement on an RFE type 5 board (incoming Bluetooth connections with
 * Wi-Fi idle and under iperf3 upload, on 2.4 GHz and 5 GHz):
 * - GNT_BT by the PTA, GNT_WL high, path owner Wi-Fi, antenna switch by
 *   the baseband at the 2.4 GHz (WLG) position, PTA table 1.  With Wi-Fi
 *   idle every connection got through on both bands; under a saturating
 *   2.4 GHz upload about half did, and the upload kept its throughput.
 * - rtw88's COEX_SET_ANT_2G (GNT_WL and the switch by the PTA too) got
 *   Bluetooth through more often under load but halved the upload.
 * - rtw88's COEX_SET_ANT_5G moves the switch to WLA, which cut Bluetooth
 *   off the antenna on this board; Wi-Fi on 5 GHz does not use it.
 * - Tables 0 and 10, which rtw88 uses with its TDMA, let Wi-Fi win every
 *   time.
 * It does not depend on the band, but channel switches reprogram the RFE
 * pins, so it is applied again after each one.
 */
void rtw_coex_runtime_setup(struct rtw_dev *rtwdev)
{
	struct rtw_coex_rfe *coex_rfe = &coex_rfe_state;

	rtw_coex_set_gnt_bt(rtwdev, COEX_GNT_SET_HW_PTA);
	rtw_coex_set_gnt_wl(rtwdev, COEX_GNT_SET_SW_HIGH);
	rtw_coex_coex_ctrl_owner(rtwdev, true);
	if (coex_rfe->ant_switch_exist)
		rtw8822b_coex_cfg_ant_switch(rtwdev, COEX_SWITCH_CTRL_BY_BBSW,
					     COEX_SWITCH_TO_WLG);
	if (rtw_efuse_share_ant(rtwdev))
		rtw_coex_set_table(rtwdev, false, COEX_TABLE_SANT_1_BT,
				   COEX_TABLE_SANT_1_WL);
}
