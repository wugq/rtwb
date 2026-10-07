/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 main.c (rtw_update_sta_info, get_rate_id,
 * get_vht_ra_mask, rtw_rate_mask_recover), tx.c (get_highest_ht_tx_rate,
 * get_highest_vht_tx_rate) and fw.c (rtw_fw_send_ra_info): the firmware
 * rate adaptation ("RA info") for one peer.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * rtwb: the peer's capabilities come from the net80211 node instead of
 * mac80211's ieee80211_sta; no RSSI-level masking (level 0) and no user
 * rate mask yet.
 */

#include "rtw88_phy.h"

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_vht.h>

#define H2C_CMD_RA_INFO		0x40

#define RA_MASK_CCK_RATES	0x0000f
#define RA_MASK_OFDM_RATES	0x00ff0
#define RA_MASK_HT_RATES_1SS	(0xff000ULL << 0)
#define RA_MASK_HT_RATES_2SS	(0xff000ULL << 8)
#define RA_MASK_HT_RATES_3SS	(0xff000ULL << 16)
#define RA_MASK_HT_RATES	(RA_MASK_HT_RATES_1SS | \
				 RA_MASK_HT_RATES_2SS | \
				 RA_MASK_HT_RATES_3SS)
#define RA_MASK_VHT_RATES_1SS	(0x3ff000ULL << 0)
#define RA_MASK_VHT_RATES_2SS	(0x3ff000ULL << 10)
#define RA_MASK_VHT_RATES_3SS	(0x3ff000ULL << 20)
#define RA_MASK_VHT_RATES	(RA_MASK_VHT_RATES_1SS | \
				 RA_MASK_VHT_RATES_2SS | \
				 RA_MASK_VHT_RATES_3SS)
#define RA_MASK_CCK_IN_BG	0x00005
#define RA_MASK_CCK_IN_HT	0x00005
#define RA_MASK_CCK_IN_VHT	0x00005
#define RA_MASK_OFDM_IN_VHT	0x00010
#define RA_MASK_OFDM_IN_HT_2G	0x00010
#define RA_MASK_OFDM_IN_HT_5G	0x00030

#define WIRELESS_CCK	0x00000001
#define WIRELESS_OFDM	0x00000002
#define WIRELESS_HT	0x00000004
#define WIRELESS_VHT	0x00000008

#define HT_STBC_EN	BIT(0)
#define VHT_STBC_EN	BIT(1)
#define HT_LDPC_EN	BIT(0)
#define VHT_LDPC_EN	BIT(1)

/* enum rtw_rate_id (main.h) beyond the ones in rtw88_reg.h */
#define RTW_RATEID_BGN_40M_2SS		0
#define RTW_RATEID_BGN_40M_1SS		1
#define RTW_RATEID_BGN_20M_2SS		2
#define RTW_RATEID_BGN_20M_1SS		3
#define RTW_RATEID_GN_N2SS		4
#define RTW_RATEID_GN_N1SS		5
#define RTW_RATEID_BG			6
#define RTW_RATEID_B_20M		8
#define RTW_RATEID_ARFR0_AC_2SS		9
#define RTW_RATEID_ARFR1_AC_1SS		10
#define RTW_RATEID_ARFR2_AC_2G_1SS	11
#define RTW_RATEID_ARFR3_AC_2G_2SS	12
#define RTW_RATEID_ARFR4_AC_3SS		13
#define RTW_RATEID_ARFR5_N_3SS		14
#define RTW_RATEID_ARFR7_N_4SS		15
#define RTW_RATEID_ARFR6_AC_4SS		16

#define SET_RA_INFO_MACID(p, v)		le32p_replace_bits((__le32 *)(p), v, GENMASK(15, 8))
#define SET_RA_INFO_RATE_ID(p, v)	le32p_replace_bits((__le32 *)(p), v, GENMASK(20, 16))
#define SET_RA_INFO_INIT_RA_LVL(p, v)	le32p_replace_bits((__le32 *)(p), v, GENMASK(22, 21))
#define SET_RA_INFO_SGI_EN(p, v)	le32p_replace_bits((__le32 *)(p), v, BIT(23))
#define SET_RA_INFO_BW_MODE(p, v)	le32p_replace_bits((__le32 *)(p), v, GENMASK(25, 24))
#define SET_RA_INFO_LDPC(p, v)		le32p_replace_bits((__le32 *)(p), v, BIT(26))
#define SET_RA_INFO_NO_UPDATE(p, v)	le32p_replace_bits((__le32 *)(p), v, BIT(27))
#define SET_RA_INFO_VHT_EN(p, v)	le32p_replace_bits((__le32 *)(p), v, GENMASK(29, 28))
#define SET_RA_INFO_DIS_PT(p, v)	le32p_replace_bits((__le32 *)(p), v, BIT(30))
#define SET_RA_INFO_RA_MASK0(p, v)	le32p_replace_bits((__le32 *)(p) + 1, v, GENMASK(7, 0))
#define SET_RA_INFO_RA_MASK1(p, v)	le32p_replace_bits((__le32 *)(p) + 1, v, GENMASK(15, 8))
#define SET_RA_INFO_RA_MASK2(p, v)	le32p_replace_bits((__le32 *)(p) + 1, v, GENMASK(23, 16))
#define SET_RA_INFO_RA_MASK3(p, v)	le32p_replace_bits((__le32 *)(p) + 1, v, GENMASK(31, 24))
#define SET_H2C_CMD_ID_CLASS(p, v)	le32p_replace_bits((__le32 *)(p), v, GENMASK(7, 0))

static u8 get_rate_id(u8 wireless_set, u8 bw_mode, u8 tx_num)
{
	u8 rate_id = 0;

	switch (wireless_set) {
	case WIRELESS_CCK:
		rate_id = RTW_RATEID_B_20M;
		break;
	case WIRELESS_OFDM:
		rate_id = RTW_RATEID_G;
		break;
	case WIRELESS_CCK | WIRELESS_OFDM:
		rate_id = RTW_RATEID_BG;
		break;
	case WIRELESS_OFDM | WIRELESS_HT:
		if (tx_num == 1)
			rate_id = RTW_RATEID_GN_N1SS;
		else if (tx_num == 2)
			rate_id = RTW_RATEID_GN_N2SS;
		else if (tx_num == 3)
			rate_id = RTW_RATEID_ARFR5_N_3SS;
		break;
	case WIRELESS_CCK | WIRELESS_OFDM | WIRELESS_HT:
		if (bw_mode == RTW_CHANNEL_WIDTH_40) {
			if (tx_num == 1)
				rate_id = RTW_RATEID_BGN_40M_1SS;
			else if (tx_num == 2)
				rate_id = RTW_RATEID_BGN_40M_2SS;
			else if (tx_num == 3)
				rate_id = RTW_RATEID_ARFR5_N_3SS;
			else if (tx_num == 4)
				rate_id = RTW_RATEID_ARFR7_N_4SS;
		} else {
			if (tx_num == 1)
				rate_id = RTW_RATEID_BGN_20M_1SS;
			else if (tx_num == 2)
				rate_id = RTW_RATEID_BGN_20M_2SS;
			else if (tx_num == 3)
				rate_id = RTW_RATEID_ARFR5_N_3SS;
			else if (tx_num == 4)
				rate_id = RTW_RATEID_ARFR7_N_4SS;
		}
		break;
	case WIRELESS_OFDM | WIRELESS_VHT:
		if (tx_num == 1)
			rate_id = RTW_RATEID_ARFR1_AC_1SS;
		else if (tx_num == 2)
			rate_id = RTW_RATEID_ARFR0_AC_2SS;
		else if (tx_num == 3)
			rate_id = RTW_RATEID_ARFR4_AC_3SS;
		else if (tx_num == 4)
			rate_id = RTW_RATEID_ARFR6_AC_4SS;
		break;
	case WIRELESS_CCK | WIRELESS_OFDM | WIRELESS_VHT:
		if (bw_mode >= RTW_CHANNEL_WIDTH_80) {
			if (tx_num == 1)
				rate_id = RTW_RATEID_ARFR1_AC_1SS;
			else if (tx_num == 2)
				rate_id = RTW_RATEID_ARFR0_AC_2SS;
			else if (tx_num == 3)
				rate_id = RTW_RATEID_ARFR4_AC_3SS;
			else if (tx_num == 4)
				rate_id = RTW_RATEID_ARFR6_AC_4SS;
		} else {
			if (tx_num == 1)
				rate_id = RTW_RATEID_ARFR2_AC_2G_1SS;
			else if (tx_num == 2)
				rate_id = RTW_RATEID_ARFR3_AC_2G_2SS;
			else if (tx_num == 3)
				rate_id = RTW_RATEID_ARFR4_AC_3SS;
			else if (tx_num == 4)
				rate_id = RTW_RATEID_ARFR6_AC_4SS;
		}
		break;
	default:
		break;
	}

	return rate_id;
}

/* rtwb: takes the peer's VHT RX MCS map directly */
static u64 get_vht_ra_mask(u16 mcs_map)
{
	u64 ra_mask = 0;
	u8 vht_mcs_cap;
	int i, nss;

	/* 4SS, every two bits for MCS7/8/9 */
	for (i = 0, nss = 12; i < 4; i++, mcs_map >>= 2, nss += 10) {
		vht_mcs_cap = mcs_map & 0x3;
		switch (vht_mcs_cap) {
		case 2: /* MCS9 */
			ra_mask |= 0x3ffULL << nss;
			break;
		case 1: /* MCS8 */
			ra_mask |= 0x1ffULL << nss;
			break;
		case 0: /* MCS7 */
			ra_mask |= 0x0ffULL << nss;
			break;
		default:
			break;
		}
	}

	return ra_mask;
}

static u64 rtw_rate_mask_recover(u64 ra_mask, u64 ra_mask_bak)
{
	if ((ra_mask & ~(RA_MASK_CCK_RATES | RA_MASK_OFDM_RATES)) == 0)
		ra_mask |= (ra_mask_bak & ~(RA_MASK_CCK_RATES | RA_MASK_OFDM_RATES));

	if (ra_mask == 0)
		ra_mask |= (ra_mask_bak & (RA_MASK_CCK_RATES | RA_MASK_OFDM_RATES));

	return ra_mask;
}

/*
 * rtwb: mac80211's supp_rates[] bitmap, built from net80211's legacy rate
 * set.  On 2.4 GHz the bits follow the CCK+OFDM order of the RA mask; on
 * 5 GHz they start at 6 Mb/s and rtw88 shifts them by 4.
 */
static u32 rtw_supp_rates(const struct ieee80211_rateset *rs, bool is_5g)
{
	static const u8 rates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
	u32 bits = 0;
	int i, j;

	for (i = 0; i < rs->rs_nrates; i++) {
		u8 r = rs->rs_rates[i] & IEEE80211_RATE_VAL;

		for (j = is_5g ? 4 : 0; j < ARRAY_SIZE(rates); j++)
			if (rates[j] == r)
				bits |= BIT(is_5g ? j - 4 : j);
	}
	return bits;
}

/* rtw_fw_send_ra_info() */
static void rtw_fw_send_ra_info(struct rtw_dev *rtwdev,
				struct rtw_sta_info *si, bool reset_ra_mask)
{
	u8 h2c_pkt[H2C_PKT_SIZE] = {0};
	bool disable_pt = true;

	SET_H2C_CMD_ID_CLASS(h2c_pkt, H2C_CMD_RA_INFO);

	SET_RA_INFO_MACID(h2c_pkt, si->mac_id);
	SET_RA_INFO_RATE_ID(h2c_pkt, si->rate_id);
	SET_RA_INFO_INIT_RA_LVL(h2c_pkt, si->init_ra_lv);
	SET_RA_INFO_SGI_EN(h2c_pkt, si->sgi_enable);
	SET_RA_INFO_BW_MODE(h2c_pkt, si->bw_mode);
	SET_RA_INFO_LDPC(h2c_pkt, !!si->ldpc_en);
	SET_RA_INFO_NO_UPDATE(h2c_pkt, !reset_ra_mask);
	SET_RA_INFO_VHT_EN(h2c_pkt, si->vht_enable);
	SET_RA_INFO_DIS_PT(h2c_pkt, disable_pt);
	SET_RA_INFO_RA_MASK0(h2c_pkt, (si->ra_mask & 0xff));
	SET_RA_INFO_RA_MASK1(h2c_pkt, (si->ra_mask & 0xff00) >> 8);
	SET_RA_INFO_RA_MASK2(h2c_pkt, (si->ra_mask & 0xff0000) >> 16);
	SET_RA_INFO_RA_MASK3(h2c_pkt, (si->ra_mask & 0xff000000) >> 24);

	si->init_ra_lv = 0;

	rtw_fw_send_h2c_command(rtwdev, h2c_pkt);
	/* rtwb: the RA_INFO_HI command is for the 8814A only */
}

/*
 * rtw_update_sta_info() for the net80211 node 'ni' (the AP in station
 * mode), then rtw_fw_send_ra_info().
 */
void rtw_update_sta_info_80211(struct rtw_dev *rtwdev,
			       struct ieee80211_node *ni,
			       struct rtw_sta_info *si, bool reset_ra_mask)
{
	struct rtw_hal *hal = &rtwdev->hal;
	bool vht = (ni->ni_flags & IEEE80211_NODE_VHT) != 0;
	bool ht = (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	u8 wireless_set;
	u8 bw_mode;
	u8 rate_id;
	u8 stbc_en = 0;
	u8 ldpc_en = 0;
	u8 tx_num = 1;
	u64 ra_mask = 0;
	u64 ra_mask_bak = 0;
	u32 supp_rates;
	bool is_vht_enable = false;
	bool is_support_sgi = false;
	int i;

	if (vht) {
		is_vht_enable = true;
		ra_mask |= get_vht_ra_mask(le16toh(ni->ni_vht_mcsinfo.rx_mcs_map));
		if (ni->ni_vhtcap & IEEE80211_VHTCAP_RXSTBC_MASK)
			stbc_en = VHT_STBC_EN;
		if (ni->ni_vhtcap & IEEE80211_VHTCAP_RXLDPC)
			ldpc_en = VHT_LDPC_EN;
	} else if (ht) {
		/* rx_mask[0..3] << 12/20/28/36 from the HT rate set */
		for (i = 0; i < ni->ni_htrates.rs_nrates; i++) {
			u8 mcs = ni->ni_htrates.rs_rates[i] & IEEE80211_RATE_VAL;

			if (mcs < 32)
				ra_mask |= 1ULL << (12 + mcs);
		}
		if (ni->ni_htcap & IEEE80211_HTCAP_RXSTBC)
			stbc_en = HT_STBC_EN;
		if (ni->ni_htcap & IEEE80211_HTCAP_LDPC)
			ldpc_en = HT_LDPC_EN;
	}

	if (rtwdev->sc_hw_cap_nss == 1)
		ra_mask &= RA_MASK_VHT_RATES_1SS | RA_MASK_HT_RATES_1SS;
	else if (rtwdev->sc_hw_cap_nss == 2)
		ra_mask &= RA_MASK_VHT_RATES_2SS | RA_MASK_HT_RATES_2SS |
			   RA_MASK_VHT_RATES_1SS | RA_MASK_HT_RATES_1SS;

	if (hal->current_band_type == RTW_BAND_5G) {
		supp_rates = rtw_supp_rates(&ni->ni_rates, true);
		ra_mask |= (u64)supp_rates << 4;
		ra_mask_bak = ra_mask;
		if (vht) {
			ra_mask &= RA_MASK_VHT_RATES | RA_MASK_OFDM_IN_VHT;
			wireless_set = WIRELESS_OFDM | WIRELESS_VHT;
		} else if (ht) {
			ra_mask &= RA_MASK_HT_RATES | RA_MASK_OFDM_IN_HT_5G;
			wireless_set = WIRELESS_OFDM | WIRELESS_HT;
		} else {
			wireless_set = WIRELESS_OFDM;
		}
	} else {
		supp_rates = rtw_supp_rates(&ni->ni_rates, false);
		ra_mask |= supp_rates;
		ra_mask_bak = ra_mask;
		if (vht) {
			ra_mask &= RA_MASK_VHT_RATES | RA_MASK_CCK_IN_VHT |
				   RA_MASK_OFDM_IN_VHT;
			wireless_set = WIRELESS_CCK | WIRELESS_OFDM |
				       WIRELESS_HT | WIRELESS_VHT;
		} else if (ht) {
			ra_mask &= RA_MASK_HT_RATES | RA_MASK_CCK_IN_HT |
				   RA_MASK_OFDM_IN_HT_2G;
			wireless_set = WIRELESS_CCK | WIRELESS_OFDM |
				       WIRELESS_HT;
		} else if (supp_rates <= 0xf) {
			wireless_set = WIRELESS_CCK;
		} else {
			ra_mask &= RA_MASK_OFDM_RATES | RA_MASK_CCK_IN_BG;
			wireless_set = WIRELESS_CCK | WIRELESS_OFDM;
		}
	}

	/*
	 * rtwb: sta->deflink.bandwidth.  In net80211 ni_chw only covers HT
	 * 20/40 MHz; the VHT width comes from ieee80211_vht_check_tx_bw().
	 */
	switch (vht && ieee80211_vht_check_tx_bw(ni, NET80211_STA_RX_BW_80) ?
	    NET80211_STA_RX_BW_80 : ni->ni_chw) {
	case NET80211_STA_RX_BW_80:
		bw_mode = RTW_CHANNEL_WIDTH_80;
		is_support_sgi = vht &&
				 (ni->ni_vhtcap & IEEE80211_VHTCAP_SHORT_GI_80);
		break;
	case NET80211_STA_RX_BW_40:
		bw_mode = RTW_CHANNEL_WIDTH_40;
		is_support_sgi = ht &&
				 (ni->ni_htcap & IEEE80211_HTCAP_SHORTGI40);
		break;
	default:
		bw_mode = RTW_CHANNEL_WIDTH_20;
		is_support_sgi = ht &&
				 (ni->ni_htcap & IEEE80211_HTCAP_SHORTGI20);
		break;
	}

	if (vht || ht)
		tx_num = rtwdev->sc_hw_cap_nss;

	rate_id = get_rate_id(wireless_set, bw_mode, tx_num);

	/* rtw_rate_mask_rssi(): RSSI level 0 keeps every rate */
	ra_mask = rtw_rate_mask_recover(ra_mask, ra_mask_bak);

	si->mac_id = 0;		/* station mode: the vif's MAC ID */
	si->bw_mode = bw_mode;
	si->stbc_en = stbc_en;
	si->ldpc_en = ldpc_en;
	si->sgi_enable = is_support_sgi;
	si->vht_enable = is_vht_enable;
	si->ht_enable = ht;
	si->ra_mask = ra_mask;
	si->rate_id = rate_id;
	si->vht_tx_mcs_map = vht ? le16toh(ni->ni_vht_mcsinfo.tx_mcs_map) : 0;
	si->ht_2ss = ht && ni->ni_htrates.rs_nrates > 8;

	rtw_fw_send_ra_info(rtwdev, si, reset_ra_mask);
}

/* get_highest_ht_tx_rate() / get_highest_vht_tx_rate() */
u8 rtw_sta_highest_tx_rate(struct rtw_dev *rtwdev, const struct rtw_sta_info *si)
{
	u16 tx_mcs_map = si->vht_tx_mcs_map;

	if (si->vht_enable) {
		if (rtwdev->sc_hw_cap_nss == 1) {
			switch (tx_mcs_map & 0x3) {
			case IEEE80211_VHT_MCS_SUPPORT_0_7:
				return DESC_RATEVHT1SS_MCS7;
			case IEEE80211_VHT_MCS_SUPPORT_0_8:
				return DESC_RATEVHT1SS_MCS8;
			default:
				return DESC_RATEVHT1SS_MCS9;
			}
		} else {
			switch ((tx_mcs_map & 0xc) >> 2) {
			case IEEE80211_VHT_MCS_SUPPORT_0_7:
				return DESC_RATEVHT2SS_MCS7;
			case IEEE80211_VHT_MCS_SUPPORT_0_8:
				return DESC_RATEVHT2SS_MCS8;
			default:
				return DESC_RATEVHT2SS_MCS9;
			}
		}
	}
	if (si->ht_enable) {
		if (rtwdev->hal.rf_type == RF_2T2R && si->ht_2ss)
			return DESC_RATEMCS15;
		return DESC_RATEMCS7;
	}
	/* sta->deflink.supp_rates[0] <= 0xf: CCK only */
	return (si->ra_mask & RA_MASK_OFDM_RATES) ? DESC_RATE54M : DESC_RATE11M;
}
