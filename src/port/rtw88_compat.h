/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Small Linux/rtw88 compatibility layer so that the files in this
 * directory can keep rtw88's code nearly line for line.  Only for port/;
 * the driver itself does not use it.
 *
 * The helpers mirror rtw88 hci.h, main.h and rtw8822b.h.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#ifndef _RTW88_COMPAT_H_
#define _RTW88_COMPAT_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include "../if_rtwbvar.h"
#include "rtw88_reg.h"
#include "rtw88_port.h"

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int8_t		s8;
typedef int16_t		s16;
typedef int32_t		s32;
typedef uint16_t	__le16;
typedef uint32_t	__le32;

/* rtw88 passes "struct rtw_dev *rtwdev" everywhere; ours is the softc. */
#define rtw_dev			rtwb_softc

#ifndef GENMASK
#define GENMASK(h, l)	(((~0U) >> (31 - (h))) & ~((1U << (l)) - 1))
#endif
#define ARRAY_SIZE(a)		nitems(a)
#define fallthrough		__attribute__((__fallthrough__))
#define __ffs(x)		(ffs(x) - 1)

#define udelay(us)		DELAY(us)
#define mdelay(ms)		DELAY((ms) * 1000)
#undef msleep			/* FreeBSD's msleep(9) is not used in port/ */
#define msleep(ms)		rtw_msleep(ms)
#define usleep_range(a, b)	DELAY(a)

#define min_t(t, a, b)		((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b)		((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define min3(a, b, c)		MIN(MIN(a, b), c)
#define max3(a, b, c)		MAX(MAX(a, b), c)
#define clamp_t(t, v, lo, hi)	min_t(t, max_t(t, v, lo), hi)

/*
 * Linux msleep() sleeps.  In port/ it is only used by the register table
 * loaders, which run at attach or under sc_sx without sc_mtx held.
 */
static inline void
rtw_msleep(unsigned int ms)
{
	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, NULL, "rtw88 msleep");
	pause_sbt("rtwbms", SBT_1MS * ms, 0, C_PREL(1));
}

#define rtw_dbg(rtwdev, mask, ...)	do { } while (0)
#define rtw_err(rtwdev, ...)	device_printf((rtwdev)->sc_dev, __VA_ARGS__)
#define rtw_warn(rtwdev, ...)	device_printf((rtwdev)->sc_dev, __VA_ARGS__)

#define WARN(cond, ...) ({						\
	bool __c = (cond);						\
	if (__c)							\
		printf("rtwb: " __VA_ARGS__);				\
	__c;								\
})
#define WARN_ON(cond)		WARN(cond, "WARN_ON(%s)\n", #cond)

/* <linux/bitfield.h> on little-endian 32-bit words */
static inline uint32_t
le32_encode_bits(uint32_t v, uint32_t mask)
{
	return (htole32((v << __ffs(mask)) & mask));
}

static inline void
le32p_replace_bits(uint32_t *p, uint32_t v, uint32_t mask)
{
	*p = htole32((le32toh(*p) & ~mask) | ((v << __ffs(mask)) & mask));
}

/* The 8822B has a 3081 WLAN CPU, not an 8051. */
#define rtw_chip_wcpu_8051(rtwdev)	false
#define rtw_chip_wcpu_3081(rtwdev)	true

/* hci.h register access */
#define rtw_read8(rtwdev, a)		rtwb_read_1(rtwdev, a)
#define rtw_read16(rtwdev, a)		rtwb_read_2(rtwdev, a)
#define rtw_read32(rtwdev, a)		rtwb_read_4(rtwdev, a)
#define rtw_write8(rtwdev, a, v)	rtwb_write_1(rtwdev, a, v)
#define rtw_write16(rtwdev, a, v)	rtwb_write_2(rtwdev, a, v)
#define rtw_write32(rtwdev, a, v)	rtwb_write_4(rtwdev, a, v)

#define rtw_write8_set(rtwdev, a, b)	rtwb_setbits_1(rtwdev, a, 0, b)
#define rtw_write8_clr(rtwdev, a, b)	rtwb_setbits_1(rtwdev, a, b, 0)
#define rtw_write32_set(rtwdev, a, b)	rtwb_setbits_4(rtwdev, a, 0, b)
#define rtw_write32_clr(rtwdev, a, b)	rtwb_setbits_4(rtwdev, a, b, 0)

static inline void
rtw_write16_set(struct rtwb_softc *sc, uint32_t addr, uint16_t bit)
{
	rtwb_write_2(sc, addr, rtwb_read_2(sc, addr) | bit);
}

static inline void
rtw_write16_clr(struct rtwb_softc *sc, uint32_t addr, uint16_t bit)
{
	rtwb_write_2(sc, addr, rtwb_read_2(sc, addr) & ~bit);
}

static inline u32
rtw_read32_mask(struct rtwb_softc *sc, u32 addr, u32 mask)
{
	return ((rtwb_read_4(sc, addr) & mask) >> __ffs(mask));
}

static inline void
rtw_write32_mask(struct rtwb_softc *sc, u32 addr, u32 mask, u32 data)
{
	u32 shift = __ffs(mask);
	u32 orig, set;

	WARN(addr & 0x3, "should be 4-byte aligned, addr = 0x%08x\n", addr);

	orig = rtwb_read_4(sc, addr);
	set = (orig & ~mask) | ((data << shift) & mask);
	rtwb_write_4(sc, addr, set);
}

static inline void
rtw_write8_mask(struct rtwb_softc *sc, u32 addr, u32 mask, u8 data)
{
	u32 shift = __ffs(mask);
	u8 orig, set;

	orig = rtwb_read_1(sc, addr);
	set = (orig & ~mask) | ((data << shift) & mask);
	rtwb_write_1(sc, addr, set);
}

/* rtw8822b.h: 0xC00-0xCFF and 0xE00-0xEFF have the same layout */
#define rtw_write32s_mask(rtwdev, addr, mask, data)			\
	do {								\
		rtw_write32_mask(rtwdev, addr, mask, data);		\
		rtw_write32_mask(rtwdev, (addr) + 0x200, mask, data);	\
	} while (0)

/* RF register access (rtw88_phy.c; 8822B uses SIPI writes). */
u32	rtw_phy_read_rf(struct rtwb_softc *, u8, u32, u32);
bool	rtw_phy_write_rf_reg_sipi(struct rtwb_softc *, u8, u32, u32, u32);
#define rtw_read_rf(rtwdev, path, addr, mask)				\
	rtw_phy_read_rf(rtwdev, path, addr, mask)
#define rtw_write_rf(rtwdev, path, addr, mask, data)			\
	rtw_phy_write_rf_reg_sipi(rtwdev, path, addr, mask, data)

/* main.h */
#define IS_CH_5G_BAND_1(channel) ((channel) >= 36 && (channel <= 48))
#define IS_CH_5G_BAND_2(channel) ((channel) >= 52 && (channel <= 64))
#define IS_CH_5G_BAND_3(channel) ((channel) >= 100 && (channel <= 144))
#define IS_CH_5G_BAND_4(channel) ((channel) >= 149 && (channel <= 177))
#define IS_CH_5G_BAND_MID(channel) \
	(IS_CH_5G_BAND_2(channel) || IS_CH_5G_BAND_3(channel))
#define IS_CH_2G_BAND(channel) ((channel) <= 14)
#define IS_CH_5G_BAND(channel) \
	(IS_CH_5G_BAND_1(channel) || IS_CH_5G_BAND_2(channel) || \
	 IS_CH_5G_BAND_3(channel) || IS_CH_5G_BAND_4(channel))

enum rtw_rf_path {
	RF_PATH_A = 0,
	RF_PATH_B = 1,
	RF_PATH_C = 2,
	RF_PATH_D = 3,
};

/* enum rtw_bandwidth; 20/40/80 are in rtw88_reg.h */
#define RTW_CHANNEL_WIDTH_160	3
#define RTW_CHANNEL_WIDTH_80_80	4
#define RTW_CHANNEL_WIDTH_5	5
#define RTW_CHANNEL_WIDTH_10	6

enum rtw_sc_offset {
	RTW_SC_DONT_CARE	= 0,
	RTW_SC_20_UPPER		= 1,
	RTW_SC_20_LOWER		= 2,
	RTW_SC_20_UPMOST	= 3,
	RTW_SC_20_LOWEST	= 4,
	RTW_SC_40_UPPER		= 9,
	RTW_SC_40_LOWER		= 10,
};

#define RTW_BAND_2G		BIT(0)	/* BIT(NL80211_BAND_2GHZ) */
#define RTW_BAND_5G		BIT(1)	/* BIT(NL80211_BAND_5GHZ) */

enum rtw_chip_ver {
	RTW_CHIP_VER_CUT_A = 0x00,
	RTW_CHIP_VER_CUT_B = 0x01,
	RTW_CHIP_VER_CUT_C = 0x02,
	RTW_CHIP_VER_CUT_D = 0x03,
	RTW_CHIP_VER_CUT_E = 0x04,
	RTW_CHIP_VER_CUT_F = 0x05,
	RTW_CHIP_VER_CUT_G = 0x06,
};

enum rtw_rfe_fem {
	RTW_RFE_IFEM,
	RTW_RFE_EFEM,
	RTW_RFE_IFEM2G_EFEM5G,
	RTW_RFE_NUM,
};

#endif /* _RTW88_COMPAT_H_ */
