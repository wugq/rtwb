/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 phy.c (TX power: by-rate offsets, regulatory
 * limits, per-rate power index from the efuse calibration, programming
 * per channel), regd.c (regulatory alternatives) and rtw8822b.c
 * (rtw8822b_set_tx_power_index).  The functions are copied verbatim except
 * where marked "rtwb:".
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * rtwb: the regulatory domain follows net80211's country code through
 * rtw88's country table (regd.c); unknown countries use RTW_REGD_WW, the
 * minimum of all regions' limits.  No SAR limits and no power tracking yet.
 */

#include "rtw88_phy.h"

#define RTW8822B_MAX_POWER_INDEX	0x3f

/* Linux bcd2bin() takes a u8; FreeBSD's takes an int and indexes a table. */
#define bcd_to_dec_pwr_by_rate(val, i)					\
	((((val) >> ((i) * 8)) & 0x0f) + ((((val) >> ((i) * 8 + 4)) & 0x0f) * 10))

#define PHY_BAND_2G	0
#define PHY_BAND_5G	1

struct rtw_power_params {
	u8 pwr_base;
	s8 pwr_offset;
	s8 pwr_limit;
	s8 pwr_remnant;
	s8 pwr_sar;
};

enum rtw_rate_section {
	RTW_RATE_SECTION_CCK = 0,
	RTW_RATE_SECTION_OFDM,
	RTW_RATE_SECTION_HT_1S,
	RTW_RATE_SECTION_HT_2S,
	RTW_RATE_SECTION_VHT_1S,
	RTW_RATE_SECTION_VHT_2S,
	__RTW_RATE_SECTION_2SS_MAX = RTW_RATE_SECTION_VHT_2S,
	RTW_RATE_SECTION_HT_3S,
	RTW_RATE_SECTION_HT_4S,
	RTW_RATE_SECTION_VHT_3S,
	RTW_RATE_SECTION_VHT_4S,

	/* keep last */
	RTW_RATE_SECTION_NUM,
};

enum rtw_regulatory_domains {
	RTW_REGD_FCC		= 0,
	RTW_REGD_MKK		= 1,
	RTW_REGD_ETSI		= 2,
	RTW_REGD_IC		= 3,
	RTW_REGD_KCC		= 4,
	RTW_REGD_ACMA		= 5,
	RTW_REGD_CHILE		= 6,
	RTW_REGD_UKRAINE	= 7,
	RTW_REGD_MEXICO		= 8,
	RTW_REGD_CN		= 9,
	RTW_REGD_QATAR		= 10,
	RTW_REGD_UK		= 11,

	RTW_REGD_WW,
	RTW_REGD_MAX
};

_Static_assert(RTW_RATE_SECTION_NUM == RTW_RATE_SECTION_NUM_, "sizes");
_Static_assert(RTW_REGD_MAX == RTW_REGD_MAX_, "sizes");
_Static_assert(DESC_RATE_MAX == DESC_RATE_MAX_, "sizes");

/* regd.c */
struct rtw_regd_alternative_t {
	bool set;
	u8 alt;
};

#define DECL_REGD_ALT(_regd, _regd_alt) \
	[(_regd)] = {.set = true, .alt = (_regd_alt)}

static const struct rtw_regd_alternative_t
rtw_regd_alt[RTW_REGD_MAX] = {
	DECL_REGD_ALT(RTW_REGD_IC, RTW_REGD_FCC),
	DECL_REGD_ALT(RTW_REGD_KCC, RTW_REGD_ETSI),
	DECL_REGD_ALT(RTW_REGD_ACMA, RTW_REGD_ETSI),
	DECL_REGD_ALT(RTW_REGD_CHILE, RTW_REGD_FCC),
	DECL_REGD_ALT(RTW_REGD_UKRAINE, RTW_REGD_ETSI),
	DECL_REGD_ALT(RTW_REGD_MEXICO, RTW_REGD_FCC),
	DECL_REGD_ALT(RTW_REGD_CN, RTW_REGD_ETSI),
	DECL_REGD_ALT(RTW_REGD_QATAR, RTW_REGD_ETSI),
	DECL_REGD_ALT(RTW_REGD_UK, RTW_REGD_ETSI),
};

static bool rtw_regd_has_alt(u8 regd, u8 *regd_alt)
{
	if (!rtw_regd_alt[regd].set)
		return false;

	*regd_alt = rtw_regd_alt[regd].alt;
	return true;
}

/* main.h struct rtw_regulatory and regd.c's country table, verbatim */
struct rtw_regulatory {
	char alpha2[2];
	u8 txpwr_regd_2g;
	u8 txpwr_regd_5g;
};

#define COUNTRY_REGD_ENT(_alpha2, _regd_2g, _regd_5g) \
	{.alpha2 = (_alpha2), \
	 .txpwr_regd_2g = (_regd_2g), \
	 .txpwr_regd_5g = (_regd_5g), \
	}

static const struct rtw_regulatory rtw_reg_map[] = {
	COUNTRY_REGD_ENT("AD", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AG", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("AI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AN", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("AO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AQ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AR", RTW_REGD_MEXICO, RTW_REGD_MEXICO),
	COUNTRY_REGD_ENT("AS", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("AT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("AU", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("AW", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("AZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BB", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("BD", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BJ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BM", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("BN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BO", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("BR", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("BS", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("BT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BV", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BW", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BY", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("BZ", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("CA", RTW_REGD_IC, RTW_REGD_IC),
	COUNTRY_REGD_ENT("CC", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("CD", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CL", RTW_REGD_CHILE, RTW_REGD_CHILE),
	COUNTRY_REGD_ENT("CM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CN", RTW_REGD_CN, RTW_REGD_CN),
	COUNTRY_REGD_ENT("CO", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("CR", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("CV", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CX", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("CY", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("CZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("DE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("DJ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("DK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("DM", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("DO", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("DZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("EC", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("EE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("EG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("EH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ER", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ES", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ET", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("FI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("FJ", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("FK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("FM", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("FO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("FR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GB", RTW_REGD_UK, RTW_REGD_UK),
	COUNTRY_REGD_ENT("GD", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("GE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GP", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GQ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GT", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("GU", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("GW", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("GY", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("HK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("HM", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("HN", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("HR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("HT", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("HU", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ID", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IQ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("IT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("JE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("JM", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("JO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("JP", RTW_REGD_MKK, RTW_REGD_MKK),
	COUNTRY_REGD_ENT("KE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KN", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("KR", RTW_REGD_KCC, RTW_REGD_KCC),
	COUNTRY_REGD_ENT("KW", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("KY", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("KZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LB", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LC", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("LI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LU", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LV", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("LY", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MC", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MD", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ME", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MF", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("MG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MH", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("MK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ML", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MP", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("MQ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MU", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MV", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MW", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MX", RTW_REGD_MEXICO, RTW_REGD_MEXICO),
	COUNTRY_REGD_ENT("MY", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("MZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NC", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NF", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("NG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NI", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("NL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NP", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("NU", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("NZ", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("OM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PA", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("PE", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("PF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PR", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("PS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("PW", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("PY", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("QA", RTW_REGD_QATAR, RTW_REGD_QATAR),
	COUNTRY_REGD_ENT("RE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("RO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("RS", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("RU", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("RW", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SB", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SC", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("SE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SI", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SJ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SL", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SR", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("ST", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("SV", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("SX", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("SZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TC", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TD", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TH", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TJ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TK", RTW_REGD_ACMA, RTW_REGD_ACMA),
	COUNTRY_REGD_ENT("TM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TO", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TR", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("TT", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("TV", RTW_REGD_ETSI, RTW_REGD_WW),
	COUNTRY_REGD_ENT("TW", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("TZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("UA", RTW_REGD_UKRAINE, RTW_REGD_UKRAINE),
	COUNTRY_REGD_ENT("UG", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("US", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("UY", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("UZ", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("VA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("VC", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("VE", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("VG", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("VI", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("VN", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("VU", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("WF", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("WS", RTW_REGD_FCC, RTW_REGD_FCC),
	COUNTRY_REGD_ENT("XK", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("YE", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("YT", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ZA", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ZM", RTW_REGD_ETSI, RTW_REGD_ETSI),
	COUNTRY_REGD_ENT("ZW", RTW_REGD_ETSI, RTW_REGD_ETSI),
};

/*
 * rtw_regd_get().  rtwb: the country is net80211's (ifconfig ... country
 * XX), looked up in the table above instead of cfg80211's regulatory hint.
 */
static u8 rtw_regd_get(struct rtw_dev *rtwdev)
{
	const char *cc = rtwdev->sc_ic.ic_regdomain.isocc;
	bool is_2g = rtwdev->hal.current_band_type == RTW_BAND_2G;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(rtw_reg_map); i++) {
		if (rtw_reg_map[i].alpha2[0] == cc[0] &&
		    rtw_reg_map[i].alpha2[1] == cc[1])
			return is_2g ? rtw_reg_map[i].txpwr_regd_2g :
			    rtw_reg_map[i].txpwr_regd_5g;
	}
	return RTW_REGD_WW;
}

static void rtw8822b_set_tx_power_index(struct rtw_dev *rtwdev);


const u8 rtw_cck_rates[] = { DESC_RATE1M, DESC_RATE2M, DESC_RATE5_5M, DESC_RATE11M };

const u8 rtw_ofdm_rates[] = {
	DESC_RATE6M,  DESC_RATE9M,  DESC_RATE12M,
	DESC_RATE18M, DESC_RATE24M, DESC_RATE36M,
	DESC_RATE48M, DESC_RATE54M
};

const u8 rtw_ht_1s_rates[] = {
	DESC_RATEMCS0, DESC_RATEMCS1, DESC_RATEMCS2,
	DESC_RATEMCS3, DESC_RATEMCS4, DESC_RATEMCS5,
	DESC_RATEMCS6, DESC_RATEMCS7
};

const u8 rtw_ht_2s_rates[] = {
	DESC_RATEMCS8,  DESC_RATEMCS9,  DESC_RATEMCS10,
	DESC_RATEMCS11, DESC_RATEMCS12, DESC_RATEMCS13,
	DESC_RATEMCS14, DESC_RATEMCS15
};

const u8 rtw_vht_1s_rates[] = {
	DESC_RATEVHT1SS_MCS0, DESC_RATEVHT1SS_MCS1,
	DESC_RATEVHT1SS_MCS2, DESC_RATEVHT1SS_MCS3,
	DESC_RATEVHT1SS_MCS4, DESC_RATEVHT1SS_MCS5,
	DESC_RATEVHT1SS_MCS6, DESC_RATEVHT1SS_MCS7,
	DESC_RATEVHT1SS_MCS8, DESC_RATEVHT1SS_MCS9
};

const u8 rtw_vht_2s_rates[] = {
	DESC_RATEVHT2SS_MCS0, DESC_RATEVHT2SS_MCS1,
	DESC_RATEVHT2SS_MCS2, DESC_RATEVHT2SS_MCS3,
	DESC_RATEVHT2SS_MCS4, DESC_RATEVHT2SS_MCS5,
	DESC_RATEVHT2SS_MCS6, DESC_RATEVHT2SS_MCS7,
	DESC_RATEVHT2SS_MCS8, DESC_RATEVHT2SS_MCS9
};

const u8 rtw_ht_3s_rates[] = {
	DESC_RATEMCS16, DESC_RATEMCS17, DESC_RATEMCS18,
	DESC_RATEMCS19, DESC_RATEMCS20, DESC_RATEMCS21,
	DESC_RATEMCS22, DESC_RATEMCS23
};

const u8 rtw_ht_4s_rates[] = {
	DESC_RATEMCS24, DESC_RATEMCS25, DESC_RATEMCS26,
	DESC_RATEMCS27, DESC_RATEMCS28, DESC_RATEMCS29,
	DESC_RATEMCS30, DESC_RATEMCS31
};

const u8 rtw_vht_3s_rates[] = {
	DESC_RATEVHT3SS_MCS0, DESC_RATEVHT3SS_MCS1,
	DESC_RATEVHT3SS_MCS2, DESC_RATEVHT3SS_MCS3,
	DESC_RATEVHT3SS_MCS4, DESC_RATEVHT3SS_MCS5,
	DESC_RATEVHT3SS_MCS6, DESC_RATEVHT3SS_MCS7,
	DESC_RATEVHT3SS_MCS8, DESC_RATEVHT3SS_MCS9
};

const u8 rtw_vht_4s_rates[] = {
	DESC_RATEVHT4SS_MCS0, DESC_RATEVHT4SS_MCS1,
	DESC_RATEVHT4SS_MCS2, DESC_RATEVHT4SS_MCS3,
	DESC_RATEVHT4SS_MCS4, DESC_RATEVHT4SS_MCS5,
	DESC_RATEVHT4SS_MCS6, DESC_RATEVHT4SS_MCS7,
	DESC_RATEVHT4SS_MCS8, DESC_RATEVHT4SS_MCS9
};

const u8 * const rtw_rate_section[RTW_RATE_SECTION_NUM] = {
	rtw_cck_rates, rtw_ofdm_rates,
	rtw_ht_1s_rates, rtw_ht_2s_rates,
	rtw_vht_1s_rates, rtw_vht_2s_rates,
	rtw_ht_3s_rates, rtw_ht_4s_rates,
	rtw_vht_3s_rates, rtw_vht_4s_rates
};

const u8 rtw_rate_size[RTW_RATE_SECTION_NUM] = {
	ARRAY_SIZE(rtw_cck_rates),
	ARRAY_SIZE(rtw_ofdm_rates),
	ARRAY_SIZE(rtw_ht_1s_rates),
	ARRAY_SIZE(rtw_ht_2s_rates),
	ARRAY_SIZE(rtw_vht_1s_rates),
	ARRAY_SIZE(rtw_vht_2s_rates),
	ARRAY_SIZE(rtw_ht_3s_rates),
	ARRAY_SIZE(rtw_ht_4s_rates),
	ARRAY_SIZE(rtw_vht_3s_rates),
	ARRAY_SIZE(rtw_vht_4s_rates)
};

static u8 tbl_to_dec_pwr_by_rate(struct rtw_dev *rtwdev, u32 hex, u8 i)
{
	/* rtwb: the 8822B has is_pwr_by_rate_dec set */
	return bcd_to_dec_pwr_by_rate(hex, i);
}

static void
rtw_phy_get_rate_values_of_txpwr_by_rate(struct rtw_dev *rtwdev,
					 u32 addr, u32 mask, u32 val, u8 *rate,
					 u8 *pwr_by_rate, u8 *rate_num)
{
	int i;

	switch (addr) {
	case 0xE00:
	case 0x830:
		rate[0] = DESC_RATE6M;
		rate[1] = DESC_RATE9M;
		rate[2] = DESC_RATE12M;
		rate[3] = DESC_RATE18M;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xE04:
	case 0x834:
		rate[0] = DESC_RATE24M;
		rate[1] = DESC_RATE36M;
		rate[2] = DESC_RATE48M;
		rate[3] = DESC_RATE54M;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xE08:
		rate[0] = DESC_RATE1M;
		pwr_by_rate[0] = bcd_to_dec_pwr_by_rate(val, 1);
		*rate_num = 1;
		break;
	case 0x86C:
		if (mask == 0xffffff00) {
			rate[0] = DESC_RATE2M;
			rate[1] = DESC_RATE5_5M;
			rate[2] = DESC_RATE11M;
			for (i = 1; i < 4; ++i)
				pwr_by_rate[i - 1] =
					tbl_to_dec_pwr_by_rate(rtwdev, val, i);
			*rate_num = 3;
		} else if (mask == 0x000000ff) {
			rate[0] = DESC_RATE11M;
			pwr_by_rate[0] = bcd_to_dec_pwr_by_rate(val, 0);
			*rate_num = 1;
		}
		break;
	case 0xE10:
	case 0x83C:
		rate[0] = DESC_RATEMCS0;
		rate[1] = DESC_RATEMCS1;
		rate[2] = DESC_RATEMCS2;
		rate[3] = DESC_RATEMCS3;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xE14:
	case 0x848:
		rate[0] = DESC_RATEMCS4;
		rate[1] = DESC_RATEMCS5;
		rate[2] = DESC_RATEMCS6;
		rate[3] = DESC_RATEMCS7;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xE18:
	case 0x84C:
		rate[0] = DESC_RATEMCS8;
		rate[1] = DESC_RATEMCS9;
		rate[2] = DESC_RATEMCS10;
		rate[3] = DESC_RATEMCS11;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xE1C:
	case 0x868:
		rate[0] = DESC_RATEMCS12;
		rate[1] = DESC_RATEMCS13;
		rate[2] = DESC_RATEMCS14;
		rate[3] = DESC_RATEMCS15;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0x838:
		rate[0] = DESC_RATE1M;
		rate[1] = DESC_RATE2M;
		rate[2] = DESC_RATE5_5M;
		for (i = 1; i < 4; ++i)
			pwr_by_rate[i - 1] = tbl_to_dec_pwr_by_rate(rtwdev,
								    val, i);
		*rate_num = 3;
		break;
	case 0xC20:
	case 0xE20:
	case 0x1820:
	case 0x1A20:
		rate[0] = DESC_RATE1M;
		rate[1] = DESC_RATE2M;
		rate[2] = DESC_RATE5_5M;
		rate[3] = DESC_RATE11M;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC24:
	case 0xE24:
	case 0x1824:
	case 0x1A24:
		rate[0] = DESC_RATE6M;
		rate[1] = DESC_RATE9M;
		rate[2] = DESC_RATE12M;
		rate[3] = DESC_RATE18M;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC28:
	case 0xE28:
	case 0x1828:
	case 0x1A28:
		rate[0] = DESC_RATE24M;
		rate[1] = DESC_RATE36M;
		rate[2] = DESC_RATE48M;
		rate[3] = DESC_RATE54M;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC2C:
	case 0xE2C:
	case 0x182C:
	case 0x1A2C:
		rate[0] = DESC_RATEMCS0;
		rate[1] = DESC_RATEMCS1;
		rate[2] = DESC_RATEMCS2;
		rate[3] = DESC_RATEMCS3;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC30:
	case 0xE30:
	case 0x1830:
	case 0x1A30:
		rate[0] = DESC_RATEMCS4;
		rate[1] = DESC_RATEMCS5;
		rate[2] = DESC_RATEMCS6;
		rate[3] = DESC_RATEMCS7;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC34:
	case 0xE34:
	case 0x1834:
	case 0x1A34:
		rate[0] = DESC_RATEMCS8;
		rate[1] = DESC_RATEMCS9;
		rate[2] = DESC_RATEMCS10;
		rate[3] = DESC_RATEMCS11;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC38:
	case 0xE38:
	case 0x1838:
	case 0x1A38:
		rate[0] = DESC_RATEMCS12;
		rate[1] = DESC_RATEMCS13;
		rate[2] = DESC_RATEMCS14;
		rate[3] = DESC_RATEMCS15;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC3C:
	case 0xE3C:
	case 0x183C:
	case 0x1A3C:
		rate[0] = DESC_RATEVHT1SS_MCS0;
		rate[1] = DESC_RATEVHT1SS_MCS1;
		rate[2] = DESC_RATEVHT1SS_MCS2;
		rate[3] = DESC_RATEVHT1SS_MCS3;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC40:
	case 0xE40:
	case 0x1840:
	case 0x1A40:
		rate[0] = DESC_RATEVHT1SS_MCS4;
		rate[1] = DESC_RATEVHT1SS_MCS5;
		rate[2] = DESC_RATEVHT1SS_MCS6;
		rate[3] = DESC_RATEVHT1SS_MCS7;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC44:
	case 0xE44:
	case 0x1844:
	case 0x1A44:
		rate[0] = DESC_RATEVHT1SS_MCS8;
		rate[1] = DESC_RATEVHT1SS_MCS9;
		rate[2] = DESC_RATEVHT2SS_MCS0;
		rate[3] = DESC_RATEVHT2SS_MCS1;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC48:
	case 0xE48:
	case 0x1848:
	case 0x1A48:
		rate[0] = DESC_RATEVHT2SS_MCS2;
		rate[1] = DESC_RATEVHT2SS_MCS3;
		rate[2] = DESC_RATEVHT2SS_MCS4;
		rate[3] = DESC_RATEVHT2SS_MCS5;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xC4C:
	case 0xE4C:
	case 0x184C:
	case 0x1A4C:
		rate[0] = DESC_RATEVHT2SS_MCS6;
		rate[1] = DESC_RATEVHT2SS_MCS7;
		rate[2] = DESC_RATEVHT2SS_MCS8;
		rate[3] = DESC_RATEVHT2SS_MCS9;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xCD8:
	case 0xED8:
	case 0x18D8:
	case 0x1AD8:
		rate[0] = DESC_RATEMCS16;
		rate[1] = DESC_RATEMCS17;
		rate[2] = DESC_RATEMCS18;
		rate[3] = DESC_RATEMCS19;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xCDC:
	case 0xEDC:
	case 0x18DC:
	case 0x1ADC:
		rate[0] = DESC_RATEMCS20;
		rate[1] = DESC_RATEMCS21;
		rate[2] = DESC_RATEMCS22;
		rate[3] = DESC_RATEMCS23;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xCE0:
	case 0xEE0:
	case 0x18E0:
	case 0x1AE0:
		rate[0] = DESC_RATEVHT3SS_MCS0;
		rate[1] = DESC_RATEVHT3SS_MCS1;
		rate[2] = DESC_RATEVHT3SS_MCS2;
		rate[3] = DESC_RATEVHT3SS_MCS3;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xCE4:
	case 0xEE4:
	case 0x18E4:
	case 0x1AE4:
		rate[0] = DESC_RATEVHT3SS_MCS4;
		rate[1] = DESC_RATEVHT3SS_MCS5;
		rate[2] = DESC_RATEVHT3SS_MCS6;
		rate[3] = DESC_RATEVHT3SS_MCS7;
		for (i = 0; i < 4; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 4;
		break;
	case 0xCE8:
	case 0xEE8:
	case 0x18E8:
	case 0x1AE8:
		rate[0] = DESC_RATEVHT3SS_MCS8;
		rate[1] = DESC_RATEVHT3SS_MCS9;
		for (i = 0; i < 2; ++i)
			pwr_by_rate[i] = tbl_to_dec_pwr_by_rate(rtwdev, val, i);
		*rate_num = 2;
		break;
	default:
		rtw_warn(rtwdev, "invalid tx power index addr 0x%08x\n", addr);
		break;
	}
}

static void rtw_phy_store_tx_power_by_rate(struct rtw_dev *rtwdev,
					   u32 band, u32 rfpath, u32 txnum,
					   u32 regaddr, u32 bitmask, u32 data)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 rate_num = 0;
	u8 rate;
	u8 rates[RTW_RF_PATH_MAX] = {0};
	s8 offset;
	s8 pwr_by_rate[RTW_RF_PATH_MAX] = {0};
	int i;

	rtw_phy_get_rate_values_of_txpwr_by_rate(rtwdev, regaddr, bitmask, data,
						 rates, pwr_by_rate, &rate_num);

	if (WARN_ON(rfpath >= RTW_RF_PATH_MAX ||
		    (band != PHY_BAND_2G && band != PHY_BAND_5G) ||
		    rate_num > RTW_RF_PATH_MAX))
		return;

	for (i = 0; i < rate_num; i++) {
		offset = pwr_by_rate[i];
		rate = rates[i];
		if (band == PHY_BAND_2G)
			hal->tx_pwr_by_rate_offset_2g[rfpath][rate] = offset;
		else
			hal->tx_pwr_by_rate_offset_5g[rfpath][rate] = offset;
	}
}

void rtw_parse_tbl_bb_pg(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{
	const struct rtw_phy_pg_cfg_pair *p = tbl->data;
	const struct rtw_phy_pg_cfg_pair *end = p + tbl->size;

	for (; p < end; p++) {
		if (p->addr == 0xfe || p->addr == 0xffe) {
			msleep(50);
			continue;
		}
		rtw_phy_store_tx_power_by_rate(rtwdev, p->band, p->rf_path,
					       p->tx_num, p->addr, p->bitmask,
					       p->data);
	}
}

static const u8 rtw_channel_idx_5g[RTW_MAX_CHANNEL_NUM_5G] = {
	36,  38,  40,  42,  44,  46,  48, /* Band 1 */
	52,  54,  56,  58,  60,  62,  64, /* Band 2 */
	100, 102, 104, 106, 108, 110, 112, /* Band 3 */
	116, 118, 120, 122, 124, 126, 128, /* Band 3 */
	132, 134, 136, 138, 140, 142, 144, /* Band 3 */
	149, 151, 153, 155, 157, 159, 161, /* Band 4 */
	165, 167, 169, 171, 173, 175, 177}; /* Band 4 */

static int rtw_channel_to_idx(u8 band, u8 channel)
{
	int ch_idx;
	u8 n_channel;

	if (band == PHY_BAND_2G) {
		ch_idx = channel - 1;
		n_channel = RTW_MAX_CHANNEL_NUM_2G;
	} else if (band == PHY_BAND_5G) {
		n_channel = RTW_MAX_CHANNEL_NUM_5G;
		for (ch_idx = 0; ch_idx < n_channel; ch_idx++)
			if (rtw_channel_idx_5g[ch_idx] == channel)
				break;
	} else {
		return -1;
	}

	if (ch_idx >= n_channel)
		return -1;

	return ch_idx;
}

static void rtw_phy_set_tx_power_limit(struct rtw_dev *rtwdev, u8 regd, u8 band,
				       u8 bw, u8 rs, u8 ch, s8 pwr_limit)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 max_power_index = RTW8822B_MAX_POWER_INDEX;
	s8 ww;
	int ch_idx;

	pwr_limit = clamp_t(s8, pwr_limit,
			    -max_power_index, max_power_index);
	ch_idx = rtw_channel_to_idx(band, ch);

	if (regd >= RTW_REGD_MAX || bw >= RTW_CHANNEL_WIDTH_MAX ||
	    rs >= RTW_RATE_SECTION_NUM || ch_idx < 0) {
		WARN(1,
		     "wrong txpwr_lmt regd=%u, band=%u bw=%u, rs=%u, ch_idx=%u, pwr_limit=%d\n",
		     regd, band, bw, rs, ch_idx, pwr_limit);
		return;
	}

	if (band == PHY_BAND_2G) {
		hal->tx_pwr_limit_2g[regd][bw][rs][ch_idx] = pwr_limit;
		ww = hal->tx_pwr_limit_2g[RTW_REGD_WW][bw][rs][ch_idx];
		ww = min_t(s8, ww, pwr_limit);
		hal->tx_pwr_limit_2g[RTW_REGD_WW][bw][rs][ch_idx] = ww;
	} else if (band == PHY_BAND_5G) {
		hal->tx_pwr_limit_5g[regd][bw][rs][ch_idx] = pwr_limit;
		ww = hal->tx_pwr_limit_5g[RTW_REGD_WW][bw][rs][ch_idx];
		ww = min_t(s8, ww, pwr_limit);
		hal->tx_pwr_limit_5g[RTW_REGD_WW][bw][rs][ch_idx] = ww;
	}
}

/* cross-reference 5G power limits if values are not assigned */
static void
rtw_xref_5g_txpwr_lmt(struct rtw_dev *rtwdev, u8 regd,
		      u8 bw, u8 ch_idx, u8 rs_ht, u8 rs_vht)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 max_power_index = RTW8822B_MAX_POWER_INDEX;
	s8 lmt_ht = hal->tx_pwr_limit_5g[regd][bw][rs_ht][ch_idx];
	s8 lmt_vht = hal->tx_pwr_limit_5g[regd][bw][rs_vht][ch_idx];

	if (lmt_ht == lmt_vht)
		return;

	if (lmt_ht == max_power_index)
		hal->tx_pwr_limit_5g[regd][bw][rs_ht][ch_idx] = lmt_vht;

	else if (lmt_vht == max_power_index)
		hal->tx_pwr_limit_5g[regd][bw][rs_vht][ch_idx] = lmt_ht;
}

/* cross-reference power limits for ht and vht */
static void
rtw_xref_txpwr_lmt_by_rs(struct rtw_dev *rtwdev, u8 regd, u8 bw, u8 ch_idx)
{
	static const u8 rs_cmp[4][2] = {
		{RTW_RATE_SECTION_HT_1S, RTW_RATE_SECTION_VHT_1S},
		{RTW_RATE_SECTION_HT_2S, RTW_RATE_SECTION_VHT_2S},
		{RTW_RATE_SECTION_HT_3S, RTW_RATE_SECTION_VHT_3S},
		{RTW_RATE_SECTION_HT_4S, RTW_RATE_SECTION_VHT_4S}
	};
	u8 rs_idx, rs_ht, rs_vht;

	for (rs_idx = 0; rs_idx < 4; rs_idx++) {
		rs_ht = rs_cmp[rs_idx][0];
		rs_vht = rs_cmp[rs_idx][1];

		rtw_xref_5g_txpwr_lmt(rtwdev, regd, bw, ch_idx, rs_ht, rs_vht);
	}
}

/* cross-reference power limits for 5G channels */
static void
rtw_xref_5g_txpwr_lmt_by_ch(struct rtw_dev *rtwdev, u8 regd, u8 bw)
{
	u8 ch_idx;

	for (ch_idx = 0; ch_idx < RTW_MAX_CHANNEL_NUM_5G; ch_idx++)
		rtw_xref_txpwr_lmt_by_rs(rtwdev, regd, bw, ch_idx);
}

/* cross-reference power limits for 20/40M bandwidth */
static void
rtw_xref_txpwr_lmt_by_bw(struct rtw_dev *rtwdev, u8 regd)
{
	u8 bw;

	for (bw = RTW_CHANNEL_WIDTH_20; bw <= RTW_CHANNEL_WIDTH_40; bw++)
		rtw_xref_5g_txpwr_lmt_by_ch(rtwdev, regd, bw);
}

/* cross-reference power limits */
static void rtw_xref_txpwr_lmt(struct rtw_dev *rtwdev)
{
	u8 regd;

	for (regd = 0; regd < RTW_REGD_MAX; regd++)
		rtw_xref_txpwr_lmt_by_bw(rtwdev, regd);
}

static void
__cfg_txpwr_lmt_by_alt(struct rtw_hal *hal, u8 regd, u8 regd_alt, u8 bw, u8 rs)
{
	u8 ch;

	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_2G; ch++)
		hal->tx_pwr_limit_2g[regd][bw][rs][ch] =
			hal->tx_pwr_limit_2g[regd_alt][bw][rs][ch];

	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_5G; ch++)
		hal->tx_pwr_limit_5g[regd][bw][rs][ch] =
			hal->tx_pwr_limit_5g[regd_alt][bw][rs][ch];
}

static void
rtw_cfg_txpwr_lmt_by_alt(struct rtw_dev *rtwdev, u8 regd, u8 regd_alt)
{
	u8 bw, rs;

	for (bw = 0; bw < RTW_CHANNEL_WIDTH_MAX; bw++)
		for (rs = 0; rs < RTW_RATE_SECTION_NUM; rs++)
			__cfg_txpwr_lmt_by_alt(&rtwdev->hal, regd, regd_alt,
					       bw, rs);
}

void rtw_parse_tbl_txpwr_lmt(struct rtw_dev *rtwdev,
			     const struct rtw_table *tbl)
{
	const struct rtw_txpwr_lmt_cfg_pair *p = tbl->data;
	const struct rtw_txpwr_lmt_cfg_pair *end = p + tbl->size;
	u32 regd_cfg_flag = 0;
	u8 regd_alt;
	u8 i;

	for (; p < end; p++) {
		regd_cfg_flag |= BIT(p->regd);
		rtw_phy_set_tx_power_limit(rtwdev, p->regd, p->band,
					   p->bw, p->rs, p->ch, p->txpwr_lmt);
	}

	for (i = 0; i < RTW_REGD_MAX; i++) {
		if (i == RTW_REGD_WW)
			continue;

		if (regd_cfg_flag & BIT(i))
			continue;

		rtw_dbg(rtwdev, RTW_DBG_REGD,
			"txpwr regd %d does not be configured\n", i);

		if (rtw_regd_has_alt(i, &regd_alt) &&
		    regd_cfg_flag & BIT(regd_alt)) {
			rtw_dbg(rtwdev, RTW_DBG_REGD,
				"cfg txpwr regd %d by regd %d as alternative\n",
				i, regd_alt);

			rtw_cfg_txpwr_lmt_by_alt(rtwdev, i, regd_alt);
			continue;
		}

		rtw_dbg(rtwdev, RTW_DBG_REGD, "cfg txpwr regd %d by WW\n", i);
		rtw_cfg_txpwr_lmt_by_alt(rtwdev, i, RTW_REGD_WW);
	}

	rtw_xref_txpwr_lmt(rtwdev);
}

static u8 rtw_get_channel_group(u8 channel, u8 rate)
{
	switch (channel) {
	default:
		WARN_ON(1);
		fallthrough;
	case 1:
	case 2:
	case 36:
	case 38:
	case 40:
	case 42:
		return 0;
	case 3:
	case 4:
	case 5:
	case 44:
	case 46:
	case 48:
	case 50:
		return 1;
	case 6:
	case 7:
	case 8:
	case 52:
	case 54:
	case 56:
	case 58:
		return 2;
	case 9:
	case 10:
	case 11:
	case 60:
	case 62:
	case 64:
		return 3;
	case 12:
	case 13:
	case 100:
	case 102:
	case 104:
	case 106:
		return 4;
	case 14:
		return rate <= DESC_RATE11M ? 5 : 4;
	case 108:
	case 110:
	case 112:
	case 114:
		return 5;
	case 116:
	case 118:
	case 120:
	case 122:
		return 6;
	case 124:
	case 126:
	case 128:
	case 130:
		return 7;
	case 132:
	case 134:
	case 136:
	case 138:
		return 8;
	case 140:
	case 142:
	case 144:
		return 9;
	case 149:
	case 151:
	case 153:
	case 155:
		return 10;
	case 157:
	case 159:
	case 161:
		return 11;
	case 165:
	case 167:
	case 169:
	case 171:
		return 12;
	case 173:
	case 175:
	case 177:
		return 13;
	}
}


static u8 rtw_phy_get_2g_tx_power_index(struct rtw_dev *rtwdev,
					struct rtw_2g_txpwr_idx *pwr_idx_2g,
					u8 bandwidth,
					u8 rate, u8 group)
{
	bool above_2ss, above_3ss, above_4ss;
	u8 factor = 1;	/* rtwb: txgi_factor of the 8822B */
	bool mcs_rate;
	u8 tx_power;

	if (rate <= DESC_RATE11M)
		tx_power = pwr_idx_2g->cck_base[group];
	else
		tx_power = pwr_idx_2g->bw40_base[group];

	if (rate >= DESC_RATE6M && rate <= DESC_RATE54M)
		tx_power += pwr_idx_2g->ht_1s_diff.ofdm * factor;

	mcs_rate = (rate >= DESC_RATEMCS0 && rate <= DESC_RATEMCS31) ||
		   (rate >= DESC_RATEVHT1SS_MCS0 &&
		    rate <= DESC_RATEVHT4SS_MCS9);
	above_2ss = (rate >= DESC_RATEMCS8 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT2SS_MCS0);
	above_3ss = (rate >= DESC_RATEMCS16 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT3SS_MCS0);
	above_4ss = (rate >= DESC_RATEMCS24 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT4SS_MCS0);

	if (!mcs_rate)
		return tx_power;

	switch (bandwidth) {
	default:
		WARN_ON(1);
		fallthrough;
	case RTW_CHANNEL_WIDTH_20:
		tx_power += pwr_idx_2g->ht_1s_diff.bw20 * factor;
		if (above_2ss)
			tx_power += pwr_idx_2g->ht_2s_diff.bw20 * factor;
		if (above_3ss)
			tx_power += pwr_idx_2g->ht_3s_diff.bw20 * factor;
		if (above_4ss)
			tx_power += pwr_idx_2g->ht_4s_diff.bw20 * factor;
		break;
	case RTW_CHANNEL_WIDTH_40:
		/* bw40 is the base power */
		if (above_2ss)
			tx_power += pwr_idx_2g->ht_2s_diff.bw40 * factor;
		if (above_3ss)
			tx_power += pwr_idx_2g->ht_3s_diff.bw40 * factor;
		if (above_4ss)
			tx_power += pwr_idx_2g->ht_4s_diff.bw40 * factor;
		break;
	}

	return tx_power;
}

static u8 rtw_phy_get_5g_tx_power_index(struct rtw_dev *rtwdev,
					struct rtw_5g_txpwr_idx *pwr_idx_5g,
					u8 bandwidth,
					u8 rate, u8 group)
{
	bool above_2ss, above_3ss, above_4ss;
	u8 factor = 1;	/* rtwb: txgi_factor of the 8822B */
	u8 upper, lower;
	bool mcs_rate;
	u8 tx_power;

	tx_power = pwr_idx_5g->bw40_base[group];

	mcs_rate = (rate >= DESC_RATEMCS0 && rate <= DESC_RATEMCS31) ||
		   (rate >= DESC_RATEVHT1SS_MCS0 &&
		    rate <= DESC_RATEVHT4SS_MCS9);
	above_2ss = (rate >= DESC_RATEMCS8 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT2SS_MCS0);
	above_3ss = (rate >= DESC_RATEMCS16 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT3SS_MCS0);
	above_4ss = (rate >= DESC_RATEMCS24 && rate <= DESC_RATEMCS31) ||
		    (rate >= DESC_RATEVHT4SS_MCS0);

	if (!mcs_rate) {
		tx_power += pwr_idx_5g->ht_1s_diff.ofdm * factor;
		return tx_power;
	}

	switch (bandwidth) {
	default:
		WARN_ON(1);
		fallthrough;
	case RTW_CHANNEL_WIDTH_20:
		tx_power += pwr_idx_5g->ht_1s_diff.bw20 * factor;
		if (above_2ss)
			tx_power += pwr_idx_5g->ht_2s_diff.bw20 * factor;
		if (above_3ss)
			tx_power += pwr_idx_5g->ht_3s_diff.bw20 * factor;
		if (above_4ss)
			tx_power += pwr_idx_5g->ht_4s_diff.bw20 * factor;
		break;
	case RTW_CHANNEL_WIDTH_40:
		/* bw40 is the base power */
		if (above_2ss)
			tx_power += pwr_idx_5g->ht_2s_diff.bw40 * factor;
		if (above_3ss)
			tx_power += pwr_idx_5g->ht_3s_diff.bw40 * factor;
		if (above_4ss)
			tx_power += pwr_idx_5g->ht_4s_diff.bw40 * factor;
		break;
	case RTW_CHANNEL_WIDTH_80:
		/* the base idx of bw80 is the average of bw40+/bw40- */
		lower = pwr_idx_5g->bw40_base[group];
		upper = pwr_idx_5g->bw40_base[group + 1];

		tx_power = (lower + upper) / 2;
		tx_power += pwr_idx_5g->vht_1s_diff.bw80 * factor;
		if (above_2ss)
			tx_power += pwr_idx_5g->vht_2s_diff.bw80 * factor;
		if (above_3ss)
			tx_power += pwr_idx_5g->vht_3s_diff.bw80 * factor;
		if (above_4ss)
			tx_power += pwr_idx_5g->vht_4s_diff.bw80 * factor;
		break;
	}

	return tx_power;
}

/* return RTW_RATE_SECTION_NUM to indicate rate is invalid */
static u8 rtw_phy_rate_to_rate_section(u8 rate)
{
	if (rate >= DESC_RATE1M && rate <= DESC_RATE11M)
		return RTW_RATE_SECTION_CCK;
	else if (rate >= DESC_RATE6M && rate <= DESC_RATE54M)
		return RTW_RATE_SECTION_OFDM;
	else if (rate >= DESC_RATEMCS0 && rate <= DESC_RATEMCS7)
		return RTW_RATE_SECTION_HT_1S;
	else if (rate >= DESC_RATEMCS8 && rate <= DESC_RATEMCS15)
		return RTW_RATE_SECTION_HT_2S;
	else if (rate >= DESC_RATEMCS16 && rate <= DESC_RATEMCS23)
		return RTW_RATE_SECTION_HT_3S;
	else if (rate >= DESC_RATEMCS24 && rate <= DESC_RATEMCS31)
		return RTW_RATE_SECTION_HT_4S;
	else if (rate >= DESC_RATEVHT1SS_MCS0 && rate <= DESC_RATEVHT1SS_MCS9)
		return RTW_RATE_SECTION_VHT_1S;
	else if (rate >= DESC_RATEVHT2SS_MCS0 && rate <= DESC_RATEVHT2SS_MCS9)
		return RTW_RATE_SECTION_VHT_2S;
	else if (rate >= DESC_RATEVHT3SS_MCS0 && rate <= DESC_RATEVHT3SS_MCS9)
		return RTW_RATE_SECTION_VHT_3S;
	else if (rate >= DESC_RATEVHT4SS_MCS0 && rate <= DESC_RATEVHT4SS_MCS9)
		return RTW_RATE_SECTION_VHT_4S;
	else
		return RTW_RATE_SECTION_NUM;
}

static s8 rtw_phy_get_tx_power_limit(struct rtw_dev *rtwdev, u8 band,
				     u8 bw, u8 rf_path,
				     u8 rate, u8 channel, u8 regd)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 *cch_by_bw = hal->cch_by_bw;
	s8 power_limit = (s8)RTW8822B_MAX_POWER_INDEX;
	u8 rs = rtw_phy_rate_to_rate_section(rate);
	int ch_idx;
	u8 cur_bw, cur_ch;
	s8 cur_lmt;

	if (regd > RTW_REGD_WW)
		return power_limit;

	if (rs == RTW_RATE_SECTION_NUM)
		goto err;

	/* only 20M BW with cck and ofdm */
	if (rs == RTW_RATE_SECTION_CCK || rs == RTW_RATE_SECTION_OFDM)
		bw = RTW_CHANNEL_WIDTH_20;

	/* only 20/40M BW with ht */
	if (rate >= DESC_RATEMCS0 && rate <= DESC_RATEMCS31)
		bw = min_t(u8, bw, RTW_CHANNEL_WIDTH_40);

	/* select min power limit among [20M BW ~ current BW] */
	for (cur_bw = RTW_CHANNEL_WIDTH_20; cur_bw <= bw; cur_bw++) {
		cur_ch = cch_by_bw[cur_bw];

		ch_idx = rtw_channel_to_idx(band, cur_ch);
		if (ch_idx < 0)
			goto err;

		cur_lmt = cur_ch <= RTW_MAX_CHANNEL_NUM_2G ?
			hal->tx_pwr_limit_2g[regd][cur_bw][rs][ch_idx] :
			hal->tx_pwr_limit_5g[regd][cur_bw][rs][ch_idx];

		power_limit = min_t(s8, cur_lmt, power_limit);
	}

	return power_limit;

err:
	WARN(1, "invalid arguments, band=%d, bw=%d, path=%d, rate=%d, ch=%d\n",
	     band, bw, rf_path, rate, channel);
	return (s8)RTW8822B_MAX_POWER_INDEX;
}

static void rtw_get_tx_power_params(struct rtw_dev *rtwdev, u8 path, u8 rate, u8 bw,
			     u8 ch, u8 regd, struct rtw_power_params *pwr_param)
{
	struct rtw_hal *hal = &rtwdev->hal;
	struct rtw_txpwr_idx *pwr_idx;
	u8 group, band;
	u8 *base = &pwr_param->pwr_base;
	s8 *offset = &pwr_param->pwr_offset;
	s8 *limit = &pwr_param->pwr_limit;
	s8 *remnant = &pwr_param->pwr_remnant;
	s8 *sar = &pwr_param->pwr_sar;

	pwr_idx = &rtwdev->efuse.txpwr_idx_table[path];
	group = rtw_get_channel_group(ch, rate);

	/* base power index for 2.4G/5G */
	if (IS_CH_2G_BAND(ch)) {
		band = PHY_BAND_2G;
		*base = rtw_phy_get_2g_tx_power_index(rtwdev,
						      &pwr_idx->pwr_idx_2g,
						      bw, rate, group);
		*offset = hal->tx_pwr_by_rate_offset_2g[path][rate];
	} else {
		band = PHY_BAND_5G;
		*base = rtw_phy_get_5g_tx_power_index(rtwdev,
						      &pwr_idx->pwr_idx_5g,
						      bw, rate, group);
		*offset = hal->tx_pwr_by_rate_offset_5g[path][rate];
	}

	*limit = rtw_phy_get_tx_power_limit(rtwdev, band, bw, path,
					    rate, ch, regd);
	/* rtwb: no power tracking (remnant) and no SAR limits yet */
	*remnant = 0;
	*sar = (s8)RTW8822B_MAX_POWER_INDEX;
}

static u8
rtw_phy_get_tx_power_index(struct rtw_dev *rtwdev, u8 rf_path, u8 rate,
			   u8 bandwidth, u8 channel, u8 regd)
{
	struct rtw_power_params pwr_param = {0};
	u8 tx_power;
	s8 offset;

	rtw_get_tx_power_params(rtwdev, rf_path, rate, bandwidth,
				channel, regd, &pwr_param);

	tx_power = pwr_param.pwr_base;
	offset = min3(pwr_param.pwr_offset,
		      pwr_param.pwr_limit,
		      pwr_param.pwr_sar);

	/* rtwb: en_dis_dpd is not set for the 8822B */
	tx_power += offset + pwr_param.pwr_remnant;

	if (tx_power > RTW8822B_MAX_POWER_INDEX)
		tx_power = RTW8822B_MAX_POWER_INDEX;

	return tx_power;
}

static void rtw_phy_set_tx_power_index_by_rs(struct rtw_dev *rtwdev,
					     u8 ch, u8 path, u8 rs)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 regd = rtw_regd_get(rtwdev);
	const u8 *rates;
	u8 size;
	u8 rate;
	u8 pwr_idx;
	u8 bw;
	int i;

	if (rs >= RTW_RATE_SECTION_NUM)
		return;

	rates = rtw_rate_section[rs];
	size = rtw_rate_size[rs];
	bw = hal->current_band_width;
	for (i = 0; i < size; i++) {
		rate = rates[i];
		pwr_idx = rtw_phy_get_tx_power_index(rtwdev, path, rate,
						     bw, ch, regd);
		hal->tx_pwr_tbl[path][rate] = pwr_idx;
	}
}

/* set tx power level by path for each rates, note that the order of the rates
 * are *very* important, bacause 8822B/8821C combines every four bytes of tx
 * power index into a four-byte power index register, and calls set_tx_agc to
 * write these values into hardware
 */
static void rtw_phy_set_tx_power_level_by_path(struct rtw_dev *rtwdev,
					       u8 ch, u8 path)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 rs;

	/* do not need cck rates if we are not in 2.4G */
	if (hal->current_band_type == RTW_BAND_2G)
		rs = RTW_RATE_SECTION_CCK;
	else
		rs = RTW_RATE_SECTION_OFDM;

	for (; rs < RTW_RATE_SECTION_NUM; rs++)
		rtw_phy_set_tx_power_index_by_rs(rtwdev, ch, path, rs);
}

void rtw_phy_set_tx_power_level(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 path;

	for (path = 0; path < hal->rf_path_num; path++)
		rtw_phy_set_tx_power_level_by_path(rtwdev, channel, path);

	rtw8822b_set_tx_power_index(rtwdev);
}

static void
rtw_phy_tx_power_by_rate_config_by_path(struct rtw_hal *hal, u8 path,
					u8 rs, u8 size, const u8 *rates)
{
	u8 rate;
	u8 base_idx, rate_idx;
	s8 base_2g, base_5g;

	if (size == 10) /* VHT rates */
		base_idx = rates[size - 3];
	else
		base_idx = rates[size - 1];
	base_2g = hal->tx_pwr_by_rate_offset_2g[path][base_idx];
	base_5g = hal->tx_pwr_by_rate_offset_5g[path][base_idx];
	hal->tx_pwr_by_rate_base_2g[path][rs] = base_2g;
	hal->tx_pwr_by_rate_base_5g[path][rs] = base_5g;
	for (rate = 0; rate < size; rate++) {
		rate_idx = rates[rate];
		hal->tx_pwr_by_rate_offset_2g[path][rate_idx] -= base_2g;
		hal->tx_pwr_by_rate_offset_5g[path][rate_idx] -= base_5g;
	}
}

static void rtw_phy_tx_power_by_rate_config(struct rtw_hal *hal)
{
	u8 path, rs;

	for (path = 0; path < RTW_RF_PATH_MAX; path++)
		for (rs = 0; rs < RTW_RATE_SECTION_NUM; rs++)
			rtw_phy_tx_power_by_rate_config_by_path(hal, path, rs,
				rtw_rate_size[rs], rtw_rate_section[rs]);
}

static void
__rtw_phy_tx_power_limit_config(struct rtw_hal *hal, u8 regd, u8 bw, u8 rs)
{
	s8 base;
	u8 ch;

	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_2G; ch++) {
		base = hal->tx_pwr_by_rate_base_2g[0][rs];
		hal->tx_pwr_limit_2g[regd][bw][rs][ch] -= base;
	}

	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_5G; ch++) {
		base = hal->tx_pwr_by_rate_base_5g[0][rs];
		hal->tx_pwr_limit_5g[regd][bw][rs][ch] -= base;
	}
}

static void rtw_phy_tx_power_limit_config(struct rtw_hal *hal)
{
	u8 regd, bw, rs;

	/* default at channel 1 */
	hal->cch_by_bw[RTW_CHANNEL_WIDTH_20] = 1;

	for (regd = 0; regd < RTW_REGD_MAX; regd++)
		for (bw = 0; bw < RTW_CHANNEL_WIDTH_MAX; bw++)
			for (rs = 0; rs < RTW_RATE_SECTION_NUM; rs++)
				__rtw_phy_tx_power_limit_config(hal, regd, bw, rs);
}

static void rtw_phy_init_tx_power_limit(struct rtw_dev *rtwdev,
					u8 regd, u8 bw, u8 rs)
{
	struct rtw_hal *hal = &rtwdev->hal;
	s8 max_power_index = (s8)RTW8822B_MAX_POWER_INDEX;
	u8 ch;

	/* 2.4G channels */
	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_2G; ch++)
		hal->tx_pwr_limit_2g[regd][bw][rs][ch] = max_power_index;

	/* 5G channels */
	for (ch = 0; ch < RTW_MAX_CHANNEL_NUM_5G; ch++)
		hal->tx_pwr_limit_5g[regd][bw][rs][ch] = max_power_index;
}

static void rtw_phy_init_tx_power(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 regd, path, rate, rs, bw;

	/* init tx power by rate offset */
	for (path = 0; path < RTW_RF_PATH_MAX; path++) {
		for (rate = 0; rate < DESC_RATE_MAX; rate++) {
			hal->tx_pwr_by_rate_offset_2g[path][rate] = 0;
			hal->tx_pwr_by_rate_offset_5g[path][rate] = 0;
		}
	}

	/* init tx power limit */
	for (regd = 0; regd < RTW_REGD_MAX; regd++)
		for (bw = 0; bw < RTW_CHANNEL_WIDTH_MAX; bw++)
			for (rs = 0; rs < RTW_RATE_SECTION_NUM; rs++)
				rtw_phy_init_tx_power_limit(rtwdev, regd, bw,
							    rs);
}


static void
rtw8822b_set_tx_power_index_by_rate(struct rtw_dev *rtwdev, u8 path,
				    u8 rs, u32 *phy_pwr_idx)
{
	struct rtw_hal *hal = &rtwdev->hal;
	static const u32 offset_txagc[2] = {0x1d00, 0x1d80};
	u8 rate, rate_idx, pwr_index, shift;
	int j;

	for (j = 0; j < rtw_rate_size[rs]; j++) {
		rate = rtw_rate_section[rs][j];
		pwr_index = hal->tx_pwr_tbl[path][rate];
		shift = rate & 0x3;
		*phy_pwr_idx |= ((u32)pwr_index << (shift * 8));
		if (shift == 0x3) {
			rate_idx = rate & 0xfc;
			rtw_write32(rtwdev, offset_txagc[path] + rate_idx,
				    *phy_pwr_idx);
			*phy_pwr_idx = 0;
		}
	}
}

static void rtw8822b_set_tx_power_index(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 phy_pwr_idx = 0;
	int rs, path;

	for (path = 0; path < hal->rf_path_num; path++) {
		for (rs = 0; rs <= __RTW_RATE_SECTION_2SS_MAX; rs++)
			rtw8822b_set_tx_power_index_by_rate(rtwdev, path, rs,
							    &phy_pwr_idx);
	}
}

/* rtw8822b_rfe_defs[]: RTW_DEF_RFE(8822b, bb_pg, pwrlmt, track) */
static const struct {
	const struct rtw_table *phy_pg_tbl;
	const struct rtw_table *txpwr_lmt_tbl;
} rtw8822b_rfe_defs[] = {
	[2] = { &rtw8822b_bb_pg_type2_tbl, &rtw8822b_txpwr_lmt_type2_tbl },
	[3] = { &rtw8822b_bb_pg_type3_tbl, &rtw8822b_txpwr_lmt_type0_tbl },
	[5] = { &rtw8822b_bb_pg_type5_tbl, &rtw8822b_txpwr_lmt_type5_tbl },
};

/* rtw_check_supported_rfe() */
bool rtw_check_supported_rfe(struct rtw_dev *rtwdev)
{
	u8 rfe = rtwdev->efuse.rfe_option;

	return (rfe < ARRAY_SIZE(rtw8822b_rfe_defs) &&
	    rtw8822b_rfe_defs[rfe].phy_pg_tbl != NULL);
}

/* rtw_chip_board_info_setup(), minus the PHY condition (rtw88_phy.c) */
void rtw_chip_board_info_setup(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 rfe = rtwdev->efuse.rfe_option;

	rtw_phy_init_tx_power(rtwdev);
	rtw_load_table(rtwdev, rtw8822b_rfe_defs[rfe].phy_pg_tbl);
	rtw_load_table(rtwdev, rtw8822b_rfe_defs[rfe].txpwr_lmt_tbl);
	rtw_phy_tx_power_by_rate_config(hal);
	rtw_phy_tx_power_limit_config(hal);
}
