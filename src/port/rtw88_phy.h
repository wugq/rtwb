/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 phy.h and main.h: PHY register table types.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#ifndef _RTW88_PHY_H_
#define _RTW88_PHY_H_

#include "rtw88_compat.h"
#include "rtw88_reg_phy.h"

struct rtw_table {
	const void *data;
	const u32 size;
	void (*parse)(struct rtw_dev *rtwdev, const struct rtw_table *tbl);
	void (*do_cfg)(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
		       u32 addr, u32 data);
	enum rtw_rf_path rf_path;
};

static inline void rtw_load_table(struct rtw_dev *rtwdev,
				  const struct rtw_table *tbl)
{
	(*tbl->parse)(rtwdev, tbl);
}

struct rtw_txpwr_lmt_cfg_pair {
	u8 regd;
	u8 band;
	u8 bw;
	u8 rs;
	u8 ch;
	s8 txpwr_lmt;
};

struct rtw_phy_pg_cfg_pair {
	u32 band;
	u32 rf_path;
	u32 tx_num;
	u32 addr;
	u32 bitmask;
	u32 data;
};

void rtw_parse_tbl_phy_cond(struct rtw_dev *rtwdev, const struct rtw_table *tbl);
void rtw_phy_cfg_mac(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
		     u32 addr, u32 data);
void rtw_phy_cfg_agc(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
		     u32 addr, u32 data);
void rtw_phy_cfg_bb(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
		    u32 addr, u32 data);
void rtw_phy_cfg_rf(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
		    u32 addr, u32 data);

void rtw_phy_setup_phy_cond(struct rtw_dev *rtwdev, u32 pkg);
void rtw_phy_load_tables(struct rtw_dev *rtwdev);
void rtw_set_channel_mac(struct rtw_dev *rtwdev, u8 channel, u8 bw,
			 u8 primary_ch_idx);
void rtw_bf_phy_init(struct rtw_dev *rtwdev);

#define RTW_DECL_TABLE_PHY_COND_CORE(name, cfg, path)	\
const struct rtw_table name ## _tbl = {			\
	.data = name,					\
	.size = ARRAY_SIZE(name),			\
	.parse = rtw_parse_tbl_phy_cond,		\
	.do_cfg = cfg,					\
	.rf_path = path,				\
}

#define RTW_DECL_TABLE_PHY_COND(name, cfg)		\
	RTW_DECL_TABLE_PHY_COND_CORE(name, cfg, 0)

#define RTW_DECL_TABLE_RF_RADIO(name, path)		\
	RTW_DECL_TABLE_PHY_COND_CORE(name, rtw_phy_cfg_rf, RF_PATH_ ## path)

void rtw_parse_tbl_bb_pg(struct rtw_dev *rtwdev, const struct rtw_table *tbl);
void rtw_parse_tbl_txpwr_lmt(struct rtw_dev *rtwdev,
			     const struct rtw_table *tbl);

#define RTW_DECL_TABLE_BB_PG(name)			\
const struct rtw_table name ## _tbl = {			\
	.data = name,					\
	.size = ARRAY_SIZE(name),			\
	.parse = rtw_parse_tbl_bb_pg,			\
}

#define RTW_DECL_TABLE_TXPWR_LMT(name)			\
const struct rtw_table name ## _tbl = {			\
	.data = name,					\
	.size = ARRAY_SIZE(name),			\
	.parse = rtw_parse_tbl_txpwr_lmt,		\
}

/* rtw8822b_table.h */
extern const struct rtw_table rtw8822b_mac_tbl;
extern const struct rtw_table rtw8822b_agc_tbl;
extern const struct rtw_table rtw8822b_bb_tbl;
extern const struct rtw_table rtw8822b_bb_pg_type2_tbl;
extern const struct rtw_table rtw8822b_bb_pg_type3_tbl;
extern const struct rtw_table rtw8822b_bb_pg_type5_tbl;
extern const struct rtw_table rtw8822b_rf_a_tbl;
extern const struct rtw_table rtw8822b_rf_b_tbl;
extern const struct rtw_table rtw8822b_txpwr_lmt_type0_tbl;
extern const struct rtw_table rtw8822b_txpwr_lmt_type2_tbl;
extern const struct rtw_table rtw8822b_txpwr_lmt_type5_tbl;

#endif /* _RTW88_PHY_H_ */
