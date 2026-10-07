/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 efuse.c, plus rtw8822b_read_efuse() and
 * rtw8822b_cfg_ldo25() from rtw8822b.c and the logical map layout of
 * struct rtw8822b_efuse from rtw8822b.h.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * Changes: 8822B/PCIe only; the logical map is decoded with byte offsets
 * instead of the packed struct; Linux errno values became positive ones.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/malloc.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include "../if_rtwbvar.h"
#include "rtw88_reg.h"
#include "rtw88_port.h"

MALLOC_DEFINE(M_RTWB_EFUSE, "rtwb_efuse", "rtwb efuse maps");

#define RTW_EFUSE_BANK_WIFI		0x0

/* Offsets into the 8822B logical efuse map (struct rtw8822b_efuse). */
#define EFUSE_8822B_TXPWR_IDX_TABLE	0x10	/* struct rtw_txpwr_idx[4] */
#define EFUSE_8822B_CHANNEL_PLAN	0xb8
#define EFUSE_8822B_XTAL_K		0xb9
#define EFUSE_8822B_THERMAL_METER	0xba
#define EFUSE_8822B_PA_TYPE		0xbc
#define EFUSE_8822B_LNA_TYPE_2G		0xbd
#define EFUSE_8822B_LNA_TYPE_5G		0xbf
#define EFUSE_8822B_RF_BOARD_OPTION	0xc1
#define EFUSE_8822B_RF_BT_SETTING	0xc3
#define EFUSE_8822B_RFE_OPTION		0xca
#define EFUSE_8822B_COUNTRY_CODE	0xcb
#define EFUSE_8822BE_MAC_ADDR		0xd0	/* rtw8822be_efuse.mac_addr */

#define invalid_efuse_header(hdr1, hdr2) \
	((hdr1) == 0xff || (((hdr1) & 0x1f) == 0xf && (hdr2) == 0xff))
#define invalid_efuse_content(word_en, i) \
	(((word_en) & BIT(i)) != 0x0)
#define get_efuse_blk_idx_2_byte(hdr1, hdr2) \
	((((hdr2) & 0xf0) >> 1) | (((hdr1) >> 5) & 0x07))
#define get_efuse_blk_idx_1_byte(hdr1) \
	(((hdr1) & 0xf0) >> 4)
#define block_idx_to_logical_idx(blk_idx, i) \
	(((blk_idx) << 3) + ((i) << 1))

static void
switch_efuse_bank(struct rtwb_softc *sc)
{
	rtwb_setbits_4(sc, REG_LDO_EFUSE_CTRL, BIT_MASK_EFUSE_BANK_SEL,
	    RTW_EFUSE_BANK_WIFI << 8);
}

static void
rtw8822b_cfg_ldo25(struct rtwb_softc *sc, bool enable)
{
	uint8_t ldo_pwr;

	ldo_pwr = rtwb_read_1(sc, REG_LDO_EFUSE_CTRL + 3);
	ldo_pwr = enable ? ldo_pwr | BIT_LDO25_EN : ldo_pwr & ~BIT_LDO25_EN;
	rtwb_write_1(sc, REG_LDO_EFUSE_CTRL + 3, ldo_pwr);
}

/* efuse header format
 *
 * | 7        5   4    0 | 7        4   3          0 | 15  8  7   0 |
 *   block[2:0]   0 1111   block[6:3]   word_en[3:0]   byte0  byte1
 * | header 1 (optional) |          header 2         |    word N    |
 *
 * word_en: 4 bits each word. 0 -> write; 1 -> not write
 * N: 1~4, depends on word_en
 */
static int
rtw_dump_logical_efuse_map(const uint8_t *phy_map, uint8_t *log_map)
{
	const uint32_t physical_size = RTW8822B_PHY_EFUSE_SIZE;
	const uint32_t protect_size = RTW8822B_PTCT_EFUSE_SIZE;
	const uint32_t logical_size = RTW8822B_LOG_EFUSE_SIZE;
	uint32_t phy_idx, log_idx;
	uint8_t hdr1, hdr2;
	uint8_t blk_idx;
	uint8_t word_en;
	int i;

	for (phy_idx = 0; phy_idx < physical_size - protect_size;) {
		hdr1 = phy_map[phy_idx];
		hdr2 = phy_map[phy_idx + 1];
		if (invalid_efuse_header(hdr1, hdr2))
			break;

		if ((hdr1 & 0x1f) == 0xf) {
			/* 2-byte header format */
			blk_idx = get_efuse_blk_idx_2_byte(hdr1, hdr2);
			word_en = hdr2 & 0xf;
			phy_idx += 2;
		} else {
			/* 1-byte header format */
			blk_idx = get_efuse_blk_idx_1_byte(hdr1);
			word_en = hdr1 & 0xf;
			phy_idx += 1;
		}

		for (i = 0; i < 4; i++) {
			if (invalid_efuse_content(word_en, i))
				continue;

			log_idx = block_idx_to_logical_idx(blk_idx, i);
			if (phy_idx + 1 > physical_size - protect_size ||
			    log_idx + 1 > logical_size)
				return (EINVAL);

			log_map[log_idx] = phy_map[phy_idx];
			log_map[log_idx + 1] = phy_map[phy_idx + 1];
			phy_idx += 2;
		}
	}
	return (0);
}

static int
rtw_dump_physical_efuse_map(struct rtwb_softc *sc, uint8_t *map)
{
	uint32_t efuse_ctl;
	uint32_t addr;
	uint32_t cnt;

	/* 8822B has no efuse_grant op. */
	switch_efuse_bank(sc);

	/* disable 2.5V LDO */
	rtw8822b_cfg_ldo25(sc, false);

	efuse_ctl = rtwb_read_4(sc, REG_EFUSE_CTRL);

	for (addr = 0; addr < RTW8822B_PHY_EFUSE_SIZE; addr++) {
		efuse_ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
		efuse_ctl |= (addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
		rtwb_write_4(sc, REG_EFUSE_CTRL, efuse_ctl & ~BIT_EF_FLAG);

		cnt = 1000000;
		do {
			DELAY(1);
			efuse_ctl = rtwb_read_4(sc, REG_EFUSE_CTRL);
			if (--cnt == 0)
				return (EBUSY);
		} while (!(efuse_ctl & BIT_EF_FLAG));

		map[addr] = (uint8_t)(efuse_ctl & BIT_MASK_EF_DATA);
	}

	return (0);
}

static void
rtw8822b_read_efuse(struct rtw_efuse *efuse, const uint8_t *log_map)
{
	efuse->rfe_option = log_map[EFUSE_8822B_RFE_OPTION];
	efuse->rf_board_option = log_map[EFUSE_8822B_RF_BOARD_OPTION];
	efuse->crystal_cap = log_map[EFUSE_8822B_XTAL_K];
	efuse->pa_type_2g = log_map[EFUSE_8822B_PA_TYPE];
	efuse->pa_type_5g = log_map[EFUSE_8822B_PA_TYPE];
	efuse->lna_type_2g = log_map[EFUSE_8822B_LNA_TYPE_2G];
	efuse->lna_type_5g = log_map[EFUSE_8822B_LNA_TYPE_5G];
	efuse->channel_plan = log_map[EFUSE_8822B_CHANNEL_PLAN];
	efuse->country_code[0] = log_map[EFUSE_8822B_COUNTRY_CODE];
	efuse->country_code[1] = log_map[EFUSE_8822B_COUNTRY_CODE + 1];
	efuse->bt_setting = log_map[EFUSE_8822B_RF_BT_SETTING];
	efuse->regd = log_map[EFUSE_8822B_RF_BOARD_OPTION] & 0x7;
	efuse->thermal_meter = log_map[EFUSE_8822B_THERMAL_METER];
	memcpy(efuse->addr, &log_map[EFUSE_8822BE_MAC_ADDR], 6);
	memcpy(efuse->txpwr_idx_table, &log_map[EFUSE_8822B_TXPWR_IDX_TABLE],
	    sizeof(efuse->txpwr_idx_table));
}

int
rtw_parse_efuse_map(struct rtwb_softc *sc, struct rtw_efuse *efuse)
{
	uint8_t *phy_map, *log_map;
	int ret;

	phy_map = malloc(RTW8822B_PHY_EFUSE_SIZE, M_RTWB_EFUSE, M_WAITOK);
	log_map = malloc(RTW8822B_LOG_EFUSE_SIZE, M_RTWB_EFUSE, M_WAITOK);

	ret = rtw_dump_physical_efuse_map(sc, phy_map);
	if (ret) {
		device_printf(sc->sc_dev,
		    "failed to dump efuse physical map\n");
		goto out_free;
	}

	memset(log_map, 0xff, RTW8822B_LOG_EFUSE_SIZE);
	ret = rtw_dump_logical_efuse_map(phy_map, log_map);
	if (ret) {
		device_printf(sc->sc_dev,
		    "failed to dump efuse logical map\n");
		goto out_free;
	}

	rtw8822b_read_efuse(efuse, log_map);

out_free:
	free(log_map, M_RTWB_EFUSE);
	free(phy_map, M_RTWB_EFUSE);
	return (ret);
}
