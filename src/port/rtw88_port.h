/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Interface between the code ported from Linux rtw88 (this directory) and
 * the rest of the driver.  Structure layouts are taken from rtw88 main.h
 * and rtw8822b.h.
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 */

#ifndef _RTW88_PORT_H_
#define _RTW88_PORT_H_

struct rtwb_softc;
struct mbuf;
struct ieee80211_node;
struct rtw_pwr_seq_cmd;

/* TX power calibration in the efuse (main.h, little-endian layout). */
struct rtw_2g_1s_pwr_idx_diff {
	int8_t ofdm:4;
	int8_t bw20:4;
} __packed;

struct rtw_2g_ns_pwr_idx_diff {
	int8_t bw20:4;
	int8_t bw40:4;
	int8_t cck:4;
	int8_t ofdm:4;
} __packed;

struct rtw_2g_txpwr_idx {
	uint8_t cck_base[6];
	uint8_t bw40_base[5];
	struct rtw_2g_1s_pwr_idx_diff ht_1s_diff;
	struct rtw_2g_ns_pwr_idx_diff ht_2s_diff;
	struct rtw_2g_ns_pwr_idx_diff ht_3s_diff;
	struct rtw_2g_ns_pwr_idx_diff ht_4s_diff;
};

struct rtw_5g_ht_1s_pwr_idx_diff {
	int8_t ofdm:4;
	int8_t bw20:4;
} __packed;

struct rtw_5g_ht_ns_pwr_idx_diff {
	int8_t bw20:4;
	int8_t bw40:4;
} __packed;

struct rtw_5g_ofdm_ns_pwr_idx_diff {
	int8_t ofdm_3s:4;
	int8_t ofdm_2s:4;
	int8_t ofdm_4s:4;
	int8_t res:4;
} __packed;

struct rtw_5g_vht_ns_pwr_idx_diff {
	int8_t bw160:4;
	int8_t bw80:4;
} __packed;

struct rtw_5g_txpwr_idx {
	uint8_t bw40_base[14];
	struct rtw_5g_ht_1s_pwr_idx_diff ht_1s_diff;
	struct rtw_5g_ht_ns_pwr_idx_diff ht_2s_diff;
	struct rtw_5g_ht_ns_pwr_idx_diff ht_3s_diff;
	struct rtw_5g_ht_ns_pwr_idx_diff ht_4s_diff;
	struct rtw_5g_ofdm_ns_pwr_idx_diff ofdm_diff;
	struct rtw_5g_vht_ns_pwr_idx_diff vht_1s_diff;
	struct rtw_5g_vht_ns_pwr_idx_diff vht_2s_diff;
	struct rtw_5g_vht_ns_pwr_idx_diff vht_3s_diff;
	struct rtw_5g_vht_ns_pwr_idx_diff vht_4s_diff;
} __packed;

struct rtw_txpwr_idx {
	struct rtw_2g_txpwr_idx pwr_idx_2g;
	struct rtw_5g_txpwr_idx pwr_idx_5g;
} __packed;


_Static_assert(sizeof(struct rtw_txpwr_idx) == 42, "efuse txpwr layout");

/* Table sizes of main.h (checked against the enums in rtw88_txpwr.c). */
#define RTW_RF_PATH_MAX		4
#define RTW_CHANNEL_WIDTH_MAX	3
#define RTW_MAX_CHANNEL_NUM_2G	14
#define RTW_MAX_CHANNEL_NUM_5G	49
#define RTW_RATE_SECTION_NUM_	10	/* RTW_RATE_SECTION_NUM */
#define RTW_REGD_MAX_		13	/* RTW_REGD_MAX */
#define DESC_RATE_MAX_		0x54	/* DESC_RATE_MAX */

/* Subset of struct rtw_efuse (main.h) filled from the logical efuse map. */
struct rtw_efuse {
	struct rtw_txpwr_idx txpwr_idx_table[4];
	uint8_t		addr[6];
	uint8_t		rfe_option;
	uint8_t		rf_board_option;
	uint8_t		crystal_cap;
	uint8_t		pa_type_2g;
	uint8_t		pa_type_5g;
	uint8_t		lna_type_2g;
	uint8_t		lna_type_5g;
	uint8_t		channel_plan;
	uint8_t		country_code[2];
	uint8_t		bt_setting;
	uint8_t		regd;
	uint8_t		thermal_meter;
};

/* struct rtw_phy_cond (main.h): match key of the PHY register tables. */
struct rtw_phy_cond {
	uint32_t rfe:8;
	uint32_t intf:4;
	uint32_t pkg:4;
	uint32_t plat:4;
	uint32_t intf_rsvd:4;
	uint32_t cut:4;
	uint32_t branch:2;
	uint32_t neg:1;
	uint32_t pos:1;
	/* for intf:4 */
	#define INTF_PCIE	BIT(0)
	#define INTF_USB	BIT(1)
	#define INTF_SDIO	BIT(2)
	/* for branch:2 */
	#define BRANCH_IF	0
	#define BRANCH_ELIF	1
	#define BRANCH_ELSE	2
	#define BRANCH_ENDIF	3
};

/* Subset of struct rtw_hal (main.h). */
struct rtw_hal {
	uint32_t	chip_version;
	uint8_t		cut_version;
	bool		mp_chip;
	uint8_t		rf_type;	/* RF_1T1R or RF_2T2R */
	uint8_t		rf_path_num;
	uint8_t		rf_phy_num;
	uint8_t		antenna_tx;	/* BB_PATH_x */
	uint8_t		antenna_rx;
	uint8_t		current_channel;
	uint8_t		primary_channel;
	uint8_t		current_band_width;
	uint8_t		current_band_type;
	uint8_t		current_primary_channel_index;
	uint8_t		cch_by_bw[3];
	struct rtw_phy_cond phy_cond;

	/* TX power (rtw88 phy.c) */
	int8_t		tx_pwr_by_rate_offset_2g[RTW_RF_PATH_MAX][DESC_RATE_MAX_];
	int8_t		tx_pwr_by_rate_offset_5g[RTW_RF_PATH_MAX][DESC_RATE_MAX_];
	int8_t		tx_pwr_by_rate_base_2g[RTW_RF_PATH_MAX][RTW_RATE_SECTION_NUM_];
	int8_t		tx_pwr_by_rate_base_5g[RTW_RF_PATH_MAX][RTW_RATE_SECTION_NUM_];
	int8_t		tx_pwr_limit_2g[RTW_REGD_MAX_][RTW_CHANNEL_WIDTH_MAX]
			    [RTW_RATE_SECTION_NUM_][RTW_MAX_CHANNEL_NUM_2G];
	int8_t		tx_pwr_limit_5g[RTW_REGD_MAX_][RTW_CHANNEL_WIDTH_MAX]
			    [RTW_RATE_SECTION_NUM_][RTW_MAX_CHANNEL_NUM_5G];
	uint8_t		tx_pwr_tbl[RTW_RF_PATH_MAX][DESC_RATE_MAX_];
};

/* struct rtw_fifo_conf (main.h): TX FIFO page layout, in 128-byte pages. */
struct rtw_fifo_conf {
	uint16_t	rsvd_boundary;
	uint16_t	rsvd_pg_num;
	uint16_t	rsvd_drv_pg_num;
	uint16_t	txff_pg_num;
	uint16_t	acq_pg_num;
	uint16_t	rsvd_drv_addr;
	uint16_t	rsvd_h2c_info_addr;
	uint16_t	rsvd_h2c_sta_info_addr;
	uint16_t	rsvd_h2cq_addr;
	uint16_t	rsvd_cpu_instr_addr;
	uint16_t	rsvd_fw_txbuf_addr;
	uint16_t	rsvd_csibuf_addr;
};

/* Subset of struct rtw_rx_pkt_stat (main.h). */
struct rtw_rx_pkt_stat {
	bool		phy_status;
	bool		icv_err;
	bool		crc_err;
	bool		decrypted;
	bool		is_c2h;
	uint8_t		cam_id;
	uint8_t		ppdu_cnt;
	uint8_t		rate;
	uint8_t		bw;
	uint8_t		shift;
	uint16_t	pkt_len;
	uint32_t	drv_info_sz;
	uint32_t	tsf_low;
	/* From the PHY status (rtw8822b_query_phy_status). */
	int8_t		rx_power[2];	/* dBm per RF path */
	int8_t		signal_power;	/* dBm, best path */
};

/* struct rtw_tx_pkt_info (main.h): what goes into a TX packet descriptor. */
struct rtw_tx_pkt_info {
	uint32_t	tx_pkt_size;
	uint8_t		offset;
	uint8_t		pkt_offset;
	uint8_t		tim_offset;
	uint8_t		mac_id;
	uint8_t		rate_id;
	uint8_t		rate;
	uint8_t		qsel;
	uint8_t		bw;
	uint8_t		sec_type;
	uint8_t		sn;
	bool		ampdu_en;
	uint8_t		ampdu_factor;
	uint8_t		ampdu_density;
	uint16_t	seq;
	bool		stbc;
	bool		ldpc;
	bool		dis_rate_fallback;
	bool		bmc;
	bool		use_rate;
	bool		ls;
	bool		fs;
	bool		short_gi;
	bool		report;
	bool		rts;
	bool		dis_qselseq;
	bool		en_hwseq;
	uint8_t		hw_ssn_sel;
	bool		nav_use_hdr;
	bool		bt_null;
};

/* Subset of struct rtw_sta_info (main.h): the peer as seen by the RA. */
struct rtw_sta_info {
	uint8_t		mac_id;
	uint8_t		rate_id;
	uint8_t		bw_mode;
	uint8_t		stbc_en;
	uint8_t		ldpc_en;
	uint8_t		init_ra_lv;
	bool		sgi_enable;
	bool		vht_enable;
	bool		ht_enable;	/* rtwb: for get_highest_ht_tx_rate() */
	bool		ht_2ss;
	uint16_t	vht_tx_mcs_map;
	uint64_t	ra_mask;
};

/* Power sequence tables (rtw8822b_pwrseq.c). */
extern const struct rtw_pwr_seq_cmd * const card_enable_flow_8822b[];
extern const struct rtw_pwr_seq_cmd * const card_disable_flow_8822b[];

/* rtw88_mac.c */
int	rtw_mac_power_on(struct rtwb_softc *);
void	rtw_mac_power_off(struct rtwb_softc *);
int	rtw_mac_init(struct rtwb_softc *);
bool	check_hw_ready(struct rtwb_softc *, uint32_t, uint32_t, uint32_t);

/* rtw8822b.c */
int	rtw8822b_mac_init(struct rtwb_softc *);
void	rtw8822b_phy_set_param(struct rtwb_softc *);
void	rtw8822b_set_channel(struct rtwb_softc *, uint8_t, uint8_t, uint8_t);
void	rtw_update_channel(struct rtwb_softc *, uint8_t, uint8_t, uint8_t,
	    uint8_t);
void	rtw8822b_query_phy_status(struct rtwb_softc *, const uint8_t *,
	    struct rtw_rx_pkt_stat *);

/* rtw88_txpwr.c */
bool	rtw_check_supported_rfe(struct rtwb_softc *);
void	rtw_chip_board_info_setup(struct rtwb_softc *);
void	rtw_phy_set_tx_power_level(struct rtwb_softc *, uint8_t);

/* rtw88_efuse.c */
int	rtw_parse_efuse_map(struct rtwb_softc *, struct rtw_efuse *);

/* rtw88_pci.c */
void	rtw_pci_setup(struct rtwb_softc *);
void	rtw_pci_init_irq_mask(struct rtwb_softc *);
void	rtw_pci_enable_interrupt(struct rtwb_softc *, bool);
void	rtw_pci_disable_interrupt(struct rtwb_softc *);
void	rtw_pci_intr(struct rtwb_softc *);
void	rtw_pci_dma_release(struct rtwb_softc *);
int	rtw_pci_write_data_rsvd_page(struct rtwb_softc *, const uint8_t *,
	    uint32_t);
int	rtw_pci_write_data_h2c(struct rtwb_softc *, const uint8_t *, uint32_t);
int	rtw_pci_tx_write_data(struct rtwb_softc *, int,
	    struct rtw_tx_pkt_info *, struct mbuf *, struct ieee80211_node *);
bool	rtw_pci_tx_ring_full(struct rtwb_softc *, int);
void	rtw_rx_query_rx_desc(const uint8_t *, struct rtw_rx_pkt_stat *);

/* rtw88_fw.c */
bool	ltecoex_read_reg(struct rtwb_softc *, uint16_t, uint32_t *);
bool	ltecoex_reg_write(struct rtwb_softc *, uint16_t, uint32_t);
int	rtw_download_firmware(struct rtwb_softc *, const uint8_t *, uint32_t);
int	rtw_dump_hw_feature(struct rtwb_softc *);
void	rtw_fw_send_general_info(struct rtwb_softc *);
void	rtw_fw_send_phydm_info(struct rtwb_softc *);
void	rtw_fw_media_status_report(struct rtwb_softc *, uint8_t, bool);
void	rtw_fw_send_h2c_command(struct rtwb_softc *, const uint8_t *);

/* rtw88_ra.c */
void	rtw_update_sta_info_80211(struct rtwb_softc *, struct ieee80211_node *,
	    struct rtw_sta_info *, bool);
uint8_t	rtw_sta_highest_tx_rate(struct rtwb_softc *,
	    const struct rtw_sta_info *);

/* rtw88_phy.c */
void	rtw_phy_setup_phy_cond(struct rtwb_softc *, uint32_t);

/* rtw88_coex.c */
void	rtw_coex_init_wifi_only(struct rtwb_softc *);
void	rtw_coex_wifi_off(struct rtwb_softc *);

/* rtw88_tx.c */
void	rtw_tx_fill_tx_desc(struct rtwb_softc *, struct rtw_tx_pkt_info *,
	    uint8_t *);
int	rtw_tx_pkt_info_80211(struct rtwb_softc *, struct ieee80211_node *,
	    struct mbuf *, struct rtw_tx_pkt_info *);
int	rtw_tx_queue_80211(struct mbuf *);

/* Provided by the driver (if_rtwb.c). */
void	rtwb_tx_ring_drained(struct rtwb_softc *);
void	rtwb_rx_c2h(struct rtwb_softc *, const uint8_t *, uint32_t);
void	rtwb_rx_frame(struct rtwb_softc *, const uint8_t *,
	    const struct rtw_rx_pkt_stat *, uint32_t);

#endif /* _RTW88_PORT_H_ */
