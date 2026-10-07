/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 phy.c: PHY condition setup, conditional register
 * table parser, table writers and RF register access through SIPI.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * Changes: only the 8822B/PCIe paths; no RF-calibration init table and no
 * BTG AGC table (the 8822B has neither).
 */

#include "rtw88_phy.h"

#define RFREG_MASK		0xfffff
#define INV_RF_DATA		0xffffffff

/* rf_base_addr / rf_sipi_addr of rtw8822b_hw_spec */
static const u32 rtw8822b_rf_base_addr[2] = {0x2800, 0x2c00};
static const u32 rtw8822b_rf_sipi_addr[2] = {0xc90, 0xe90};

struct rtw_phy_cond2 {
	u8 type_glna;
	u8 type_gpa;
	u8 type_alna;
	u8 type_apa;
};

struct phy_cfg_pair {
	u32 addr;
	u32 data;
};

union phy_table_tile {
	struct {
		struct rtw_phy_cond cond;
		struct rtw_phy_cond2 cond2;
	} __packed;
	struct phy_cfg_pair cfg;
};

_Static_assert(sizeof(union phy_table_tile) == sizeof(struct phy_cfg_pair),
    "phy_table_tile must be one address/data pair");

u32
rtw_phy_read_rf(struct rtw_dev *rtwdev, u8 rf_path, u32 addr, u32 mask)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 direct_addr;

	if (rf_path >= hal->rf_phy_num) {
		rtw_err(rtwdev, "unsupported rf path (%d)\n", rf_path);
		return INV_RF_DATA;
	}

	addr &= 0xff;
	direct_addr = rtw8822b_rf_base_addr[rf_path] + (addr << 2);
	mask &= RFREG_MASK;

	return rtw_read32_mask(rtwdev, direct_addr, mask);
}

bool
rtw_phy_write_rf_reg_sipi(struct rtw_dev *rtwdev, u8 rf_path, u32 addr,
    u32 mask, u32 data)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 data_and_addr;
	u32 old_data = 0;
	u32 shift;

	if (rf_path >= hal->rf_phy_num) {
		rtw_err(rtwdev, "unsupported rf path (%d)\n", rf_path);
		return false;
	}

	addr &= 0xff;
	mask &= RFREG_MASK;

	if (mask != RFREG_MASK) {
		old_data = rtw_phy_read_rf(rtwdev, rf_path, addr, RFREG_MASK);

		if (old_data == INV_RF_DATA) {
			rtw_err(rtwdev, "Write fail, rf is disabled\n");
			return false;
		}

		shift = __ffs(mask);
		data = ((old_data) & (~mask)) | (data << shift);
	}

	data_and_addr = ((addr << 20) | (data & 0x000fffff)) & 0x0fffffff;

	rtw_write32(rtwdev, rtw8822b_rf_sipi_addr[rf_path], data_and_addr);

	udelay(13);

	return true;
}

/* rtw_phy_setup_phy_cond(); the 8822B has no package type (pkg 0). */
void
rtw_phy_setup_phy_cond(struct rtw_dev *rtwdev, u32 pkg)
{
	struct rtw_hal *hal = &rtwdev->hal;
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw_phy_cond cond = {};

	cond.cut = hal->cut_version ? hal->cut_version : 15;
	cond.pkg = pkg ? pkg : 15;
	cond.plat = 0x04;
	cond.rfe = efuse->rfe_option;
	cond.intf = INTF_PCIE;

	hal->phy_cond = cond;
}

static bool
check_positive(struct rtw_dev *rtwdev, struct rtw_phy_cond cond)
{
	struct rtw_hal *hal = &rtwdev->hal;
	struct rtw_phy_cond drv_cond = hal->phy_cond;

	if (cond.cut && cond.cut != drv_cond.cut)
		return false;

	if (cond.pkg && cond.pkg != drv_cond.pkg)
		return false;

	if (cond.intf && cond.intf != drv_cond.intf)
		return false;

	if (cond.rfe != drv_cond.rfe)
		return false;

	return true;
}

void
rtw_parse_tbl_phy_cond(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{
	const union phy_table_tile *p = tbl->data;
	const union phy_table_tile *end = p + tbl->size / 2;
	struct rtw_phy_cond pos_cond = {};
	bool is_matched = true, is_skipped = false;

	for (; p < end; p++) {
		if (p->cond.pos) {
			switch (p->cond.branch) {
			case BRANCH_ENDIF:
				is_matched = true;
				is_skipped = false;
				break;
			case BRANCH_ELSE:
				is_matched = is_skipped ? false : true;
				break;
			case BRANCH_IF:
			case BRANCH_ELIF:
			default:
				pos_cond = p->cond;
				break;
			}
		} else if (p->cond.neg) {
			if (!is_skipped) {
				if (check_positive(rtwdev, pos_cond)) {
					is_matched = true;
					is_skipped = true;
				} else {
					is_matched = false;
					is_skipped = false;
				}
			} else {
				is_matched = false;
			}
		} else if (is_matched) {
			(*tbl->do_cfg)(rtwdev, tbl, p->cfg.addr, p->cfg.data);
		}
	}
}

void
rtw_phy_cfg_mac(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
    u32 addr, u32 data)
{
	rtw_write8(rtwdev, addr, data);
}

void
rtw_phy_cfg_agc(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
    u32 addr, u32 data)
{
	rtw_write32(rtwdev, addr, data);
}

void
rtw_phy_cfg_bb(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
    u32 addr, u32 data)
{
	if (addr == 0xfe)
		msleep(50);
	else if (addr == 0xfd)
		mdelay(5);
	else if (addr == 0xfc)
		mdelay(1);
	else if (addr == 0xfb)
		usleep_range(50, 60);
	else if (addr == 0xfa)
		udelay(5);
	else if (addr == 0xf9)
		udelay(1);
	else
		rtw_write32(rtwdev, addr, data);
}

void
rtw_phy_cfg_rf(struct rtw_dev *rtwdev, const struct rtw_table *tbl,
    u32 addr, u32 data)
{
	if (addr == 0xffe) {
		msleep(50);
	} else if (addr == 0xfe) {
		usleep_range(100, 110);
	} else {
		rtw_write_rf(rtwdev, tbl->rf_path, addr, RFREG_MASK, data);
		udelay(1);
	}
}

void
rtw_phy_load_tables(struct rtw_dev *rtwdev)
{
	static const struct rtw_table * const rf_tbl[2] = {
		&rtw8822b_rf_a_tbl, &rtw8822b_rf_b_tbl
	};
	u8 rf_path;

	rtw_load_table(rtwdev, &rtw8822b_mac_tbl);
	rtw_load_table(rtwdev, &rtw8822b_bb_tbl);
	rtw_load_table(rtwdev, &rtw8822b_agc_tbl);

	for (rf_path = 0; rf_path < rtwdev->hal.rf_path_num; rf_path++)
		rtw_load_table(rtwdev, rf_tbl[rf_path]);
}
