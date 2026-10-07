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
 * busdma helpers.  The RTL8822BE can only address 32 bits (rtw88 sets a
 * 32-bit DMA mask), so every buffer is allocated below 4 GB.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include "if_rtwbvar.h"

static void
rtwb_dma_map_addr(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	if (error != 0)
		return;
	KASSERT(nsegs == 1, ("too many DMA segments, %d should be 1", nsegs));
	*(bus_addr_t *)arg = segs[0].ds_addr;
}

/* Allocate one physically contiguous, zeroed, coherent DMA buffer. */
int
rtwb_dma_alloc(struct rtwb_softc *sc, struct rtwb_dma *dma, bus_size_t size,
    bus_size_t align)
{
	int error;

	memset(dma, 0, sizeof(*dma));
	dma->size = size;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), align, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    size, 1, size, 0, NULL, NULL, &dma->tag);
	if (error != 0)
		goto fail;

	error = bus_dmamem_alloc(dma->tag, &dma->vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &dma->map);
	if (error != 0)
		goto fail;

	error = bus_dmamap_load(dma->tag, dma->map, dma->vaddr, size,
	    rtwb_dma_map_addr, &dma->paddr, BUS_DMA_NOWAIT);
	if (error != 0 || dma->paddr == 0) {
		if (error == 0)
			error = ENOMEM;
		goto fail;
	}
	return (0);

fail:
	device_printf(sc->sc_dev, "could not allocate %ju bytes of DMA memory "
	    "(error %d)\n", (uintmax_t)size, error);
	rtwb_dma_free(dma);
	return (error);
}

void
rtwb_dma_free(struct rtwb_dma *dma)
{
	if (dma->paddr != 0) {
		bus_dmamap_sync(dma->tag, dma->map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(dma->tag, dma->map);
		dma->paddr = 0;
	}
	if (dma->vaddr != NULL) {
		bus_dmamem_free(dma->tag, dma->vaddr, dma->map);
		dma->vaddr = NULL;
	}
	if (dma->tag != NULL) {
		bus_dma_tag_destroy(dma->tag);
		dma->tag = NULL;
	}
}

/*
 * Ring sizes follow rtw88 pci.h: 256 slots for BE, 128 for the other TX
 * queues, 512 RX slots of RTWB_RX_BUF_SIZE bytes.  The beacon queue has
 * its own one-entry ring (sc_bcn_ring) and is skipped here.
 */
static uint32_t
rtwb_txq_len(int q)
{
	return (q == RTWB_TXQ_BE ? 256 : 128);
}

int
rtwb_alloc_rings(struct rtwb_softc *sc)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	struct rtwb_tx_ring *tx;
	int error, i, q;

	/* TX payloads: one segment each, below 4 GB. */
	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    RTWB_TX_MAX_SIZE, 1, RTWB_TX_MAX_SIZE, 0, NULL, NULL,
	    &sc->sc_tx_mbuf_tag);
	if (error != 0)
		return (error);

	for (q = 0; q < RTWB_NTXQ; q++) {
		if (q == RTWB_TXQ_BCN)
			continue;
		tx = &sc->sc_tx_ring[q];
		tx->len = rtwb_txq_len(q);
		error = rtwb_dma_alloc(sc, &tx->desc,
		    tx->len * RTWB_TX_BUF_DESC_SZ, PAGE_SIZE);
		if (error != 0)
			return (error);
		if (q == RTWB_TXQ_H2C)
			tx->slot_size = RTWB_TX_PKT_DESC_SZ + RTWB_H2C_PKT_SIZE;
		else
			tx->slot_size = RTWB_TX_PKT_DESC_SZ;
		error = rtwb_dma_alloc(sc, &tx->buf, tx->len * tx->slot_size,
		    PAGE_SIZE);
		if (error != 0)
			return (error);
		if (q == RTWB_TXQ_H2C)
			continue;
		tx->slot = malloc(tx->len * sizeof(*tx->slot), M_DEVBUF,
		    M_WAITOK | M_ZERO);
		for (i = 0; i < tx->len; i++) {
			error = bus_dmamap_create(sc->sc_tx_mbuf_tag, 0,
			    &tx->slot[i].map);
			if (error != 0)
				return (error);
			tx->nmaps++;
		}
	}

	rx->len = RTWB_RX_RING_LEN;
	error = rtwb_dma_alloc(sc, &rx->desc, rx->len * RTWB_RX_BUF_DESC_SZ,
	    PAGE_SIZE);
	if (error != 0)
		return (error);

	/* One tag for all RX buffers; each buffer is one segment. */
	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), 8, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    RTWB_RX_BUF_SIZE, 1, RTWB_RX_BUF_SIZE, 0, NULL, NULL,
	    &rx->buf_tag);
	if (error != 0)
		return (error);
	rx->slot = malloc(rx->len * sizeof(*rx->slot), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	for (i = 0; i < rx->len; i++) {
		struct rtwb_rx_slot *s = &rx->slot[i];

		error = bus_dmamem_alloc(rx->buf_tag, &s->vaddr,
		    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &s->map);
		if (error != 0)
			return (error);
		error = bus_dmamap_load(rx->buf_tag, s->map, s->vaddr,
		    RTWB_RX_BUF_SIZE, rtwb_dma_map_addr, &s->paddr,
		    BUS_DMA_NOWAIT);
		if (error != 0 || s->paddr == 0)
			return (error != 0 ? error : ENOMEM);
	}
	return (0);
}

void
rtwb_free_rings(struct rtwb_softc *sc)
{
	struct rtwb_rx_ring *rx = &sc->sc_rx_ring;
	int i, q;

	for (q = 0; q < RTWB_NTXQ; q++) {
		struct rtwb_tx_ring *tx = &sc->sc_tx_ring[q];

		if (tx->slot != NULL) {
			/*
			 * rtwb_hw_stop() has already freed pending mbufs.  A
			 * map may legitimately be NULL (no bouncing needed)
			 * but still counts against the tag: destroy them all.
			 */
			for (i = 0; i < tx->nmaps; i++)
				bus_dmamap_destroy(sc->sc_tx_mbuf_tag,
				    tx->slot[i].map);
			tx->nmaps = 0;
			free(tx->slot, M_DEVBUF);
			tx->slot = NULL;
		}
		rtwb_dma_free(&tx->buf);
		rtwb_dma_free(&tx->desc);
	}
	if (sc->sc_tx_mbuf_tag != NULL) {
		if (bus_dma_tag_destroy(sc->sc_tx_mbuf_tag) != 0)
			device_printf(sc->sc_dev, "TX DMA tag still busy\n");
		sc->sc_tx_mbuf_tag = NULL;
	}

	if (rx->slot != NULL) {
		for (i = 0; i < rx->len; i++) {
			struct rtwb_rx_slot *s = &rx->slot[i];

			if (s->paddr != 0) {
				bus_dmamap_sync(rx->buf_tag, s->map,
				    BUS_DMASYNC_POSTREAD);
				bus_dmamap_unload(rx->buf_tag, s->map);
			}
			if (s->vaddr != NULL)
				bus_dmamem_free(rx->buf_tag, s->vaddr, s->map);
		}
		free(rx->slot, M_DEVBUF);
		rx->slot = NULL;
	}
	if (rx->buf_tag != NULL) {
		bus_dma_tag_destroy(rx->buf_tag);
		rx->buf_tag = NULL;
	}
	rtwb_dma_free(&rx->desc);
}
