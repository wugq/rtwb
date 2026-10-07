/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 tx.c and tx.h: TX packet descriptor fill
 * (rtw_tx_fill_tx_desc, verbatim apart from taking the descriptor
 * pointer), and the per-frame decisions of rtw_tx_pkt_info_update(),
 * rtw_tx_mgmt_pkt_info_update(), rtw_tx_data_pkt_info_update() and
 * rtw_tx_queue_mapping(), rewritten for net80211 mbufs.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#include "rtw88_phy.h"

#include <net80211/ieee80211_var.h>

struct rtw_tx_desc {
	__le32 w0;
	__le32 w1;
	__le32 w2;
	__le32 w3;
	__le32 w4;
	__le32 w5;
	__le32 w6;
	__le32 w7;
	__le32 w8;
	__le32 w9;
} __packed;

#define RTW_TX_DESC_W0_TXPKTSIZE GENMASK(15, 0)
#define RTW_TX_DESC_W0_OFFSET GENMASK(23, 16)
#define RTW_TX_DESC_W0_BMC BIT(24)
#define RTW_TX_DESC_W0_LS BIT(26)
#define RTW_TX_DESC_W0_DISQSELSEQ BIT(31)
#define RTW_TX_DESC_W1_MACID GENMASK(7, 0)
#define RTW_TX_DESC_W1_QSEL GENMASK(12, 8)
#define RTW_TX_DESC_W1_RATE_ID GENMASK(20, 16)
#define RTW_TX_DESC_W1_SEC_TYPE GENMASK(23, 22)
#define RTW_TX_DESC_W1_PKT_OFFSET GENMASK(28, 24)
#define RTW_TX_DESC_W1_MORE_DATA BIT(29)
#define RTW_TX_DESC_W2_AGG_EN BIT(12)
#define RTW_TX_DESC_W2_SPE_RPT BIT(19)
#define RTW_TX_DESC_W2_AMPDU_DEN GENMASK(22, 20)
#define RTW_TX_DESC_W2_BT_NULL BIT(23)
#define RTW_TX_DESC_W3_HW_SSN_SEL GENMASK(7, 6)
#define RTW_TX_DESC_W3_USE_RATE BIT(8)
#define RTW_TX_DESC_W3_DISDATAFB BIT(10)
#define RTW_TX_DESC_W3_USE_RTS BIT(12)
#define RTW_TX_DESC_W3_NAVUSEHDR BIT(15)
#define RTW_TX_DESC_W3_MAX_AGG_NUM GENMASK(21, 17)
#define RTW_TX_DESC_W4_DATARATE GENMASK(6, 0)
#define RTW_TX_DESC_W4_DATARATE_FB_LIMIT GENMASK(12, 8)
#define RTW_TX_DESC_W4_RTSRATE GENMASK(28, 24)
#define RTW_TX_DESC_W5_DATA_SHORT BIT(4)
#define RTW_TX_DESC_W5_DATA_BW GENMASK(6, 5)
#define RTW_TX_DESC_W5_DATA_LDPC BIT(7)
#define RTW_TX_DESC_W5_DATA_STBC GENMASK(9, 8)
#define RTW_TX_DESC_W5_DATA_RTS_SHORT BIT(12)
#define RTW_TX_DESC_W6_SW_DEFINE GENMASK(11, 0)
#define RTW_TX_DESC_W7_TXDESC_CHECKSUM GENMASK(15, 0)
#define RTW_TX_DESC_W7_DMA_TXAGG_NUM GENMASK(31, 24)
#define RTW_TX_DESC_W8_EN_HWSEQ BIT(15)
#define RTW_TX_DESC_W9_SW_SEQ GENMASK(23, 12)
#define RTW_TX_DESC_W9_TIM_EN BIT(7)
#define RTW_TX_DESC_W9_TIM_OFFSET GENMASK(6, 0)
/* enum rtw_tx_desc_queue_select: TIDs 0..15, then these (rtw88_reg.h) */

#define RTW_RATEID_BG		6
#define RTW_RATEID_B_20M	8

void rtw_tx_fill_tx_desc(struct rtw_dev *rtwdev,
			 struct rtw_tx_pkt_info *pkt_info, u8 *desc)
{
	struct rtw_tx_desc *tx_desc = (struct rtw_tx_desc *)desc;
	bool more_data = false;

	if (pkt_info->qsel == TX_DESC_QSEL_HIGH)
		more_data = true;

	tx_desc->w0 = le32_encode_bits(pkt_info->tx_pkt_size, RTW_TX_DESC_W0_TXPKTSIZE) |
		      le32_encode_bits(pkt_info->offset, RTW_TX_DESC_W0_OFFSET) |
		      le32_encode_bits(pkt_info->bmc, RTW_TX_DESC_W0_BMC) |
		      le32_encode_bits(pkt_info->ls, RTW_TX_DESC_W0_LS) |
		      le32_encode_bits(pkt_info->dis_qselseq, RTW_TX_DESC_W0_DISQSELSEQ);

	tx_desc->w1 = le32_encode_bits(pkt_info->mac_id, RTW_TX_DESC_W1_MACID) |
		      le32_encode_bits(pkt_info->qsel, RTW_TX_DESC_W1_QSEL) |
		      le32_encode_bits(pkt_info->rate_id, RTW_TX_DESC_W1_RATE_ID) |
		      le32_encode_bits(pkt_info->sec_type, RTW_TX_DESC_W1_SEC_TYPE) |
		      le32_encode_bits(pkt_info->pkt_offset, RTW_TX_DESC_W1_PKT_OFFSET) |
		      le32_encode_bits(more_data, RTW_TX_DESC_W1_MORE_DATA);

	tx_desc->w2 = le32_encode_bits(pkt_info->ampdu_en, RTW_TX_DESC_W2_AGG_EN) |
		      le32_encode_bits(pkt_info->report, RTW_TX_DESC_W2_SPE_RPT) |
		      le32_encode_bits(pkt_info->ampdu_density, RTW_TX_DESC_W2_AMPDU_DEN) |
		      le32_encode_bits(pkt_info->bt_null, RTW_TX_DESC_W2_BT_NULL);

	tx_desc->w3 = le32_encode_bits(pkt_info->hw_ssn_sel, RTW_TX_DESC_W3_HW_SSN_SEL) |
		      le32_encode_bits(pkt_info->use_rate, RTW_TX_DESC_W3_USE_RATE) |
		      le32_encode_bits(pkt_info->dis_rate_fallback, RTW_TX_DESC_W3_DISDATAFB) |
		      le32_encode_bits(pkt_info->rts, RTW_TX_DESC_W3_USE_RTS) |
		      le32_encode_bits(pkt_info->nav_use_hdr, RTW_TX_DESC_W3_NAVUSEHDR) |
		      le32_encode_bits(pkt_info->ampdu_factor, RTW_TX_DESC_W3_MAX_AGG_NUM);

	tx_desc->w4 = le32_encode_bits(pkt_info->rate, RTW_TX_DESC_W4_DATARATE);

	/* rtwb: the 8822B has no old_datarate_fb_limit */

	tx_desc->w5 = le32_encode_bits(pkt_info->short_gi, RTW_TX_DESC_W5_DATA_SHORT) |
		      le32_encode_bits(pkt_info->bw, RTW_TX_DESC_W5_DATA_BW) |
		      le32_encode_bits(pkt_info->ldpc, RTW_TX_DESC_W5_DATA_LDPC) |
		      le32_encode_bits(pkt_info->stbc, RTW_TX_DESC_W5_DATA_STBC);

	tx_desc->w6 = le32_encode_bits(pkt_info->sn, RTW_TX_DESC_W6_SW_DEFINE);

	tx_desc->w8 = le32_encode_bits(pkt_info->en_hwseq, RTW_TX_DESC_W8_EN_HWSEQ);

	tx_desc->w9 = le32_encode_bits(pkt_info->seq, RTW_TX_DESC_W9_SW_SEQ);

	if (pkt_info->rts) {
		tx_desc->w4 |= le32_encode_bits(DESC_RATE24M, RTW_TX_DESC_W4_RTSRATE);
		tx_desc->w5 |= le32_encode_bits(1, RTW_TX_DESC_W5_DATA_RTS_SHORT);
	}

	if (pkt_info->tim_offset)
		tx_desc->w9 |= le32_encode_bits(1, RTW_TX_DESC_W9_TIM_EN) |
			       le32_encode_bits(pkt_info->tim_offset, RTW_TX_DESC_W9_TIM_OFFSET);
}

/* ac_to_hwq[] with net80211's WME_AC_* order (BE, BK, VI, VO). */
static const int rtw_ac_to_hwq[WME_NUM_AC] = {
	[WME_AC_BE] = RTWB_TXQ_BE,
	[WME_AC_BK] = RTWB_TXQ_BK,
	[WME_AC_VI] = RTWB_TXQ_VI,
	[WME_AC_VO] = RTWB_TXQ_VO,
};

/* rtw_tx_queue_mapping() alone, for a frame that is not encrypted yet. */
int
rtw_tx_queue_80211(struct mbuf *m)
{
	const struct ieee80211_frame *wh = mtod(m, const struct ieee80211_frame *);
	uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	if (type != IEEE80211_FC0_TYPE_DATA)
		return (RTWB_TXQ_MGMT);
	if (IEEE80211_IS_MULTICAST(wh->i_addr1))
		return (RTWB_TXQ_HI0);
	return (rtw_ac_to_hwq[M_WME_GETAC(m)]);
}

/*
 * rtw_tx_pkt_info_update() for one net80211 frame; returns the TX queue
 * (rtw_tx_queue_mapping).  rtwb: crypto is done in software by net80211,
 * so sec_type stays 0.  Unicast data to the associated AP goes through the
 * firmware rate adaptation of its MAC ID (rtw_tx_data_pkt_info_update()
 * with a station); everything else uses the "no station" defaults, with a
 * fixed 6 Mb/s for data.
 */
int
rtw_tx_pkt_info_80211(struct rtw_dev *rtwdev, struct ieee80211_node *ni,
    struct mbuf *m, struct rtw_tx_pkt_info *pkt_info)
{
	struct ieee80211com *ic = ni->ni_ic;
	const struct ieee80211_frame *wh;
	bool is_5g, is_mgmt, bmc;
	uint8_t type, subtype;
	int queue;

	wh = mtod(m, const struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	is_5g = IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan);
	bmc = IEEE80211_IS_MULTICAST(wh->i_addr1);

	memset(pkt_info, 0, sizeof(*pkt_info));
	pkt_info->mac_id = 0;

	/* ieee80211_is_mgmt() || ieee80211_is_nullfunc() */
	is_mgmt = type != IEEE80211_FC0_TYPE_DATA ||
	    (subtype & IEEE80211_FC0_SUBTYPE_NODATA) != 0;
	if (is_mgmt) {
		/* rtw_tx_mgmt_pkt_info_update() + _update_rate() */
		pkt_info->rate_id = is_5g ? RTW_RATEID_G : RTW_RATEID_B_20M;
		pkt_info->rate = is_5g ? DESC_RATE6M : DESC_RATE1M;
		pkt_info->use_rate = true;
		pkt_info->dis_rate_fallback = true;
		pkt_info->dis_qselseq = true;
		pkt_info->en_hwseq = true;
		pkt_info->hw_ssn_sel = 0;
	} else if (rtwdev->sc_assoc && !bmc && ni == ni->ni_vap->iv_bss) {
		/* rtw_tx_data_pkt_info_update() with a station */
		const struct rtw_sta_info *si = &rtwdev->sc_sta;

		pkt_info->seq = le16toh(*(const uint16_t *)wh->i_seq) >>
		    IEEE80211_SEQ_SEQ_SHIFT;
		pkt_info->rate = rtw_sta_highest_tx_rate(rtwdev, si);
		pkt_info->rate_id = si->rate_id;
		pkt_info->bw = si->bw_mode;
		pkt_info->stbc = rtwdev->hal.rf_path_num > 1 ? si->stbc_en : 0;
		pkt_info->ldpc = si->ldpc_en;
		pkt_info->mac_id = si->mac_id;
	} else {
		/* rtw_tx_data_pkt_info_update() without a station entry */
		pkt_info->seq = le16toh(*(const uint16_t *)wh->i_seq) >>
		    IEEE80211_SEQ_SEQ_SHIFT;
		pkt_info->rate = DESC_RATE6M;
		pkt_info->rate_id = is_5g ? RTW_RATEID_G : RTW_RATEID_BG;
		pkt_info->bw = RTW_CHANNEL_WIDTH_20;
		pkt_info->use_rate = true;
		pkt_info->dis_rate_fallback = true;
	}

	pkt_info->bmc = bmc;
	pkt_info->tx_pkt_size = m->m_pkthdr.len;
	pkt_info->offset = RTWB_TX_PKT_DESC_SZ;
	pkt_info->ls = true;

	/* rtw_tx_queue_mapping() and the PCI queue select */
	if (type != IEEE80211_FC0_TYPE_DATA) {
		queue = RTWB_TXQ_MGMT;
		pkt_info->qsel = TX_DESC_QSEL_MGMT;
	} else if (bmc) {
		queue = RTWB_TXQ_HI0;
		pkt_info->qsel = TX_DESC_QSEL_HIGH;
	} else {
		queue = rtw_ac_to_hwq[M_WME_GETAC(m)];
		pkt_info->qsel = IEEE80211_QOS_HAS_SEQ(wh) ?
		    ieee80211_gettid(wh) : 0;
	}
	return (queue);
}
