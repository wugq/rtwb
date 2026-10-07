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

#ifndef _IF_RTWBVAR_H_
#define _IF_RTWBVAR_H_

#include <sys/mbuf.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include "port/rtw88_port.h"

/* One coherent DMA buffer below 4 GB. */
struct rtwb_dma {
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	void			*vaddr;
	bus_addr_t		paddr;
	bus_size_t		size;
};

/* Largest firmware chunk sent through the reserved page (rtw88 mac.c). */
#define RTWB_FW_CHUNK_SIZE	0x1000

/* Hardware sizes (rtw88 rtw8822b.c hw spec, pci.h, fw.h). */
#define RTWB_TX_PKT_DESC_SZ	48
#define RTWB_TX_BUF_DESC_SZ	16
#define RTWB_RX_PKT_DESC_SZ	24
#define RTWB_RX_BUF_DESC_SZ	8
#define RTWB_RX_BUF_SIZE	(11454 + 24)	/* RTK_PCI_RX_BUF_SIZE */
#define RTWB_RX_RING_LEN	512		/* RTK_MAX_RX_DESC_NUM */
#define RTWB_H2C_PKT_SIZE	32
#define RTWB_TX_MAX_SIZE	MJUM9BYTES	/* one payload segment */
#define RTWB_SND_QUEUE_LEN	1024		/* sc_snd, behind the rings */

/* TX queues, in rtw88's enum rtw_tx_queue_type order. */
enum rtwb_txq {
	RTWB_TXQ_BK = 0,
	RTWB_TXQ_BE,
	RTWB_TXQ_VI,
	RTWB_TXQ_VO,
	RTWB_TXQ_BCN,		/* reserved page; uses sc_bcn_ring/buf */
	RTWB_TXQ_MGMT,
	RTWB_TXQ_HI0,
	RTWB_TXQ_H2C,
	RTWB_NTXQ
};

/* An mbuf in flight on a TX ring. */
struct rtwb_tx_slot {
	struct mbuf		*m;
	struct ieee80211_node	*ni;
	bus_dmamap_t		map;
};

/*
 * A TX buffer-descriptor ring.  'buf' holds one fixed-size area per slot:
 * the TX packet descriptor (data/management queues, whose payload is a
 * mapped mbuf) or descriptor plus payload (H2C, copied in).
 */
struct rtwb_tx_ring {
	struct rtwb_dma		desc;
	struct rtwb_dma		buf;
	struct rtwb_tx_slot	*slot;	/* NULL for H2C */
	uint32_t		nmaps;	/* slot maps created */
	uint32_t		slot_size;
	uint32_t		len;
	uint32_t		wp;	/* next slot the host fills */
	uint32_t		rp;	/* next slot the hardware completes */
};

/* RX buffer of one RX buffer-descriptor slot. */
struct rtwb_rx_slot {
	bus_dmamap_t		map;
	void			*vaddr;
	bus_addr_t		paddr;
};

struct rtwb_rx_ring {
	struct rtwb_dma		desc;
	bus_dma_tag_t		buf_tag;
	struct rtwb_rx_slot	*slot;
	uint32_t		len;
	uint32_t		rp;	/* next slot the host reads */
	uint16_t		rx_tag;	/* expected total_pkt_size tag */
};

struct rtwb_rx_radiotap_header {
	struct ieee80211_radiotap_header wr_ihdr;
	uint64_t	wr_tsft;
	uint8_t		wr_flags;
	uint8_t		wr_rate;
	uint16_t	wr_chan_freq;
	uint16_t	wr_chan_flags;
	int8_t		wr_dbm_antsignal;
	int8_t		wr_dbm_antnoise;
} __packed __aligned(8);

#define RTWB_RX_RADIOTAP_PRESENT			\
	(1 << IEEE80211_RADIOTAP_TSFT |			\
	 1 << IEEE80211_RADIOTAP_FLAGS |		\
	 1 << IEEE80211_RADIOTAP_RATE |			\
	 1 << IEEE80211_RADIOTAP_CHANNEL |		\
	 1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL |	\
	 1 << IEEE80211_RADIOTAP_DBM_ANTNOISE)

struct rtwb_tx_radiotap_header {
	struct ieee80211_radiotap_header wt_ihdr;
	uint8_t		wt_flags;
	uint8_t		wt_pad;
	uint16_t	wt_chan_freq;
	uint16_t	wt_chan_flags;
} __packed;

#define RTWB_TX_RADIOTAP_PRESENT			\
	(1 << IEEE80211_RADIOTAP_FLAGS |		\
	 1 << IEEE80211_RADIOTAP_CHANNEL)

struct rtwb_vap {
	struct ieee80211vap	vap;
	int			(*newstate)(struct ieee80211vap *,
				    enum ieee80211_state, int);
};
#define RTWB_VAP(vap)		((struct rtwb_vap *)(vap))

/* Noise floor reported to net80211 (dBm); the chip does not measure it. */
#define RTWB_NOISE_FLOOR	-95

struct rtwb_softc {
	struct ieee80211com	sc_ic;
	device_t		sc_dev;
	struct mtx		sc_mtx;
	const struct firmware	*sc_fw;
	struct mbufq		sc_rxq;		/* frames for net80211 */
	struct mbufq		sc_snd;		/* data frames waiting for a slot */
	uint32_t		sc_rcr;		/* REG_RCR value (hal.rcr) */
	struct rtwb_rx_radiotap_header sc_rxtap;
	struct rtwb_tx_radiotap_header sc_txtap;

	/* BAR2: memory-mapped chip registers (bar_id = 2 in rtw88 pci.c). */
	struct resource		*sc_mem;
	int			sc_mem_rid;

	/*
	 * Hardware state with rtw88's names (struct rtw_hal, rtw_efuse), so
	 * the code in port/ can stay close to the original.
	 */
	struct rtw_hal		hal;
	struct rtw_efuse	efuse;
	bool			mp_mode;	/* always false */
	struct rtw_fifo_conf	sc_fifo;

	/* HW feature report from the firmware (rtw_dump_hw_feature). */
	uint8_t			sc_hw_cap_bw;	/* BIT(RTW_CHANNEL_WIDTH_x) */
	uint8_t			sc_hw_cap_nss;
	uint8_t			sc_hw_cap_ptcl;
	uint8_t			sc_hw_cap_ant_num;

	/* Interrupt. */
	struct resource		*sc_irq;
	int			sc_irq_rid;
	void			*sc_ih;
	bool			sc_msi;
	bool			sc_running;	/* interrupts may be re-enabled */
	bool			sc_powered;	/* MAC power-on sequence done */
	bool			sc_ic_attached;
	bool			sc_assoc;	/* station vap in RUN */
	struct rtw_sta_info	sc_sta;		/* the AP, for the firmware RA */
	uint8_t			sc_ra_rate;	/* last RA report: DESC_RATE* */
	uint8_t			sc_ra_sgi;
	uint8_t			sc_ra_bw;
	uint64_t		sc_ra_reports;
	uint32_t		sc_reg_addr;	/* debug sysctl */
	int			sc_tx_report;	/* debug: request CCX TX reports */
	int			sc_debug;
	uint8_t			sc_tx_report_sn;
	uint64_t		sc_tx_rpt_ok;
	uint64_t		sc_tx_rpt_fail;
	uint32_t		sc_irq_mask[4];

	struct rtwb_tx_ring	sc_tx_ring[RTWB_NTXQ];
	bus_dma_tag_t		sc_tx_mbuf_tag;
	struct rtwb_rx_ring	sc_rx_ring;
	uint16_t		sc_h2c_seq;
	uint8_t			sc_h2c_last_box;	/* H2C mailbox 0..3 */

	/* Counters, exported with sysctl. */
	uint64_t		sc_intr_count;
	uint64_t		sc_rx_frames;
	uint64_t		sc_rx_c2h;
	uint64_t		sc_rx_tag_err;
	uint64_t		sc_h2c_sent;
	uint64_t		sc_h2c_done;
	uint8_t			sc_last_c2h_id;
	uint64_t		sc_rx_beacons;
	uint64_t		sc_rx_crc_err;
	uint64_t		sc_tx_frames;
	uint64_t		sc_tx_done;
	uint64_t		sc_tx_err;
	uint64_t		sc_tx_queued;	/* had to wait in sc_snd */
	uint64_t		sc_tx_qfull;	/* sc_snd full, dropped */
	uint64_t		sc_tx_err_encap;
	uint64_t		sc_tx_err_mbuf;
	uint64_t		sc_tx_err_map;
	int			sc_tx_err_map_last;	/* errno */

	/*
	 * Beacon queue: a one-entry TX buffer-descriptor ring and one
	 * buffer (TX packet descriptor + payload).  rtw88 uses it to write
	 * the reserved page, e.g. for the firmware download.
	 */
	struct rtwb_dma		sc_bcn_ring;
	struct rtwb_dma		sc_bcn_buf;

	/* Firmware image info, from its header. */
	uint16_t		sc_fw_version;
	uint8_t			sc_fw_subversion;
	uint8_t			sc_fw_subindex;
	uint16_t		sc_fw_h2c_version;
};

#define RTWB_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define RTWB_UNLOCK(sc)		mtx_unlock(&(sc)->sc_mtx)
#define RTWB_LOCK_ASSERT(sc)	mtx_assert(&(sc)->sc_mtx, MA_OWNED)

/* rtwb_dma.c */
int	rtwb_dma_alloc(struct rtwb_softc *, struct rtwb_dma *, bus_size_t,
	    bus_size_t);
void	rtwb_dma_free(struct rtwb_dma *);
int	rtwb_alloc_rings(struct rtwb_softc *);
void	rtwb_free_rings(struct rtwb_softc *);

/* Register access. The chip is little-endian; amd64 bus_space does no swapping. */
#define rtwb_read_1(sc, reg)	bus_read_1((sc)->sc_mem, (reg))
#define rtwb_read_2(sc, reg)	bus_read_2((sc)->sc_mem, (reg))
#define rtwb_read_4(sc, reg)	bus_read_4((sc)->sc_mem, (reg))
#define rtwb_write_1(sc, reg, v) bus_write_1((sc)->sc_mem, (reg), (v))
#define rtwb_write_2(sc, reg, v) bus_write_2((sc)->sc_mem, (reg), (v))
#define rtwb_write_4(sc, reg, v) bus_write_4((sc)->sc_mem, (reg), (v))

/* Read-modify-write helpers: clear 'clr' bits, then set 'set' bits. */
static inline void
rtwb_setbits_1(struct rtwb_softc *sc, uint16_t reg, uint8_t clr, uint8_t set)
{
	rtwb_write_1(sc, reg, (rtwb_read_1(sc, reg) & ~clr) | set);
}

static inline void
rtwb_setbits_4(struct rtwb_softc *sc, uint16_t reg, uint32_t clr,
    uint32_t set)
{
	rtwb_write_4(sc, reg, (rtwb_read_4(sc, reg) & ~clr) | set);
}

#endif /* _IF_RTWBVAR_H_ */
