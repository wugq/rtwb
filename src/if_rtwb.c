/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 wugq <wugq.dev@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * rtwb: native net80211 driver for the Realtek RTL8822BE (PCIe).
 *
 * Attach identifies the chip, reads the efuse, asks the firmware for the
 * hardware capabilities and powers the chip off again, like rtw88's probe.
 * The full bring-up (power on, firmware, MAC/BB/RF init, rings,
 * interrupts) runs when net80211 brings the interface up (ic_parent) and
 * is undone when it goes down.
 *
 * Station and monitor mode, 802.11a/b/g/n/ac (up to VHT80, two streams),
 * rate adaptation in the firmware, software crypto in net80211.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/firmware.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ratectl.h>

#include "if_rtwbvar.h"
#include "port/rtw88_reg.h"

#define RTWB_FW_NAME	"rtw88/rtw8822b_fw.bin"

/* REG_RCR bits (rtw88 reg.h). */
#define RCR_APP_FCS		BIT(31)
#define RCR_APP_MIC		BIT(30)
#define RCR_APP_ICV		BIT(29)
#define RCR_APP_PHYSTS		BIT(28)
#define RCR_PKTCTL_DLEN		BIT(20)
#define RCR_HTC_LOC_CTRL	BIT(14)
#define RCR_CBSSID_BCN		BIT(7)
#define RCR_CBSSID_DATA		BIT(6)
#define RCR_APWRMGT		BIT(5)
#define RCR_ADD3		BIT(4)
#define RCR_AB			BIT(3)
#define RCR_AM			BIT(2)
#define RCR_APM			BIT(1)
#define RCR_AAP			BIT(0)

/* rtw88 main.c rtw_core_init(): default rx filter setting */
#define RTWB_RCR_DEFAULT	(RCR_APP_FCS | RCR_APP_MIC | RCR_APP_ICV | \
				 RCR_PKTCTL_DLEN | RCR_HTC_LOC_CTRL | \
				 RCR_APP_PHYSTS | RCR_AB | RCR_AM | RCR_APM)

/* Port 0 registers (rtw88 mac80211.c rtw_vif_port[0]). */
#define REG_PORT0_MACADDR	0x0610
#define REG_PORT0_BSSID		0x0618
#define REG_PORT0_NETTYPE	0x0100		/* REG_CR, mask 0x30000 */
#define PORT0_NETTYPE_MASK	0x00030000
#define PORT0_NETTYPE_SHIFT	16
#define REG_PORT0_AID		0x06a8		/* mask 0x7ff */
#define RTW_NET_NO_LINK		0
#define RTW_NET_MGD_LINKED	2

static const uint8_t rtwb_chan_5ghz[] = {
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165
};

static device_detach_t rtwb_detach;
static void	rtwb_drain_snd(struct rtwb_softc *);

static const struct rtwb_ident {
	uint16_t	vendor;
	uint16_t	device;
	const char	*name;
} rtwb_ident_table[] = {
	{ 0x10ec, 0xb822, "Realtek RTL8822BE 802.11ac" },
};

static int
rtwb_probe(device_t dev)
{
	int i;

	for (i = 0; i < nitems(rtwb_ident_table); i++) {
		if (pci_get_vendor(dev) == rtwb_ident_table[i].vendor &&
		    pci_get_device(dev) == rtwb_ident_table[i].device) {
			device_set_desc(dev, rtwb_ident_table[i].name);
			return (BUS_PROBE_DEFAULT);
		}
	}
	return (ENXIO);
}

static void
rtwb_read_chip_info(struct rtwb_softc *sc)
{
	struct rtw_hal *hal = &sc->hal;
	uint32_t ver;

	/* REG_SYS_CFG1 is valid before power-on (rtw88 main.c). */
	ver = rtwb_read_4(sc, REG_SYS_CFG1);
	hal->chip_version = ver;
	hal->cut_version = BIT_GET_CHIP_VER(ver);
	hal->mp_chip = (ver & BIT_RTL_ID) ? false : true;
	if (ver & BIT_RF_TYPE_ID) {
		hal->rf_type = RF_2T2R;
		hal->rf_path_num = 2;
		hal->antenna_tx = BB_PATH_AB;
		hal->antenna_rx = BB_PATH_AB;
	} else {
		hal->rf_type = RF_1T1R;
		hal->rf_path_num = 1;
		hal->antenna_tx = BB_PATH_A;
		hal->antenna_rx = BB_PATH_A;
	}
	hal->rf_phy_num = hal->rf_path_num;	/* no fix_rf_phy_num on 8822B */

	device_printf(sc->sc_dev, "SYS_CFG1 %#010x: %s chip, cut %c, %dT%dR\n",
	    ver, hal->mp_chip ? "production" : "test",
	    'A' + hal->cut_version, hal->rf_path_num, hal->rf_path_num);
}

/* Download the cached firmware image.  MAC on, bus mastering enabled. */
static int
rtwb_download_firmware(struct rtwb_softc *sc)
{
	int error;

	error = rtw_download_firmware(sc, sc->sc_fw->data,
	    sc->sc_fw->datasize);
	if (error != 0)
		device_printf(sc->sc_dev,
		    "firmware download failed (%d), MCUFW_CTRL %#06x\n",
		    error, rtwb_read_2(sc, REG_MCUFW_CTRL));
	return (error);
}

static int
rtwb_get_firmware(struct rtwb_softc *sc)
{
	const uint8_t *hdr;

	/* NOWARN: firmware(9) first tries a kernel module of that name. */
	sc->sc_fw = firmware_get_flags(RTWB_FW_NAME, FIRMWARE_GET_NOWARN);
	if (sc->sc_fw == NULL) {
		device_printf(sc->sc_dev, "could not load firmware %s\n",
		    RTWB_FW_NAME);
		return (ENOENT);
	}
	if (sc->sc_fw->datasize < FW_HDR_SIZE)
		return (EINVAL);

	hdr = sc->sc_fw->data;
	sc->sc_fw_version = le16dec(hdr + FW_HDR_VERSION);
	sc->sc_fw_subversion = hdr[FW_HDR_SUBVERSION];
	sc->sc_fw_subindex = hdr[FW_HDR_SUBINDEX];
	sc->sc_fw_h2c_version = le16dec(hdr + FW_HDR_H2C_FMT_VER);
	return (0);
}

/*
 * rtw_chip_efuse_info_setup(): power on, read the efuse, run the firmware
 * once for its HW feature report, power off again.
 */
static int
rtwb_read_hw_info(struct rtwb_softc *sc)
{
	struct rtw_efuse *efuse = &sc->efuse;
	int error;

	rtw_pci_disable_interrupt(sc);
	rtw_pci_setup(sc);
	error = rtw_mac_power_on(sc);
	if (error != 0)
		return (error);
	sc->sc_powered = true;

	/* The firmware answers this through REG_C2HEVT once it runs. */
	rtwb_write_1(sc, REG_C2HEVT, C2H_HW_FEATURE_DUMP);

	error = rtw_parse_efuse_map(sc, efuse);
	if (error == 0) {
		pci_enable_busmaster(sc->sc_dev);
		error = rtwb_download_firmware(sc);
	}
	if (error == 0)
		error = rtw_dump_hw_feature(sc);

	rtw_mac_power_off(sc);
	sc->sc_powered = false;
	pci_disable_busmaster(sc->sc_dev);
	if (error != 0)
		return (error);

	if (!rtw_check_supported_rfe(sc)) {
		device_printf(sc->sc_dev, "unsupported RFE option %u\n",
		    efuse->rfe_option);
		return (ENODEV);
	}
	rtw_phy_setup_phy_cond(sc, 0);
	rtw_chip_board_info_setup(sc);

	device_printf(sc->sc_dev, "MAC %6D, RFE option %u, firmware %u.%u.%u, "
	    "hw cap: bw %#x, nss %u, ptcl %u, ant_num %u\n",
	    efuse->addr, ":", efuse->rfe_option, sc->sc_fw_version,
	    sc->sc_fw_subversion, sc->sc_fw_subindex, sc->sc_hw_cap_bw,
	    sc->sc_hw_cap_nss, sc->sc_hw_cap_ptcl, sc->sc_hw_cap_ant_num);
	return (0);
}

static void
rtwb_write_addr(struct rtwb_softc *sc, uint16_t reg, const uint8_t *addr)
{
	int i;

	for (i = 0; i < IEEE80211_ADDR_LEN; i++)
		rtwb_write_1(sc, reg + i, addr[i]);
}

/* Receive filter: rtw88's default, opened up in monitor mode. */
static void
rtwb_update_rcr(struct rtwb_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	RTWB_LOCK_ASSERT(sc);
	sc->sc_rcr = RTWB_RCR_DEFAULT;
	if (ic->ic_opmode == IEEE80211_M_MONITOR || ic->ic_promisc > 0)
		sc->sc_rcr |= RCR_AAP | RCR_ADD3 | RCR_APWRMGT;
	else if (sc->sc_assoc)
		sc->sc_rcr |= RCR_CBSSID_DATA | RCR_CBSSID_BCN;
	if (sc->sc_running)
		rtwb_write_4(sc, REG_RCR, sc->sc_rcr);
}

/*
 * rtw_set_channel() without mac80211: 'primary' is the 20 MHz control
 * channel, 'center' the center of the 'bw' wide channel.
 */
static int
rtwb_set_chan_bw(struct rtwb_softc *sc, uint8_t primary, uint8_t center,
    uint8_t bw)
{
	uint8_t band;

	RTWB_LOCK_ASSERT(sc);
	KASSERT(bw == RTW_CHANNEL_WIDTH_20 || bw == RTW_CHANNEL_WIDTH_40 ||
	    bw == RTW_CHANNEL_WIDTH_80, ("bad channel width %u", bw));
	if (!((primary >= 1 && primary <= 14) ||
	    (primary >= 36 && primary <= 177)))
		return (EINVAL);
	band = primary > 14 ? RTW_BAND_5G_ : RTW_BAND_2G_;
	rtw_update_channel(sc, center, primary, band, bw);
	rtw8822b_set_channel(sc, center, bw,
	    sc->hal.current_primary_channel_index);
	rtw_phy_set_tx_power_level(sc, center);
	return (0);
}

static int
rtwb_set_chan(struct rtwb_softc *sc, uint8_t chan)
{
	return (rtwb_set_chan_bw(sc, chan, chan, RTW_CHANNEL_WIDTH_20));
}

/* rtw_get_channel_params() for a net80211 channel. */
static int
rtwb_set_ieee_chan(struct rtwb_softc *sc, struct ieee80211_channel *c)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t chan = ieee80211_chan2ieee(ic, c);

	if (IEEE80211_IS_CHAN_VHT80(c))
		return (rtwb_set_chan_bw(sc, chan, c->ic_vht_ch_freq1,
		    RTW_CHANNEL_WIDTH_80));
	if (IEEE80211_IS_CHAN_HT40U(c))
		return (rtwb_set_chan_bw(sc, chan, chan + 2,
		    RTW_CHANNEL_WIDTH_40));
	if (IEEE80211_IS_CHAN_HT40D(c))
		return (rtwb_set_chan_bw(sc, chan, chan - 2,
		    RTW_CHANNEL_WIDTH_40));
	return (rtwb_set_chan(sc, chan));
}

/*
 * rtw_core_start() / rtw_power_on().  Called with sc_sx held and sc_mtx
 * not held: the BB/RF tables sleep.  sc_running stays false until the
 * end, so the interrupt handler and the net80211 methods do not touch
 * the hardware meanwhile; the last part, which sends H2C commands and
 * enables interrupts, runs under sc_mtx.
 */
static int
rtwb_hw_init(struct rtwb_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	int error;

	RTWB_SX_ASSERT(sc);
	RTWB_LOCK_ASSERT_NOTOWNED(sc);
	if (sc->sc_running)
		return (0);

	rtw_pci_disable_interrupt(sc);
	rtw_pci_setup(sc);
	error = rtw_mac_power_on(sc);
	if (error != 0)
		return (error);
	RTWB_LOCK(sc);
	sc->sc_powered = true;
	RTWB_UNLOCK(sc);

	pci_enable_busmaster(sc->sc_dev);
	error = rtwb_download_firmware(sc);
	if (error != 0)
		goto fail;
	sc->sc_h2c_seq = 0;
	sc->sc_h2c_last_box = 0;

	error = rtw_mac_init(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "MAC init failed (%d)\n", error);
		goto fail;
	}

	/* BB/RF: register tables, TRX paths, RFE pins (phy_set_param). */
	rtw8822b_phy_set_param(sc);

	RTWB_LOCK(sc);
	rtwb_write_addr(sc, REG_PORT0_MACADDR,
	    vap != NULL ? vap->iv_myaddr : ic->ic_macaddr);

	error = rtwb_set_ieee_chan(sc, ic->ic_curchan);
	if (error != 0) {
		RTWB_UNLOCK(sc);
		goto fail;
	}

	/* rtw_hci_start(): interrupts on. */
	rtw_pci_init_irq_mask(sc);
	sc->sc_running = true;
	rtwb_update_rcr(sc);
	rtw_pci_enable_interrupt(sc, false);

	/* send H2C after HCI has started */
	rtw_fw_send_general_info(sc);
	rtw_fw_send_phydm_info(sc);

	/* WLAN/BT antenna sharing: Wi-Fi only for now (see rtw88_coex.c). */
	rtw_coex_init_wifi_only(sc);
	RTWB_UNLOCK(sc);
	return (0);

fail:
	rtw_mac_power_off(sc);
	RTWB_LOCK(sc);
	sc->sc_powered = false;
	RTWB_UNLOCK(sc);
	pci_disable_busmaster(sc->sc_dev);
	return (error);
}

/* rtw_core_stop() / rtw_power_off(), with sc_sx and sc_mtx held. */
static void
rtwb_hw_stop(struct rtwb_softc *sc)
{
	RTWB_SX_ASSERT(sc);
	RTWB_LOCK_ASSERT(sc);

	sc->sc_running = false;
	sc->sc_assoc = false;
	rtw_pci_disable_interrupt(sc);
	if (sc->sc_powered) {
		/* Powering the MAC off stops its DMA engines. */
		rtw_pci_dma_release(sc);
		rtw_mac_power_off(sc);
		sc->sc_powered = false;
	}
	pci_disable_busmaster(sc->sc_dev);
	mbufq_drain(&sc->sc_rxq);
	rtwb_drain_snd(sc);
	KASSERT(mbufq_len(&sc->sc_snd) == 0, ("send queue not empty"));
}

/* RX hooks called by port/rtw88_pci.c with the softc lock held. */
/* rtw88 fw.h: C2H ids and the CCX TX report layouts */
#define C2H_CCX_TX_RPT		0x03
#define C2H_HALMAC		0xff
#define C2H_CCX_RPT		0x0f	/* sub id of C2H_HALMAC */
#define C2H_RA_RPT		0x0c	/* struct rtw_c2h_ra_rpt, 7 bytes */
#define CCX_REPORT_SEQNUM_V0(p)	((p)[6] & 0xfc)
#define CCX_REPORT_STATUS_V0(p)	((p)[0] & 0xc0)
#define CCX_REPORT_SEQNUM_V1(p)	((p)[8] & 0xfc)
#define CCX_REPORT_STATUS_V1(p)	((p)[9] & 0xc0)

void
rtwb_rx_c2h(struct rtwb_softc *sc, const uint8_t *c2h, uint32_t len)
{
	const uint8_t *payload = c2h + 2;	/* struct rtw_c2h_cmd */
	uint8_t sn, st;

	sc->sc_rx_c2h++;
	if (len < 2)
		return;
	sc->sc_last_c2h_id = c2h[0];

	/* rtw_fw_ra_report_handle(): the rate the firmware RA settled on */
	if (c2h[0] == C2H_RA_RPT && len >= 2 + 7) {
		sc->sc_ra_rate = payload[0] & 0x7f;
		sc->sc_ra_sgi = (payload[0] & 0x80) != 0;
		sc->sc_ra_bw = payload[6];
		sc->sc_ra_reports++;
		return;
	}

	/* rtw_tx_report_handle(): status 0 means the frame was delivered */
	if (c2h[0] == C2H_CCX_TX_RPT && len >= 2 + 7) {
		sn = CCX_REPORT_SEQNUM_V0(payload);
		st = CCX_REPORT_STATUS_V0(payload);
	} else if (c2h[0] == C2H_HALMAC && len >= 2 + 10 &&
	    payload[0] == C2H_CCX_RPT) {
		sn = CCX_REPORT_SEQNUM_V1(payload);
		st = CCX_REPORT_STATUS_V1(payload);
	} else
		return;

	if (st == 0)
		sc->sc_tx_rpt_ok++;
	else
		sc->sc_tx_rpt_fail++;
	if (sc->sc_tx_rpt_ok + sc->sc_tx_rpt_fail <= 8)
		device_printf(sc->sc_dev, "TX report sn %#04x status %#04x\n",
		    sn, st);
}

/* Legacy rate codes 0..11 in 500 kb/s units. */
static const uint8_t rtwb_legacy_rates[] = {
	2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
};

/*
 * Copy one received frame into an mbuf and queue it; rtwb_intr() hands
 * the queue to net80211 after dropping the lock (rtw_pci_rx_napi() +
 * rtw_rx_fill_rx_status()).
 */
void
rtwb_rx_frame(struct rtwb_softc *sc, const uint8_t *rx_desc,
    const struct rtw_rx_pkt_stat *ps0, uint32_t pkt_offset)
{
	struct ieee80211_rx_stats rxs;
	struct rtw_rx_pkt_stat ps = *ps0;
	struct mbuf *m;
	uint32_t len;
	int8_t signal;

	if (ps.crc_err || ps.icv_err) {
		sc->sc_rx_crc_err++;
		return;
	}
	len = ps.pkt_len;
	if (sc->sc_rcr & RCR_APP_FCS) {
		if (len < IEEE80211_CRC_LEN)
			return;
		len -= IEEE80211_CRC_LEN;
	}
	if (len < sizeof(struct ieee80211_frame_ack) || len > MJUM16BYTES)
		return;
	sc->sc_rx_frames++;
	if (len >= 24 && rx_desc[pkt_offset] == 0x80)
		sc->sc_rx_beacons++;

	m = m_get3(len, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		counter_u64_add(sc->sc_ic.ic_ierrors, 1);
		return;
	}
	memcpy(mtod(m, uint8_t *), rx_desc + pkt_offset, len);
	m->m_pkthdr.len = m->m_len = len;

	signal = RTWB_NOISE_FLOOR;
	if (ps.phy_status && ps.drv_info_sz != 0) {
		rtw8822b_query_phy_status(sc,
		    rx_desc + RTWB_RX_PKT_DESC_SZ + ps.shift, &ps);
		signal = ps.signal_power;
	}

	memset(&rxs, 0, sizeof(rxs));
	rxs.r_flags = IEEE80211_R_NF | IEEE80211_R_RSSI | IEEE80211_R_TSF32 |
	    IEEE80211_R_TSF_START;
	rxs.c_nf = RTWB_NOISE_FLOOR;
	rxs.c_rssi = MAX(signal - RTWB_NOISE_FLOOR, 0);
	rxs.c_rx_tsf = ps.tsf_low;
	if (ps.rate <= DESC_RATE54M) {
		rxs.c_pktflags |= ps.rate <= DESC_RATE11M ?
		    IEEE80211_RX_F_CCK : IEEE80211_RX_F_OFDM;
		rxs.c_rate = rtwb_legacy_rates[ps.rate];
	} else if (ps.rate <= DESC_RATEMCS31) {
		rxs.c_pktflags |= IEEE80211_RX_F_HT;
		rxs.c_rate = ps.rate - DESC_RATEMCS0;
	} else if (ps.rate >= DESC_RATEVHT1SS_MCS0) {
		rxs.c_pktflags |= IEEE80211_RX_F_VHT;
		rxs.c_vhtnss = (ps.rate - DESC_RATEVHT1SS_MCS0) / 10 + 1;
		rxs.c_rate = (ps.rate - DESC_RATEVHT1SS_MCS0) % 10;
	}
	(void)ieee80211_add_rx_params(m, &rxs);

	if (mbufq_enqueue(&sc->sc_rxq, m) != 0) {
		counter_u64_add(sc->sc_ic.ic_ierrors, 1);
		m_freem(m);
	}
}

/* Hand queued frames to net80211, without the softc lock. */
static void
rtwb_rx_deliver(struct rtwb_softc *sc, struct mbufq *q)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame_min *wh;
	struct ieee80211_rx_stats rxs;
	struct ieee80211_node *ni;
	struct epoch_tracker et;
	struct mbuf *m;

	NET_EPOCH_ENTER(et);
	while ((m = mbufq_dequeue(q)) != NULL) {
		if (ieee80211_radiotap_active(ic) &&
		    ieee80211_get_rx_params(m, &rxs) == 0) {
			struct rtwb_rx_radiotap_header *tap = &sc->sc_rxtap;

			tap->wr_flags = 0;
			tap->wr_tsft = htole64(rxs.c_rx_tsf);
			if (rxs.c_pktflags & (IEEE80211_RX_F_HT |
			    IEEE80211_RX_F_VHT))
				tap->wr_rate = IEEE80211_RATE_MCS | rxs.c_rate;
			else
				tap->wr_rate = rxs.c_rate;
			tap->wr_dbm_antsignal = rxs.c_rssi + rxs.c_nf;
			tap->wr_dbm_antnoise = rxs.c_nf;
		}

		wh = mtod(m, struct ieee80211_frame_min *);
		if (m->m_len >= sizeof(*wh))
			ni = ieee80211_find_rxnode(ic, wh);
		else
			ni = NULL;
		if (ni != NULL) {
			if (ni->ni_flags & IEEE80211_NODE_HT)
				m->m_flags |= M_AMPDU;
			(void)ieee80211_input_mimo(ni, m);
			ieee80211_free_node(ni);
		} else
			(void)ieee80211_input_mimo_all(ic, m);
	}
	NET_EPOCH_EXIT(et);
}

static void
rtwb_intr(void *arg)
{
	struct rtwb_softc *sc = arg;
	struct mbufq q;
	struct mbuf *m;

	mbufq_init(&q, RTWB_RX_RING_LEN);
	RTWB_LOCK(sc);
	if (!sc->sc_running) {
		RTWB_UNLOCK(sc);
		return;
	}
	sc->sc_intr_count++;
	rtw_pci_intr(sc);
	while ((m = mbufq_dequeue(&sc->sc_rxq)) != NULL)
		(void)mbufq_enqueue(&q, m);
	RTWB_UNLOCK(sc);

	rtwb_rx_deliver(sc, &q);
}

static int
rtwb_setup_intr(struct rtwb_softc *sc)
{
	device_t dev = sc->sc_dev;
	int count, error;

	count = 1;
	if (pci_msi_count(dev) > 0 && pci_alloc_msi(dev, &count) == 0) {
		sc->sc_msi = true;
		sc->sc_irq_rid = 1;
	} else {
		sc->sc_irq_rid = 0;
	}
	sc->sc_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->sc_irq_rid,
	    RF_ACTIVE | (sc->sc_msi ? 0 : RF_SHAREABLE));
	if (sc->sc_irq == NULL) {
		device_printf(dev, "could not allocate interrupt\n");
		return (ENXIO);
	}
	error = bus_setup_intr(dev, sc->sc_irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, rtwb_intr, sc, &sc->sc_ih);
	if (error != 0)
		device_printf(dev, "could not set up interrupt (%d)\n", error);
	return (error);
}

static void
rtwb_teardown_intr(struct rtwb_softc *sc)
{
	device_t dev = sc->sc_dev;

	if (sc->sc_ih != NULL) {
		bus_teardown_intr(dev, sc->sc_irq, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid,
		    sc->sc_irq);
		sc->sc_irq = NULL;
	}
	if (sc->sc_msi) {
		pci_release_msi(dev);
		sc->sc_msi = false;
	}
}

/* --- net80211 methods --- */

static void
rtwb_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
    struct ieee80211_channel chans[])
{
	struct rtwb_softc *sc = ic->ic_softc;
	uint8_t bands[IEEE80211_MODE_BYTES];
	int cbw_flags;

	cbw_flags = (sc->sc_hw_cap_bw & BIT(RTW_CHANNEL_WIDTH_40)) ?
	    NET80211_CBW_FLAG_HT40 : 0;

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	setbit(bands, IEEE80211_MODE_11NG);
	ieee80211_add_channels_default_2ghz(chans, maxchans, nchans, bands,
	    cbw_flags);

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11A);
	setbit(bands, IEEE80211_MODE_11NA);
	if (IEEE80211_CONF_VHT(ic)) {
		setbit(bands, IEEE80211_MODE_VHT_5GHZ);
		/* Only enable VHT80 if HT40/VHT40 is available */
		if ((cbw_flags & NET80211_CBW_FLAG_HT40) &&
		    (sc->sc_hw_cap_bw & BIT(RTW_CHANNEL_WIDTH_80)))
			cbw_flags |= NET80211_CBW_FLAG_VHT80;
	}
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
	    rtwb_chan_5ghz, nitems(rtwb_chan_5ghz), bands, cbw_flags);
}

static void
rtwb_set_channel(struct ieee80211com *ic)
{
	struct rtwb_softc *sc = ic->ic_softc;

	RTWB_LOCK(sc);
	if (sc->sc_running)
		(void)rtwb_set_ieee_chan(sc, ic->ic_curchan);
	RTWB_UNLOCK(sc);
}

/* The channel width of the BSS changed (HT operation IE). */
static void
rtwb_update_chw(struct ieee80211com *ic)
{
	rtwb_set_channel(ic);
}

static void
rtwb_scan_start(struct ieee80211com *ic)
{
	/* The default filter already accepts beacons from any BSSID. */
}

static void
rtwb_scan_end(struct ieee80211com *ic)
{
}

static void
rtwb_update_promisc(struct ieee80211com *ic)
{
	struct rtwb_softc *sc = ic->ic_softc;

	RTWB_LOCK(sc);
	rtwb_update_rcr(sc);
	RTWB_UNLOCK(sc);
}

static void
rtwb_update_mcast(struct ieee80211com *ic)
{
	/* RCR_AM accepts all multicast. */
}

static void
rtwb_parent(struct ieee80211com *ic)
{
	struct rtwb_softc *sc = ic->ic_softc;
	bool startall = false;
	int error;

	RTWB_SX_LOCK(sc);
	if (ic->ic_nrunning > 0) {
		if (!sc->sc_running) {
			error = rtwb_hw_init(sc);
			if (error == 0)
				startall = true;
			else
				device_printf(sc->sc_dev,
				    "hardware init failed (%d)\n", error);
		}
	} else {
		RTWB_LOCK(sc);
		if (sc->sc_running || sc->sc_powered)
			rtwb_hw_stop(sc);
		RTWB_UNLOCK(sc);
	}
	RTWB_SX_UNLOCK(sc);

	if (startall)
		ieee80211_start_all(ic);
}

/* Make the frame one contiguous buffer: the payload is one DMA segment. */
static struct mbuf *
rtwb_tx_linearize(struct mbuf *m)
{
	struct mbuf *n;

	if (m->m_next == NULL)
		return (m);
	if (m->m_pkthdr.len > RTWB_TX_MAX_SIZE)
		goto drop;
	n = m_get3(m->m_pkthdr.len, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (n == NULL)
		goto drop;
	m_copydata(m, 0, m->m_pkthdr.len, mtod(n, caddr_t));
	n->m_len = m->m_pkthdr.len;
	M_MOVE_PKTHDR(n, m);	/* keeps the net80211 tags (callbacks) */
	m_freem(m);
	return (n);
drop:
	m_freem(m);
	return (NULL);
}

/*
 * Queue one 802.11 frame (rtw_tx()).  If its ring is full, return EAGAIN
 * without touching 'm'.  Otherwise always consume 'm'; on success the
 * ring slot keeps the node reference until the frame is done.
 */
static int
rtwb_tx_start(struct rtwb_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m)
{
	struct ieee80211vap *vap = ni->ni_vap;
	struct rtw_tx_pkt_info pkt_info;
	struct ieee80211_frame *wh;
	int error, queue;

	RTWB_LOCK_ASSERT(sc);
	KASSERT(ni != NULL, ("TX frame without a node"));
	KASSERT(sc->sc_running, ("TX while the hardware is down"));

	if (rtw_pci_tx_ring_full(sc, rtw_tx_queue_80211(m)))
		return (EAGAIN);

	wh = mtod(m, struct ieee80211_frame *);
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
		/* Software crypto: net80211 adds IV/MIC and encrypts. */
		if (ieee80211_crypto_encap(ni, m) == NULL) {
			sc->sc_tx_err_encap++;
			m_freem(m);
			return (ENOBUFS);
		}
	}
	m = rtwb_tx_linearize(m);
	if (m == NULL) {
		sc->sc_tx_err_mbuf++;
		return (ENOBUFS);
	}

	queue = rtw_tx_pkt_info_80211(sc, ni, m, &pkt_info);
	if (sc->sc_tx_report) {
		/* rtw_tx_report_enable(): sn in [7:2] */
		pkt_info.sn = (++sc->sc_tx_report_sn << 2) & 0xfc;
		pkt_info.report = true;
	}

	if (ieee80211_radiotap_active_vap(vap)) {
		sc->sc_txtap.wt_flags = 0;
		ieee80211_radiotap_tx(vap, m);
	}

	error = rtw_pci_tx_write_data(sc, queue, &pkt_info, m, ni);
	if (error != 0) {
		sc->sc_tx_err++;
		sc->sc_tx_err_map++;
		sc->sc_tx_err_map_last = error;
		m_freem(m);
	}
	return (error);
}

/*
 * Send what waits in sc_snd while the rings have room (rtwn_start()).
 * The frames keep their order: a frame whose ring is full stops the loop.
 */
static void
rtwb_start(struct rtwb_softc *sc)
{
	struct ieee80211_node *ni;
	struct mbuf *m;
	int error;

	RTWB_LOCK_ASSERT(sc);
	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		error = rtwb_tx_start(sc, ni, m);
		if (error == EAGAIN) {
			mbufq_prepend(&sc->sc_snd, m);
			break;
		}
		if (error != 0) {
			/* rtwb_tx_start() consumed m; the node ref is ours */
			if_inc_counter(ni->ni_vap->iv_ifp, IFCOUNTER_OERRORS,
			    1);
			ieee80211_free_node(ni);
		}
	}
}

/* Called by the TX-done interrupt path with the lock held. */
void
rtwb_tx_ring_drained(struct rtwb_softc *sc)
{
	if (sc->sc_running && mbufq_len(&sc->sc_snd) > 0)
		rtwb_start(sc);
}

static void
rtwb_drain_snd(struct rtwb_softc *sc)
{
	struct ieee80211_node *ni;
	struct mbuf *m;

	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m_freem(m);
		ieee80211_free_node(ni);
	}
}

static int
rtwb_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	struct rtwb_softc *sc = ic->ic_softc;
	int error;

	RTWB_LOCK(sc);
	if (!sc->sc_running) {
		RTWB_UNLOCK(sc);
		return (ENXIO);		/* net80211 frees m and ni */
	}
	error = mbufq_enqueue(&sc->sc_snd, m);
	if (error != 0) {
		sc->sc_tx_qfull++;
		RTWB_UNLOCK(sc);
		return (error);		/* net80211 frees m and ni */
	}
	if (mbufq_len(&sc->sc_snd) > 1)
		sc->sc_tx_queued++;
	rtwb_start(sc);
	RTWB_UNLOCK(sc);
	return (0);
}

static int
rtwb_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params)
{
	struct rtwb_softc *sc = ni->ni_ic->ic_softc;
	int error;

	RTWB_LOCK(sc);
	if (!sc->sc_running) {
		RTWB_UNLOCK(sc);
		m_freem(m);		/* the driver frees m, net80211 ni */
		return (ENETDOWN);
	}
	/* The bpf parameters (rate, retries) are not honoured yet. */
	error = rtwb_tx_start(sc, ni, m);
	RTWB_UNLOCK(sc);
	if (error == EAGAIN) {
		/* ring full: m was not consumed */
		m_freem(m);
		error = ENOBUFS;
	}
	return (error);
}

/*
 * Station link state into the hardware: rtw_vif_port_config() for port 0
 * (BSSID, AID, network type), the BSSID receive filter, and the firmware
 * media status for MAC ID 0.
 */
static void
rtwb_set_link(struct rtwb_softc *sc, struct ieee80211_node *ni)
{
	static const uint8_t zero[IEEE80211_ADDR_LEN];
	uint32_t cr;

	RTWB_LOCK_ASSERT(sc);
	if (!sc->sc_running)
		return;

	sc->sc_assoc = ni != NULL;
	rtwb_write_addr(sc, REG_PORT0_BSSID, ni != NULL ? ni->ni_bssid : zero);
	rtwb_write_2(sc, REG_PORT0_AID,
	    (rtwb_read_2(sc, REG_PORT0_AID) & ~0x7ff) |
	    (ni != NULL ? IEEE80211_AID(ni->ni_associd) & 0x7ff : 0));
	cr = rtwb_read_4(sc, REG_PORT0_NETTYPE) & ~PORT0_NETTYPE_MASK;
	cr |= (ni != NULL ? RTW_NET_MGD_LINKED : RTW_NET_NO_LINK) <<
	    PORT0_NETTYPE_SHIFT;
	rtwb_write_4(sc, REG_PORT0_NETTYPE, cr);
	rtwb_update_rcr(sc);
	rtw_fw_media_status_report(sc, 0, ni != NULL);

	/* rtw_sta_add() -> rtw_update_sta_info(): start the firmware RA */
	if (ni != NULL) {
		rtw_update_sta_info_80211(sc, ni, &sc->sc_sta, true);
		if (bootverbose || sc->sc_debug)
			device_printf(sc->sc_dev, "RA: mac_id %u rate_id %u "
			    "bw %u sgi %d vht %d ht %d mask %#jx (ni_chw %d, "
			    "radio bw %u center %u)\n", sc->sc_sta.mac_id,
			    sc->sc_sta.rate_id, sc->sc_sta.bw_mode,
			    sc->sc_sta.sgi_enable, sc->sc_sta.vht_enable,
			    sc->sc_sta.ht_enable, (uintmax_t)sc->sc_sta.ra_mask,
			    ni->ni_chw, sc->hal.current_band_width,
			    sc->hal.current_channel);
	}
}

static int
rtwb_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct rtwb_vap *uvp = RTWB_VAP(vap);
	struct ieee80211com *ic = vap->iv_ic;
	struct rtwb_softc *sc = ic->ic_softc;
	enum ieee80211_state ostate = vap->iv_state;

	if (vap->iv_opmode == IEEE80211_M_STA &&
	    (ostate == IEEE80211_S_RUN || nstate == IEEE80211_S_RUN)) {
		IEEE80211_UNLOCK(ic);
		RTWB_LOCK(sc);
		if (nstate == IEEE80211_S_RUN)
			rtwb_set_link(sc, vap->iv_bss);
		else
			rtwb_set_link(sc, NULL);
		RTWB_UNLOCK(sc);
		IEEE80211_LOCK(ic);
	}

	return (uvp->newstate(vap, nstate, arg));
}

static struct ieee80211vap *
rtwb_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ], int unit,
    enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct rtwb_softc *sc = ic->ic_softc;
	struct rtwb_vap *uvp;
	struct ieee80211vap *vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))		/* only one at a time */
		return (NULL);
	if (opmode != IEEE80211_M_STA && opmode != IEEE80211_M_MONITOR)
		return (NULL);

	uvp = malloc(sizeof(*uvp), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &uvp->vap;
	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
	    flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		free(uvp, M_80211_VAP);
		return (NULL);
	}

	/* override state transition machine */
	uvp->newstate = vap->iv_newstate;
	vap->iv_newstate = rtwb_newstate;

	/* 802.11n parameters (chip->ampdu_density, 64K A-MPDU) */
	vap->iv_ampdu_density = IEEE80211_HTCAP_MPDUDENSITY_2;
	vap->iv_ampdu_rxmax = IEEE80211_HTCAP_MAXRXAMPDU_64K;
	vap->iv_ampdu_limit = IEEE80211_HTCAP_MAXRXAMPDU_64K;

	ieee80211_ratectl_init(vap);
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	ic->ic_opmode = opmode;

	RTWB_LOCK(sc);
	if (sc->sc_running)
		rtwb_write_addr(sc, REG_PORT0_MACADDR, vap->iv_myaddr);
	rtwb_update_rcr(sc);
	RTWB_UNLOCK(sc);

	return (vap);
}

static void
rtwb_vap_delete(struct ieee80211vap *vap)
{
	struct rtwb_vap *uvp = RTWB_VAP(vap);

	ieee80211_ratectl_deinit(vap);
	ieee80211_vap_detach(vap);
	free(uvp, M_80211_VAP);
}

static void
rtwb_ic_attach(struct rtwb_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(sc->sc_dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_caps =
	      IEEE80211_C_STA		/* station mode */
	    | IEEE80211_C_MONITOR	/* monitor mode */
	    | IEEE80211_C_SHPREAMBLE	/* short preamble supported */
	    | IEEE80211_C_SHSLOT	/* short slot time supported */
	    | IEEE80211_C_WPA		/* 802.11i, software crypto */
	    ;
	/* rtw_init_ht_cap(); no TX STBC on the 8822B, no TX aggregation yet */
	ic->ic_htcaps =
	      IEEE80211_HTC_HT			/* HT operation */
	    | IEEE80211_HTC_RX_AMSDU_AMPDU	/* A-MSDU in A-MPDU */
	    | IEEE80211_HTCAP_SHORTGI20		/* short GI in 20MHz */
	    | IEEE80211_HTCAP_MAXAMSDU_3839	/* max A-MSDU length */
	    | IEEE80211_HTCAP_SMPS_OFF		/* SM PS mode disabled */
	    | IEEE80211_HTCAP_RXSTBC_1STREAM	/* 1 RX STBC stream */
	    | IEEE80211_HTCAP_LDPC		/* LDPC RX (chip->rx_ldpc) */
	    ;
	if (sc->sc_hw_cap_bw & BIT(RTW_CHANNEL_WIDTH_40))
		ic->ic_htcaps |= IEEE80211_HTCAP_CHWIDTH40 |
		    IEEE80211_HTCAP_SHORTGI40 | IEEE80211_HTCAP_DSSSCCK40;
	ic->ic_txstream = sc->sc_hw_cap_nss;
	ic->ic_rxstream = sc->sc_hw_cap_nss;

	/*
	 * rtw_init_vht_cap(), when the HW feature report allows VHT.  The
	 * beamformee bits are left out: sounding is not ported yet.
	 */
	if (sc->sc_hw_cap_ptcl == EFUSE_HW_CAP_IGNORE ||
	    sc->sc_hw_cap_ptcl == EFUSE_HW_CAP_PTCL_VHT) {
		uint32_t mcs_map = 0;
		int i;

		ic->ic_flags_ext |= IEEE80211_FEXT_VHT;
		ic->ic_vht_cap.vht_cap_info =
		    IEEE80211_VHTCAP_MAX_MPDU_LENGTH_11454 |
		    IEEE80211_VHTCAP_SHORT_GI_80 |
		    IEEE80211_VHTCAP_RXSTBC_1 |
		    IEEE80211_VHTCAP_HTC_VHT |
		    IEEE80211_VHTCAP_RXLDPC |
		    _IEEE80211_SHIFTMASK(7,
		      IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK);
		if (sc->hal.rf_path_num > 1)
			ic->ic_vht_cap.vht_cap_info |= IEEE80211_VHTCAP_TXSTBC;
		for (i = 0; i < 8; i++)
			mcs_map |= (i < sc->sc_hw_cap_nss ?
			    IEEE80211_VHT_MCS_SUPPORT_0_9 :
			    IEEE80211_VHT_MCS_NOT_SUPPORTED) << (i * 2);
		ic->ic_vht_cap.supp_mcs.rx_mcs_map = mcs_map;
		ic->ic_vht_cap.supp_mcs.tx_mcs_map = mcs_map;
		ic->ic_vht_cap.supp_mcs.rx_highest = 0;
		ic->ic_vht_cap.supp_mcs.tx_highest = 0;
	}
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->efuse.addr);

	rtwb_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
	    ic->ic_channels);

	ieee80211_ifattach(ic);
	ic->ic_raw_xmit = rtwb_raw_xmit;
	ic->ic_transmit = rtwb_transmit;
	ic->ic_parent = rtwb_parent;
	ic->ic_scan_start = rtwb_scan_start;
	ic->ic_scan_end = rtwb_scan_end;
	ic->ic_set_channel = rtwb_set_channel;
	ic->ic_update_chw = rtwb_update_chw;
	ic->ic_getradiocaps = rtwb_getradiocaps;
	ic->ic_vap_create = rtwb_vap_create;
	ic->ic_vap_delete = rtwb_vap_delete;
	ic->ic_update_promisc = rtwb_update_promisc;
	ic->ic_update_mcast = rtwb_update_mcast;

	ieee80211_set_software_ciphers(ic, IEEE80211_CRYPTO_WEP |
	    IEEE80211_CRYPTO_TKIP | IEEE80211_CRYPTO_AES_CCM |
	    IEEE80211_CRYPTO_AES_GCM_128);

	ieee80211_radiotap_attach(ic,
	    &sc->sc_txtap.wt_ihdr, sizeof(sc->sc_txtap),
	    RTWB_TX_RADIOTAP_PRESENT,
	    &sc->sc_rxtap.wr_ihdr, sizeof(sc->sc_rxtap),
	    RTWB_RX_RADIOTAP_PRESENT);

	if (bootverbose)
		ieee80211_announce(ic);
}

/* --- sysctl --- */

static int
rtwb_sysctl_channel(SYSCTL_HANDLER_ARGS)
{
	struct rtwb_softc *sc = arg1;
	int chan, error;

	chan = sc->hal.current_channel;
	error = sysctl_handle_int(oidp, &chan, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (chan < 1 || chan > 177)
		return (EINVAL);
	RTWB_LOCK(sc);
	if (sc->sc_running)
		error = rtwb_set_chan(sc, chan);
	else
		error = ENETDOWN;
	RTWB_UNLOCK(sc);
	return (error);
}

/* Debugging aid: read/write the 32-bit register at dev.rtwb.N.reg_addr. */
static int
rtwb_sysctl_reg_val(SYSCTL_HANDLER_ARGS)
{
	struct rtwb_softc *sc = arg1;
	uint32_t val;
	int error;

	if (sc->sc_reg_addr > 0xfffc || (sc->sc_reg_addr & 3) != 0)
		return (EINVAL);
	RTWB_LOCK(sc);
	if (!sc->sc_powered) {
		RTWB_UNLOCK(sc);
		return (ENETDOWN);
	}
	val = rtwb_read_4(sc, sc->sc_reg_addr);
	RTWB_UNLOCK(sc);
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	RTWB_LOCK(sc);
	if (sc->sc_powered)
		rtwb_write_4(sc, sc->sc_reg_addr, val);
	RTWB_UNLOCK(sc);
	return (0);
}

/* The per-rate TX power indices last programmed (hal.tx_pwr_tbl). */
static int
rtwb_sysctl_txpwr(SYSCTL_HANDLER_ARGS)
{
	struct rtwb_softc *sc = arg1;
	struct sbuf *sb;
	int error, path, rate;

	sb = sbuf_new_for_sysctl(NULL, NULL, 512, req);
	RTWB_LOCK(sc);
	for (path = 0; path < sc->hal.rf_path_num; path++) {
		sbuf_printf(sb, "\npath %c:", 'A' + path);
		/* CCK, OFDM, HT MCS0-15, VHT 1SS/2SS MCS0-9 */
		for (rate = 0; rate <= 0x3f; rate++)
			sbuf_printf(sb, " %02x",
			    sc->hal.tx_pwr_tbl[path][rate]);
	}
	RTWB_UNLOCK(sc);
	error = sbuf_finish(sb);
	sbuf_delete(sb);
	return (error);
}

static void
rtwb_sysctl_attach(struct rtwb_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->sc_dev);
	struct sysctl_oid_list *child =
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->sc_dev));

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "intr", CTLFLAG_RD,
	    &sc->sc_intr_count, 0, "interrupts handled");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_frames", CTLFLAG_RD,
	    &sc->sc_rx_frames, 0, "802.11 frames received");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_beacons", CTLFLAG_RD,
	    &sc->sc_rx_beacons, 0, "beacons received");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_crc_err", CTLFLAG_RD,
	    &sc->sc_rx_crc_err, 0, "frames with CRC/ICV errors");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_frames", CTLFLAG_RD,
	    &sc->sc_tx_frames, 0, "frames queued for transmission");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_done", CTLFLAG_RD,
	    &sc->sc_tx_done, 0, "frames completed by the hardware");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_err", CTLFLAG_RD,
	    &sc->sc_tx_err, 0, "frames dropped by the driver");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_err_encap", CTLFLAG_RD,
	    &sc->sc_tx_err_encap, 0, "TX drops: crypto encapsulation");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_err_mbuf", CTLFLAG_RD,
	    &sc->sc_tx_err_mbuf, 0, "TX drops: no mbuf to linearize");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_err_map", CTLFLAG_RD,
	    &sc->sc_tx_err_map, 0, "TX drops: DMA mapping");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "tx_err_map_last", CTLFLAG_RD,
	    &sc->sc_tx_err_map_last, 0,
	    "errno of the last DMA mapping failure");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_qfull", CTLFLAG_RD,
	    &sc->sc_tx_qfull, 0,
	    "frames dropped because the send queue was full");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_queued", CTLFLAG_RD,
	    &sc->sc_tx_queued, 0, "frames that waited for a free TX slot");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_c2h", CTLFLAG_RD,
	    &sc->sc_rx_c2h, 0, "C2H packets from the firmware");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_tag_err", CTLFLAG_RD,
	    &sc->sc_rx_tag_err, 0, "RX DMA tag mismatches");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "h2c_sent", CTLFLAG_RD,
	    &sc->sc_h2c_sent, 0, "H2C packets queued");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "h2c_done", CTLFLAG_RD,
	    &sc->sc_h2c_done, 0, "H2C packets completed by the hardware");
	SYSCTL_ADD_U8(ctx, child, OID_AUTO, "last_c2h_id", CTLFLAG_RD,
	    &sc->sc_last_c2h_id, 0, "id of the last C2H packet");
	SYSCTL_ADD_U8(ctx, child, OID_AUTO, "ra_rate", CTLFLAG_RD,
	    &sc->sc_ra_rate, 0, "rate chosen by the firmware RA (DESC_RATE)");
	SYSCTL_ADD_U8(ctx, child, OID_AUTO, "ra_bw", CTLFLAG_RD,
	    &sc->sc_ra_bw, 0, "bandwidth of the firmware RA rate");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "ra_reports", CTLFLAG_RD,
	    &sc->sc_ra_reports, 0, "firmware RA reports received");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "debug", CTLFLAG_RW,
	    &sc->sc_debug, 0, "print link details");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "tx_report", CTLFLAG_RW,
	    &sc->sc_tx_report, 0, "debug: ask the firmware for TX reports");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_rpt_ok", CTLFLAG_RD,
	    &sc->sc_tx_rpt_ok, 0, "TX reports with status ok");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_rpt_fail", CTLFLAG_RD,
	    &sc->sc_tx_rpt_fail, 0, "TX reports with a failure status");
	SYSCTL_ADD_U32(ctx, child, OID_AUTO, "reg_addr", CTLFLAG_RW,
	    &sc->sc_reg_addr, 0, "register address for reg_val");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "reg_val",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    rtwb_sysctl_reg_val, "IU", "32-bit register at reg_addr");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "txpwr",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    rtwb_sysctl_txpwr, "A", "TX power index per rate (0x00-0x3f)");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "channel",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    rtwb_sysctl_channel, "I", "current channel (20 MHz)");
}

/* --- device methods --- */

static int
rtwb_attach(device_t dev)
{
	struct rtwb_softc *sc = device_get_softc(dev);
	int error;

	sc->sc_dev = dev;
	sx_init(&sc->sc_sx, "rtwb init");
	mtx_init(&sc->sc_mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	mbufq_init(&sc->sc_rxq, RTWB_RX_RING_LEN);
	mbufq_init(&sc->sc_snd, RTWB_SND_QUEUE_LEN);

	/* No DMA until the rings are set up. */
	pci_disable_busmaster(dev);

	sc->sc_mem_rid = PCIR_BAR(2);
	sc->sc_mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->sc_mem_rid, RF_ACTIVE);
	if (sc->sc_mem == NULL) {
		device_printf(dev, "could not map BAR2\n");
		error = ENXIO;
		goto fail;
	}

	rtwb_read_chip_info(sc);

	error = rtwb_dma_alloc(sc, &sc->sc_bcn_ring, RTWB_TX_BUF_DESC_SZ,
	    PAGE_SIZE);
	if (error != 0)
		goto fail;
	error = rtwb_dma_alloc(sc, &sc->sc_bcn_buf,
	    RTWB_TX_PKT_DESC_SZ + RTWB_FW_CHUNK_SIZE, PAGE_SIZE);
	if (error != 0)
		goto fail;
	error = rtwb_alloc_rings(sc);
	if (error != 0) {
		device_printf(dev, "could not allocate rings (%d)\n", error);
		goto fail;
	}

	error = rtwb_get_firmware(sc);
	if (error != 0)
		goto fail;
	error = rtwb_read_hw_info(sc);
	if (error != 0)
		goto fail;

	rtwb_sysctl_attach(sc);
	error = rtwb_setup_intr(sc);
	if (error != 0)
		goto fail;

	rtwb_ic_attach(sc);
	sc->sc_ic_attached = true;
	return (0);

fail:
	rtwb_detach(dev);
	return (error);
}

static int
rtwb_detach(device_t dev)
{
	struct rtwb_softc *sc = device_get_softc(dev);

	/* Stops every vap, which brings the hardware down (ic_parent). */
	if (sc->sc_ic_attached) {
		ieee80211_ifdetach(&sc->sc_ic);
		sc->sc_ic_attached = false;
	}

	if (sc->sc_mem != NULL) {
		RTWB_SX_LOCK(sc);
		RTWB_LOCK(sc);
		rtwb_hw_stop(sc);
		RTWB_UNLOCK(sc);
		RTWB_SX_UNLOCK(sc);
	}
	rtwb_teardown_intr(sc);

	rtwb_free_rings(sc);
	rtwb_dma_free(&sc->sc_bcn_buf);
	rtwb_dma_free(&sc->sc_bcn_ring);
	if (sc->sc_fw != NULL) {
		firmware_put(sc->sc_fw, FIRMWARE_UNLOAD);
		sc->sc_fw = NULL;
	}

	if (sc->sc_mem != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_mem_rid,
		    sc->sc_mem);
		sc->sc_mem = NULL;
	}
	if (mtx_initialized(&sc->sc_mtx))
		mtx_destroy(&sc->sc_mtx);
	if (lock_initialized(&sc->sc_sx.lock_object))
		sx_destroy(&sc->sc_sx);
	return (0);
}

static device_method_t rtwb_methods[] = {
	DEVMETHOD(device_probe,		rtwb_probe),
	DEVMETHOD(device_attach,	rtwb_attach),
	DEVMETHOD(device_detach,	rtwb_detach),

	DEVMETHOD_END
};

static driver_t rtwb_driver = {
	"rtwb",
	rtwb_methods,
	sizeof(struct rtwb_softc)
};

/*
 * No MODULE_PNP_INFO on purpose: the base system's if_rtw88 claims the
 * same device, and devmatch would load both at boot.  Users choose rtwb
 * with kld_list and keep if_rtw88 out with devmatch_blocklist.
 */
DRIVER_MODULE(rtwb, pci, rtwb_driver, NULL, NULL);
MODULE_VERSION(rtwb, 1);
MODULE_DEPEND(rtwb, pci, 1, 1, 1);
MODULE_DEPEND(rtwb, wlan, 1, 1, 1);
MODULE_DEPEND(rtwb, firmware, 1, 1, 1);
