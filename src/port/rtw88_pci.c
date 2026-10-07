/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 pci.c (buffer descriptor rings, reserved page
 * and H2C writes, interrupt status handling, RX ring processing), tx.c
 * (TX packet descriptor fill) and rx.c (RX descriptor parsing).
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * Changes: busdma instead of the Linux DMA API.  The beacon queue uses one
 * preallocated buffer (sc_bcn_buf), so a reserved-page write must not be
 * started before the previous one has reached the reserved page (the
 * caller checks BIT_BCN_VALID_V1, as rtw_fw_write_data_rsvd_page() does).
 * H2C packets are copied into fixed per-slot buffers.  RX frames are
 * copied out of fixed per-slot buffers, as rtw88 does with its skbs, and
 * handed to rtwb_rx_frame()/rtwb_rx_c2h().  The interrupt handler runs in
 * an ithread with HIMR masked, replacing rtw88's hard-irq/thread split.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include "../if_rtwbvar.h"
#include "rtw88_reg.h"
#include "rtw88_port.h"

/* struct rtw_pci_tx_buffer_desc */
struct rtw_pci_tx_buffer_desc {
	uint16_t	buf_size;	/* le16 */
	uint16_t	psb_len;	/* le16 */
	uint32_t	dma;		/* le32 */
} __packed;

/* struct rtw_pci_rx_buffer_desc */
struct rtw_pci_rx_buffer_desc {
	uint16_t	buf_size;	/* le16 */
	uint16_t	total_pkt_size;	/* le16 */
	uint32_t	dma;		/* le32 */
} __packed;

#define TRX_BD_IDX_MASK		0xfff		/* GENMASK(11, 0) */
#define TRX_BD_HW_IDX_SHIFT	16		/* GENMASK(27, 16) */
#define RX_TAG_MAX		8192

/* Per-queue registers: descriptor base, ring length, host/hw index. */
static const struct {
	uint16_t	desa;
	uint16_t	num;
	uint16_t	idx;
} rtw_pci_txq_regs[RTWB_NTXQ] = {
	[RTWB_TXQ_BK]	= { RTK_PCI_TXBD_DESA_BKQ, RTK_PCI_TXBD_NUM_BKQ,
			    RTK_PCI_TXBD_IDX_BKQ },
	[RTWB_TXQ_BE]	= { RTK_PCI_TXBD_DESA_BEQ, RTK_PCI_TXBD_NUM_BEQ,
			    RTK_PCI_TXBD_IDX_BEQ },
	[RTWB_TXQ_VI]	= { RTK_PCI_TXBD_DESA_VIQ, RTK_PCI_TXBD_NUM_VIQ,
			    RTK_PCI_TXBD_IDX_VIQ },
	[RTWB_TXQ_VO]	= { RTK_PCI_TXBD_DESA_VOQ, RTK_PCI_TXBD_NUM_VOQ,
			    RTK_PCI_TXBD_IDX_VOQ },
	[RTWB_TXQ_BCN]	= { RTK_PCI_TXBD_DESA_BCNQ, 0, 0 },
	[RTWB_TXQ_MGMT]	= { RTK_PCI_TXBD_DESA_MGMTQ, RTK_PCI_TXBD_NUM_MGMTQ,
			    RTK_PCI_TXBD_IDX_MGMTQ },
	[RTWB_TXQ_HI0]	= { RTK_PCI_TXBD_DESA_HI0Q, RTK_PCI_TXBD_NUM_HI0Q,
			    RTK_PCI_TXBD_IDX_HI0Q },
	[RTWB_TXQ_H2C]	= { RTK_PCI_TXBD_DESA_H2CQ, RTK_PCI_TXBD_NUM_H2CQ,
			    RTK_PCI_TXBD_IDX_H2CQ },
};

/* rtw_pci_reset_rx_desc() / rtw_pci_sync_rx_desc_device() */
static void
rtw_pci_reset_rx_desc(struct rtwb_softc *sc, uint32_t idx)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	struct rtw_pci_rx_buffer_desc *bd;

	bus_dmamap_sync(rx->buf_tag, rx->slot[idx].map, BUS_DMASYNC_PREREAD);
	bd = (struct rtw_pci_rx_buffer_desc *)rx->desc.vaddr + idx;
	memset(bd, 0, sizeof(*bd));
	bd->buf_size = htole16(RTWB_RX_BUF_SIZE);
	bd->dma = htole32((uint32_t)rx->slot[idx].paddr);
}

/* rtw_pci_reset_buf_desc() */
static void
rtw_pci_reset_buf_desc(struct rtwb_softc *sc)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	struct rtwb_tx_ring *tx;
	uint32_t i;
	int q;
	uint8_t tmp;

	tmp = rtwb_read_1(sc, RTK_PCI_CTRL + 3);
	rtwb_write_1(sc, RTK_PCI_CTRL + 3, tmp | 0xf7);

	rtwb_write_4(sc, RTK_PCI_TXBD_DESA_BCNQ,
	    (uint32_t)sc->sc_bcn_ring.paddr);

	for (q = 0; q < RTWB_NTXQ; q++) {
		if (q == RTWB_TXQ_BCN)
			continue;
		tx = &sc->sc_tx_ring[q];
		tx->rp = 0;
		tx->wp = 0;
		rtwb_write_2(sc, rtw_pci_txq_regs[q].num,
		    tx->len & TRX_BD_IDX_MASK);
		rtwb_write_4(sc, rtw_pci_txq_regs[q].desa,
		    (uint32_t)tx->desc.paddr);
	}

	/* Hand every RX buffer to the hardware. */
	for (i = 0; i < rx->len; i++)
		rtw_pci_reset_rx_desc(sc, i);
	bus_dmamap_sync(rx->desc.tag, rx->desc.map, BUS_DMASYNC_PREWRITE);
	rx->rp = 0;
	rtwb_write_2(sc, RTK_PCI_RXBD_NUM_MPDUQ, rx->len & TRX_BD_IDX_MASK);
	rtwb_write_4(sc, RTK_PCI_RXBD_DESA_MPDUQ, (uint32_t)rx->desc.paddr);

	/* reset read/write point */
	rtwb_write_4(sc, RTK_PCI_TXBD_RWPTR_CLR, 0xffffffff);

	/* reset H2C Queue index in a single write */
	rtwb_setbits_4(sc, RTK_PCI_TXBD_H2CQ_CSR, 0,
	    BIT_CLR_H2CQ_HOST_IDX | BIT_CLR_H2CQ_HW_IDX);
}

static void
rtw_pci_dma_reset(struct rtwb_softc *sc)
{
	/* reset dma and rx tag */
	rtwb_setbits_4(sc, RTK_PCI_CTRL, 0,
	    BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN);
	sc->sc_rx_ring.rx_tag = 0;
}

/* rtw_pci_setup(): point the hardware at our rings and reset DMA. */
void
rtw_pci_setup(struct rtwb_softc *sc)
{
	rtw_pci_reset_buf_desc(sc);
	rtw_pci_dma_reset(sc);
}

/* rtw_pci_init(): interrupt sources we handle. */
void
rtw_pci_init_irq_mask(struct rtwb_softc *sc)
{
	sc->sc_irq_mask[0] = IMR_HIGHDOK |
			     IMR_MGNTDOK |
			     IMR_BKDOK |
			     IMR_BEDOK |
			     IMR_VIDOK |
			     IMR_VODOK |
			     IMR_ROK |
			     IMR_BCNDMAINT_E |
			     IMR_C2HCMD;
	sc->sc_irq_mask[1] = IMR_TXFOVW;
	sc->sc_irq_mask[2] = 0;
	sc->sc_irq_mask[3] = IMR_H2CDOK;
}

void
rtw_pci_enable_interrupt(struct rtwb_softc *sc, bool exclude_rx)
{
	uint32_t imr0_unmask = exclude_rx ? IMR_ROK : 0;

	rtwb_write_4(sc, RTK_PCI_HIMR0, sc->sc_irq_mask[0] & ~imr0_unmask);
	rtwb_write_4(sc, RTK_PCI_HIMR1, sc->sc_irq_mask[1]);
	rtwb_write_4(sc, RTK_PCI_HIMR3, sc->sc_irq_mask[3]);
}

void
rtw_pci_disable_interrupt(struct rtwb_softc *sc)
{
	rtwb_write_4(sc, RTK_PCI_HIMR0, 0);
	rtwb_write_4(sc, RTK_PCI_HIMR1, 0);
	rtwb_write_4(sc, RTK_PCI_HIMR3, 0);
}

/*
 * rtw_tx_fill_tx_desc() for packets that only need a size, an offset and
 * a queue: reserved-page and H2C packets.  The descriptor checksum is
 * only used by USB/SDIO.
 */
static void
rtw_fill_tx_desc_simple(uint32_t *desc, uint32_t size, uint8_t offset,
    uint8_t qsel)
{
	memset(desc, 0, RTWB_TX_PKT_DESC_SZ);
	desc[0] = htole32((size << RTW_TX_DESC_W0_TXPKTSIZE_S) |
	    ((uint32_t)offset << RTW_TX_DESC_W0_OFFSET_S));
	desc[1] = htole32((uint32_t)qsel << RTW_TX_DESC_W1_QSEL_S);
}

/*
 * rtw_tx_rsvd_page_pkt_info_update(RSVD_BEACON) + rtw_tx_fill_tx_desc().
 * Before the first channel is set rtw88's current_band_type is 0, so it
 * takes the non-2G branch: RATEID_G at 6 Mbps.
 */
static void
rtw_fill_rsvd_page_tx_desc(uint32_t *desc, const uint8_t *pkt, uint32_t size)
{
	bool bmc;

	rtw_fill_tx_desc_simple(desc, size, RTWB_TX_PKT_DESC_SZ,
	    TX_DESC_QSEL_BEACON);

	/* addr1 of the 802.11 header: group bit of its first octet. */
	bmc = size >= 10 && (pkt[4] & 0x01) != 0;

	desc[0] |= htole32((bmc ? RTW_TX_DESC_W0_BMC : 0) |
	    RTW_TX_DESC_W0_LS | RTW_TX_DESC_W0_DISQSELSEQ);
	desc[1] |= htole32(RTW_RATEID_G << RTW_TX_DESC_W1_RATE_ID_S);
	desc[3] = htole32((0 << RTW_TX_DESC_W3_HW_SSN_SEL_S) |
	    RTW_TX_DESC_W3_USE_RATE | RTW_TX_DESC_W3_DISDATAFB);
	desc[4] = htole32(DESC_RATE6M << RTW_TX_DESC_W4_DATARATE_S);
	desc[8] = htole32(RTW_TX_DESC_W8_EN_HWSEQ);
}

/* Fill the two buffer descriptors of one TX slot (rtw_pci_tx_write_data). */
static void
rtw_pci_fill_tx_bd(struct rtw_pci_tx_buffer_desc *bd, bus_addr_t dma,
    uint32_t size, bool own)
{
	uint32_t len = RTWB_TX_PKT_DESC_SZ + size;
	uint32_t psb_len;

	memset(bd, 0, RTWB_TX_BUF_DESC_SZ);
	psb_len = (len - 1) / 128 + 1;
	if (own)
		psb_len |= 1 << RTK_PCI_TXBD_OWN_OFFSET;
	bd[0].psb_len = htole16(psb_len);
	bd[0].buf_size = htole16(RTWB_TX_PKT_DESC_SZ);
	bd[0].dma = htole32((uint32_t)dma);
	bd[1].buf_size = htole16(size);
	bd[1].dma = htole32((uint32_t)(dma + RTWB_TX_PKT_DESC_SZ));
}

/* rtw_pci_write_data_rsvd_page() + rtw_pci_tx_write_data(BCN queue). */
int
rtw_pci_write_data_rsvd_page(struct rtwb_softc *sc, const uint8_t *buf,
    uint32_t size)
{
	uint8_t *pkt;
	uint8_t reg_bcn_work;

	if (size == 0 || size > RTWB_FW_CHUNK_SIZE)
		return (EINVAL);

	/* TX packet descriptor followed by the data. */
	pkt = sc->sc_bcn_buf.vaddr;
	memcpy(pkt + RTWB_TX_PKT_DESC_SZ, buf, size);
	rtw_fill_rsvd_page_tx_desc((uint32_t *)pkt, buf, size);
	bus_dmamap_sync(sc->sc_bcn_buf.tag, sc->sc_bcn_buf.map,
	    BUS_DMASYNC_PREWRITE);

	rtw_pci_fill_tx_bd(sc->sc_bcn_ring.vaddr, sc->sc_bcn_buf.paddr, size,
	    true);
	bus_dmamap_sync(sc->sc_bcn_ring.tag, sc->sc_bcn_ring.map,
	    BUS_DMASYNC_PREWRITE);

	/* reserved pages go through beacon queue */
	reg_bcn_work = rtwb_read_1(sc, RTK_PCI_TXBD_BCN_WORK);
	reg_bcn_work |= BIT_PCI_BCNQ_FLAG;
	rtwb_write_1(sc, RTK_PCI_TXBD_BCN_WORK, reg_bcn_work);

	return (0);
}

/* avail_desc(): one slot stays empty to tell a full ring from an empty one */
static uint32_t
avail_desc(uint32_t wp, uint32_t rp, uint32_t len)
{
	if (rp > wp)
		return (rp - wp - 1);
	else
		return (len - wp + rp - 1);
}

/* rtw_pci_write_data_h2c() + rtw_pci_tx_write_data(H2C) + kick off. */
int
rtw_pci_write_data_h2c(struct rtwb_softc *sc, const uint8_t *buf,
    uint32_t size)
{
	struct rtwb_tx_ring *tx = &sc->sc_tx_ring[RTWB_TXQ_H2C];
	struct rtw_pci_tx_buffer_desc *bd;
	uint8_t *pkt;
	bus_addr_t dma;

	RTWB_LOCK_ASSERT(sc);

	if (size == 0 || size > RTWB_H2C_PKT_SIZE)
		return (EINVAL);
	if (avail_desc(tx->wp, tx->rp, tx->len) == 0)
		return (ENOSPC);

	pkt = (uint8_t *)tx->buf.vaddr + tx->wp * tx->slot_size;
	dma = tx->buf.paddr + tx->wp * tx->slot_size;
	memcpy(pkt + RTWB_TX_PKT_DESC_SZ, buf, size);
	/* pkt_info only carries the size here; offset stays 0 as in rtw88 */
	rtw_fill_tx_desc_simple((uint32_t *)pkt, size, 0, TX_DESC_QSEL_H2C);
	bus_dmamap_sync(tx->buf.tag, tx->buf.map, BUS_DMASYNC_PREWRITE);

	bd = (struct rtw_pci_tx_buffer_desc *)((uint8_t *)tx->desc.vaddr +
	    tx->wp * RTWB_TX_BUF_DESC_SZ);
	rtw_pci_fill_tx_bd(bd, dma, size, false);
	bus_dmamap_sync(tx->desc.tag, tx->desc.map, BUS_DMASYNC_PREWRITE);

	if (++tx->wp >= tx->len)
		tx->wp = 0;

	/* rtw_pci_tx_kick_off_queue() */
	rtwb_write_2(sc, rtw_pci_txq_regs[RTWB_TXQ_H2C].idx,
	    tx->wp & TRX_BD_IDX_MASK);
	sc->sc_h2c_sent++;

	return (0);
}

/*
 * rtw_pci_tx_write_data() for a data or management queue: the TX packet
 * descriptor goes into the slot's descriptor area, the frame itself is
 * the mapped mbuf (one segment), and the two buffer descriptors point at
 * them.  rtw88 instead pushes the descriptor in front of the skb data.
 * On success the slot owns 'm' and the node reference.
 */
int
rtw_pci_tx_write_data(struct rtwb_softc *sc, int q,
    struct rtw_tx_pkt_info *pkt_info, struct mbuf *m,
    struct ieee80211_node *ni)
{
	struct rtwb_tx_ring *tx = &sc->sc_tx_ring[q];
	struct rtwb_tx_slot *slot;
	struct rtw_pci_tx_buffer_desc *bd;
	bus_dma_segment_t seg;
	uint8_t *desc;
	bus_addr_t desc_dma;
	uint32_t len, psb_len;
	int error, nsegs;

	RTWB_LOCK_ASSERT(sc);
	KASSERT(tx->slot != NULL, ("TX queue %d has no mbuf slots", q));

	if (avail_desc(tx->wp, tx->rp, tx->len) == 0)
		return (ENOBUFS);

	slot = &tx->slot[tx->wp];
	error = bus_dmamap_load_mbuf_sg(sc->sc_tx_mbuf_tag, slot->map, m,
	    &seg, &nsegs, BUS_DMA_NOWAIT);
	if (error != 0)
		return (error);
	bus_dmamap_sync(sc->sc_tx_mbuf_tag, slot->map, BUS_DMASYNC_PREWRITE);

	desc = (uint8_t *)tx->buf.vaddr + tx->wp * tx->slot_size;
	desc_dma = tx->buf.paddr + tx->wp * tx->slot_size;
	memset(desc, 0, RTWB_TX_PKT_DESC_SZ);
	rtw_tx_fill_tx_desc(sc, pkt_info, desc);
	bus_dmamap_sync(tx->buf.tag, tx->buf.map, BUS_DMASYNC_PREWRITE);

	/* after this we got dma mapped, there is no way back */
	bd = (struct rtw_pci_tx_buffer_desc *)((uint8_t *)tx->desc.vaddr +
	    tx->wp * RTWB_TX_BUF_DESC_SZ);
	memset(bd, 0, RTWB_TX_BUF_DESC_SZ);
	len = RTWB_TX_PKT_DESC_SZ + seg.ds_len;
	psb_len = (len - 1) / 128 + 1;
	bd[0].psb_len = htole16(psb_len);
	bd[0].buf_size = htole16(RTWB_TX_PKT_DESC_SZ);
	bd[0].dma = htole32((uint32_t)desc_dma);
	bd[1].buf_size = htole16(seg.ds_len);
	bd[1].dma = htole32((uint32_t)seg.ds_addr);
	bus_dmamap_sync(tx->desc.tag, tx->desc.map, BUS_DMASYNC_PREWRITE);

	slot->m = m;
	slot->ni = ni;

	/* update write-index and kick it off (rtw_pci_tx_kick_off_queue) */
	if (++tx->wp >= tx->len)
		tx->wp = 0;
	rtwb_write_2(sc, rtw_pci_txq_regs[q].idx, tx->wp & TRX_BD_IDX_MASK);
	sc->sc_tx_frames++;

	return (0);
}

bool
rtw_pci_tx_ring_full(struct rtwb_softc *sc, int q)
{
	struct rtwb_tx_ring *tx = &sc->sc_tx_ring[q];

	return (avail_desc(tx->wp, tx->rp, tx->len) == 0);
}

/* Give a finished (or dropped) frame back to net80211. */
static void
rtw_pci_tx_slot_done(struct rtwb_softc *sc, struct rtwb_tx_slot *slot,
    int status)
{
	bus_dmamap_sync(sc->sc_tx_mbuf_tag, slot->map, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->sc_tx_mbuf_tag, slot->map);
	if (slot->ni != NULL)
		ieee80211_tx_complete(slot->ni, slot->m, status);
	else
		m_freem(slot->m);
	slot->m = NULL;
	slot->ni = NULL;
}

/* rtw_pci_tx_isr(): reclaim slots the hardware has finished. */
static void
rtw_pci_tx_isr(struct rtwb_softc *sc, int q)
{
	struct rtwb_tx_ring *tx = &sc->sc_tx_ring[q];
	uint32_t bd_idx, cur_rp, count;

	bd_idx = rtwb_read_4(sc, rtw_pci_txq_regs[q].idx);
	cur_rp = (bd_idx >> 16) & TRX_BD_IDX_MASK;
	if (cur_rp >= tx->rp)
		count = cur_rp - tx->rp;
	else
		count = tx->len - (tx->rp - cur_rp);

	bus_dmamap_sync(tx->desc.tag, tx->desc.map, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_sync(tx->buf.tag, tx->buf.map, BUS_DMASYNC_POSTWRITE);

	if (q == RTWB_TXQ_H2C) {
		/* just free command packets from host to card */
		sc->sc_h2c_done += count;
		tx->rp = cur_rp;
		return;
	}

	/*
	 * rtw88 reports ACK status from the TX report C2H only when asked;
	 * otherwise "always ACK".  Do the same: status 0.
	 */
	while (count--) {
		struct rtwb_tx_slot *slot = &tx->slot[tx->rp];

		if (slot->m != NULL) {
			rtw_pci_tx_slot_done(sc, slot, 0);
			sc->sc_tx_done++;
		}
		if (++tx->rp >= tx->len)
			tx->rp = 0;
	}

	/* rtwb: frames may be waiting for a free slot */
	rtwb_tx_ring_drained(sc);
}

/* rtw_pci_get_hw_rx_ring_nr() */
static uint32_t
rtw_pci_get_hw_rx_ring_nr(struct rtwb_softc *sc)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	uint32_t tmp, cur_wp;

	tmp = rtwb_read_4(sc, RTK_PCI_RXBD_IDX_MPDUQ);
	cur_wp = (tmp >> TRX_BD_HW_IDX_SHIFT) & TRX_BD_IDX_MASK;
	if (cur_wp >= rx->rp)
		return (cur_wp - rx->rp);
	else
		return (rx->len - (rx->rp - cur_wp));
}

/* rtw_pci_dma_check(): the hardware tags each RX buffer descriptor. */
static void
rtw_pci_dma_check(struct rtwb_softc *sc, uint32_t idx)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	struct rtw_pci_rx_buffer_desc *bd;
	uint16_t total_pkt_size;

	bd = (struct rtw_pci_rx_buffer_desc *)rx->desc.vaddr + idx;
	total_pkt_size = le16toh(bd->total_pkt_size);

	/* rx tag mismatch, throw a warning */
	if (total_pkt_size != rx->rx_tag) {
		if (sc->sc_rx_tag_err++ == 0)
			device_printf(sc->sc_dev,
			    "pci bus timeout, check dma status\n");
	}

	rx->rx_tag = (rx->rx_tag + 1) % RX_TAG_MAX;
}

/*
 * rtw_rx_query_rx_desc(), the fields needed to find the payload.  The PHY
 * status is parsed by the caller once BB/RF support exists.
 */
void
rtw_rx_query_rx_desc(const uint8_t *rx_desc, struct rtw_rx_pkt_stat *ps)
{
	uint32_t w0 = le32dec(rx_desc + 0);
	uint32_t w1 = le32dec(rx_desc + 4);
	uint32_t w2 = le32dec(rx_desc + 8);
	uint32_t w3 = le32dec(rx_desc + 12);
	uint32_t w4 = le32dec(rx_desc + 16);
	uint32_t w5 = le32dec(rx_desc + 20);
	uint32_t enc_type, swdec;

	memset(ps, 0, sizeof(*ps));
	ps->pkt_len = w0 & 0x3fff;			/* GENMASK(13, 0) */
	ps->crc_err = (w0 & BIT(14)) != 0;
	ps->icv_err = (w0 & BIT(15)) != 0;
	ps->drv_info_sz = ((w0 >> 16) & 0xf) * 8;	/* units of 8 bytes */
	enc_type = (w0 >> 20) & 0x7;
	ps->shift = (w0 >> 24) & 0x3;
	ps->phy_status = (w0 & BIT(26)) != 0;
	swdec = (w0 & BIT(27)) != 0;
	ps->decrypted = !swdec && enc_type != 0;
	ps->cam_id = w1 & 0x7f;
	ps->is_c2h = (w2 & BIT(28)) != 0;
	ps->ppdu_cnt = (w2 >> 29) & 0x3;
	ps->rate = w3 & 0x7f;
	ps->bw = (w4 >> 4) & 0x3;
	ps->tsf_low = w5;
}

/* rtw_pci_rx_napi(), without the budget: drain everything. */
static void
rtw_pci_rx_isr(struct rtwb_softc *sc)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	struct rtw_rx_pkt_stat ps;
	uint32_t count, pkt_offset, len;
	uint8_t *rx_desc;

	count = rtw_pci_get_hw_rx_ring_nr(sc);

	bus_dmamap_sync(rx->desc.tag, rx->desc.map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	while (count--) {
		rtw_pci_dma_check(sc, rx->rp);
		bus_dmamap_sync(rx->buf_tag, rx->slot[rx->rp].map,
		    BUS_DMASYNC_POSTREAD);
		rx_desc = rx->slot[rx->rp].vaddr;
		rtw_rx_query_rx_desc(rx_desc, &ps);

		/* offset from rx_desc to payload */
		pkt_offset = RTWB_RX_PKT_DESC_SZ + ps.drv_info_sz + ps.shift;
		len = ps.pkt_len + pkt_offset;
		if (len <= RTWB_RX_BUF_SIZE) {
			if (ps.is_c2h)
				rtwb_rx_c2h(sc, rx_desc + pkt_offset,
				    ps.pkt_len);
			else
				rtwb_rx_frame(sc, rx_desc, &ps, pkt_offset);
		}

		/* re-enable this buffer for DMA */
		rtw_pci_reset_rx_desc(sc, rx->rp);

		/* host read next element in ring */
		if (++rx->rp >= rx->len)
			rx->rp = 0;
	}
	bus_dmamap_sync(rx->desc.tag, rx->desc.map, BUS_DMASYNC_PREWRITE);

	rtwb_write_2(sc, RTK_PCI_RXBD_IDX_MPDUQ, rx->rp);
}

/*
 * rtw_pci_interrupt_handler() + rtw_pci_interrupt_threadfn().  Called with
 * the softc lock held.
 */
void
rtw_pci_intr(struct rtwb_softc *sc)
{
	uint32_t irq_status[4];

	RTWB_LOCK_ASSERT(sc);

	/*
	 * disable HIMR here to also avoid new HISR flag being raised before
	 * the HISRs have been Write-1-cleared for MSI. If not all of the HISRs
	 * are cleared, the edge-triggered interrupt will not be generated when
	 * a new HISR flag is set.
	 */
	rtw_pci_disable_interrupt(sc);

	/* rtw_pci_irq_recognized() */
	irq_status[0] = rtwb_read_4(sc, RTK_PCI_HISR0);
	irq_status[1] = rtwb_read_4(sc, RTK_PCI_HISR1);
	irq_status[3] = rtwb_read_4(sc, RTK_PCI_HISR3);
	irq_status[0] &= sc->sc_irq_mask[0];
	irq_status[1] &= sc->sc_irq_mask[1];
	irq_status[3] &= sc->sc_irq_mask[3];
	rtwb_write_4(sc, RTK_PCI_HISR0, irq_status[0]);
	rtwb_write_4(sc, RTK_PCI_HISR1, irq_status[1]);
	rtwb_write_4(sc, RTK_PCI_HISR3, irq_status[3]);

	if (irq_status[0] & IMR_MGNTDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_MGMT);
	if (irq_status[0] & IMR_HIGHDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_HI0);
	if (irq_status[0] & IMR_BEDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_BE);
	if (irq_status[0] & IMR_BKDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_BK);
	if (irq_status[0] & IMR_VODOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_VO);
	if (irq_status[0] & IMR_VIDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_VI);
	if (irq_status[3] & IMR_H2CDOK)
		rtw_pci_tx_isr(sc, RTWB_TXQ_H2C);
	if (irq_status[0] & IMR_ROK)
		rtw_pci_rx_isr(sc);
	/* IMR_C2HCMD: rtw88 only uses it for 8051-WCPU register C2H. */

	/* all of the jobs for this interrupt have been done */
	if (sc->sc_running)
		rtw_pci_enable_interrupt(sc, false);
}

/*
 * rtw_pci_dma_release(): reset the rings and drop frames still queued.
 * Called with the MAC already stopped or about to be powered off.
 */
void
rtw_pci_dma_release(struct rtwb_softc *sc)
{
	struct rtwb_tx_ring *tx;
	uint32_t i;
	int q;

	for (q = 0; q < RTWB_NTXQ; q++) {
		tx = &sc->sc_tx_ring[q];
		if (tx->slot == NULL)
			continue;
		for (i = 0; i < tx->len; i++)
			if (tx->slot[i].m != NULL)
				rtw_pci_tx_slot_done(sc, &tx->slot[i], 1);
	}
	rtw_pci_reset_buf_desc(sc);
}
