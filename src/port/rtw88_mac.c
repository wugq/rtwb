/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 mac.c: power sequence parser, MAC power on/off,
 * MAC init after the firmware download (TX DMA queue mapping, FIFO page
 * layout, LLT, H2C queue, RX driver-info size).
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * Changes: only the PCIe / 3081-WCPU (8822B) paths are kept; Linux errno
 * values became positive FreeBSD ones; register access goes through
 * rtwb_read_*() / rtwb_write_*().
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include "../if_rtwbvar.h"
#include "rtw88_reg.h"
#include "rtw88_port.h"

static bool
do_pwr_poll_cmd(struct rtwb_softc *sc, uint32_t addr, uint8_t mask,
    uint8_t target)
{
	int i;

	target &= mask;
	for (i = 0; i < RTW_PWR_POLLING_CNT; i++) {
		if ((rtwb_read_1(sc, addr) & mask) == target)
			return (true);
		DELAY(50);
	}
	return (false);
}

static int
rtw_pwr_cmd_polling(struct rtwb_softc *sc, const struct rtw_pwr_seq_cmd *cmd)
{
	uint8_t value;

	if (do_pwr_poll_cmd(sc, cmd->offset, cmd->mask, cmd->value))
		return (0);

	/* PCIe: toggle BIT_PFM_WOWL and try again. */
	value = rtwb_read_1(sc, REG_SYS_PW_CTRL);
	rtwb_write_1(sc, REG_SYS_PW_CTRL, value | BIT_PFM_WOWL);
	rtwb_write_1(sc, REG_SYS_PW_CTRL, value & ~BIT_PFM_WOWL);

	if (do_pwr_poll_cmd(sc, cmd->offset, cmd->mask, cmd->value))
		return (0);

	device_printf(sc->sc_dev,
	    "failed to poll offset=0x%x mask=0x%x value=0x%x\n",
	    cmd->offset, cmd->mask, cmd->value);
	return (EBUSY);
}

static int
rtw_sub_pwr_seq_parser(struct rtwb_softc *sc, uint8_t intf_mask,
    uint8_t cut_mask, const struct rtw_pwr_seq_cmd *cmd)
{
	const struct rtw_pwr_seq_cmd *cur_cmd;
	uint8_t value;

	for (cur_cmd = cmd; cur_cmd->cmd != RTW_PWR_CMD_END; cur_cmd++) {
		if (!(cur_cmd->intf_mask & intf_mask) ||
		    !(cur_cmd->cut_mask & cut_mask))
			continue;

		/* Entries for the SDIO local bus never match PCIe. */
		switch (cur_cmd->cmd) {
		case RTW_PWR_CMD_WRITE:
			value = rtwb_read_1(sc, cur_cmd->offset);
			value &= ~cur_cmd->mask;
			value |= (cur_cmd->value & cur_cmd->mask);
			rtwb_write_1(sc, cur_cmd->offset, value);
			break;
		case RTW_PWR_CMD_POLLING:
			if (rtw_pwr_cmd_polling(sc, cur_cmd))
				return (EBUSY);
			break;
		case RTW_PWR_CMD_DELAY:
			if (cur_cmd->value == RTW_PWR_DELAY_US)
				DELAY(cur_cmd->offset);
			else
				DELAY(cur_cmd->offset * 1000);
			break;
		case RTW_PWR_CMD_READ:
			break;
		default:
			return (EINVAL);
		}
	}

	return (0);
}

static int
rtw_pwr_seq_parser(struct rtwb_softc *sc,
    const struct rtw_pwr_seq_cmd * const *cmd_seq)
{
	uint8_t cut_mask = cut_version_to_mask(sc->hal.cut_version);
	int idx, ret;

	for (idx = 0; cmd_seq[idx] != NULL; idx++) {
		ret = rtw_sub_pwr_seq_parser(sc, RTW_PWR_INTF_PCI_MSK,
		    cut_mask, cmd_seq[idx]);
		if (ret)
			return (ret);
	}
	return (0);
}

static int
rtw_mac_pre_system_cfg(struct rtwb_softc *sc)
{
	uint32_t value32;
	uint8_t value8;

	rtwb_write_1(sc, REG_RSV_CTRL, 0);

	/* PCIe */
	rtwb_setbits_4(sc, REG_HCI_OPT_CTRL, 0, BIT_USB_SUS_DIS);

	/* config PIN Mux */
	value32 = rtwb_read_4(sc, REG_PAD_CTRL1);
	value32 |= BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL;
	rtwb_write_4(sc, REG_PAD_CTRL1, value32);

	value32 = rtwb_read_4(sc, REG_LED_CFG);
	value32 &= ~(BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
	rtwb_write_4(sc, REG_LED_CFG, value32);

	value32 = rtwb_read_4(sc, REG_GPIO_MUXCFG);
	value32 |= BIT_WLRFE_4_5_EN;
	rtwb_write_4(sc, REG_GPIO_MUXCFG, value32);

	/* disable BB/RF */
	value8 = rtwb_read_1(sc, REG_SYS_FUNC_EN);
	value8 &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtwb_write_1(sc, REG_SYS_FUNC_EN, value8);

	value8 = rtwb_read_1(sc, REG_RF_CTRL);
	value8 &= ~(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN);
	rtwb_write_1(sc, REG_RF_CTRL, value8);

	value32 = rtwb_read_4(sc, REG_WLRF1);
	value32 &= ~BIT_WLRF1_BBRF_EN;
	rtwb_write_4(sc, REG_WLRF1, value32);

	return (0);
}

static int
rtw_mac_power_switch(struct rtwb_softc *sc, bool pwr_on)
{
	uint8_t rpwm;
	bool cur_pwr;

	/* 3081 WCPU: if firmware is still alive, toggle RPWM to wake it. */
	rpwm = rtwb_read_1(sc, REG_PCIE_RPWM);
	if (rtwb_read_2(sc, REG_MCUFW_CTRL) == 0xC078) {
		rpwm = (rpwm ^ BIT_RPWM_TOGGLE) & BIT_RPWM_TOGGLE;
		rtwb_write_1(sc, REG_PCIE_RPWM, rpwm);
	}

	/* REG_CR reads 0xea while the MAC is powered off. */
	cur_pwr = rtwb_read_1(sc, REG_CR) != 0xea;
	if (pwr_on == cur_pwr)
		return (EALREADY);

	return (rtw_pwr_seq_parser(sc, pwr_on ? card_enable_flow_8822b :
	    card_disable_flow_8822b));
}

static int
rtw_mac_init_system_cfg(struct rtwb_softc *sc)
{
	uint32_t value, tmp;
	uint8_t value8;

	value = rtwb_read_4(sc, REG_CPU_DMEM_CON);
	value |= BIT_WL_PLATFORM_RST | BIT_DDMA_EN;
	rtwb_write_4(sc, REG_CPU_DMEM_CON, value);

	rtwb_setbits_1(sc, REG_SYS_FUNC_EN + 1, 0, RTW8822B_SYS_FUNC_EN);
	value8 = (rtwb_read_1(sc, REG_CR_EXT + 3) & 0xF0) | 0x0C;
	rtwb_write_1(sc, REG_CR_EXT + 3, value8);

	/* disable boot-from-flash for driver's DL FW */
	tmp = rtwb_read_4(sc, REG_MCUFW_CTRL);
	if (tmp & BIT_BOOT_FSPI_EN) {
		rtwb_write_4(sc, REG_MCUFW_CTRL, tmp & ~BIT_BOOT_FSPI_EN);
		value = rtwb_read_4(sc, REG_GPIO_MUXCFG) & ~BIT_FSPI_EN;
		rtwb_write_4(sc, REG_GPIO_MUXCFG, value);
	}

	return (0);
}

int
rtw_mac_power_on(struct rtwb_softc *sc)
{
	int ret;

	ret = rtw_mac_pre_system_cfg(sc);
	if (ret)
		goto err;

	ret = rtw_mac_power_switch(sc, true);
	if (ret == EALREADY) {
		/* Already on (e.g. left on by a previous driver): cycle it. */
		rtw_mac_power_switch(sc, false);

		ret = rtw_mac_pre_system_cfg(sc);
		if (ret)
			goto err;

		ret = rtw_mac_power_switch(sc, true);
		if (ret)
			goto err;
	} else if (ret) {
		goto err;
	}

	ret = rtw_mac_init_system_cfg(sc);
	if (ret)
		goto err;

	return (0);

err:
	device_printf(sc->sc_dev, "mac power on failed (%d)\n", ret);
	return (ret);
}

void
rtw_mac_power_off(struct rtwb_softc *sc)
{
	rtw_mac_power_switch(sc, false);
}

/* --- MAC init after the firmware download (rtw_mac_init). --- */

/* rqpn_table_8822b[1] (PCIe): vo, vi, be, bk, mg, hi */
static int
txdma_queue_mapping(struct rtwb_softc *sc)
{
	uint16_t txdma_pq_map = 0;

	txdma_pq_map |= BIT_TXDMA_HIQ_MAP(RTW_DMA_MAPPING_HIGH);
	txdma_pq_map |= BIT_TXDMA_MGQ_MAP(RTW_DMA_MAPPING_EXTRA);
	txdma_pq_map |= BIT_TXDMA_BKQ_MAP(RTW_DMA_MAPPING_LOW);
	txdma_pq_map |= BIT_TXDMA_BEQ_MAP(RTW_DMA_MAPPING_LOW);
	txdma_pq_map |= BIT_TXDMA_VIQ_MAP(RTW_DMA_MAPPING_NORMAL);
	txdma_pq_map |= BIT_TXDMA_VOQ_MAP(RTW_DMA_MAPPING_NORMAL);
	rtwb_write_2(sc, REG_TXDMA_PQ_MAP, txdma_pq_map);

	rtwb_write_1(sc, REG_CR, 0);
	rtwb_write_1(sc, REG_CR, MAC_TRX_ENABLE);
	rtwb_write_4(sc, REG_H2CQ_CSR, BIT_H2CQ_FULL);

	return (0);
}

static int
rtw_set_trx_fifo_info(struct rtwb_softc *sc)
{
	struct rtw_fifo_conf *fifo = &sc->sc_fifo;
	const uint16_t csi_buf_pg_num = RTW8822B_CSI_BUF_PG_NUM;
	uint16_t cur_pg_addr;

	/* config rsvd page num */
	fifo->rsvd_drv_pg_num = RTW8822B_RSVD_DRV_PG_NUM;
	fifo->txff_pg_num = RTW8822B_TXFF_SIZE / TX_PAGE_SIZE;
	fifo->rsvd_pg_num = fifo->rsvd_drv_pg_num +
			   RSVD_PG_H2C_EXTRAINFO_NUM +
			   RSVD_PG_H2C_STATICINFO_NUM +
			   RSVD_PG_H2CQ_NUM +
			   RSVD_PG_CPU_INSTRUCTION_NUM +
			   RSVD_PG_FW_TXBUF_NUM +
			   csi_buf_pg_num;

	if (fifo->rsvd_pg_num > fifo->txff_pg_num)
		return (ENOMEM);

	fifo->acq_pg_num = fifo->txff_pg_num - fifo->rsvd_pg_num;
	fifo->rsvd_boundary = fifo->txff_pg_num - fifo->rsvd_pg_num;

	cur_pg_addr = fifo->txff_pg_num;
	cur_pg_addr -= csi_buf_pg_num;
	fifo->rsvd_csibuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_FW_TXBUF_NUM;
	fifo->rsvd_fw_txbuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_CPU_INSTRUCTION_NUM;
	fifo->rsvd_cpu_instr_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2CQ_NUM;
	fifo->rsvd_h2cq_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_STATICINFO_NUM;
	fifo->rsvd_h2c_sta_info_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_EXTRAINFO_NUM;
	fifo->rsvd_h2c_info_addr = cur_pg_addr;
	cur_pg_addr -= fifo->rsvd_drv_pg_num;
	fifo->rsvd_drv_addr = cur_pg_addr;

	if (fifo->rsvd_boundary != fifo->rsvd_drv_addr) {
		device_printf(sc->sc_dev, "wrong rsvd driver address\n");
		return (EINVAL);
	}

	return (0);
}

/* page_table_8822b[1] (PCIe): hq, nq, lq, exq, gapq */
#define PG_HQ_NUM	64
#define PG_NQ_NUM	64
#define PG_LQ_NUM	64
#define PG_EXQ_NUM	64
#define PG_GAPQ_NUM	1

static int
priority_queue_cfg(struct rtwb_softc *sc)
{
	struct rtw_fifo_conf *fifo = &sc->sc_fifo;
	uint16_t pubq_num;
	int ret;

	ret = rtw_set_trx_fifo_info(sc);
	if (ret)
		return (ret);

	pubq_num = fifo->acq_pg_num - PG_HQ_NUM - PG_LQ_NUM -
		   PG_NQ_NUM - PG_EXQ_NUM - PG_GAPQ_NUM;

	/* __priority_queue_cfg() */
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_1, PG_HQ_NUM);
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_2, PG_LQ_NUM);
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_3, PG_NQ_NUM);
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_4, PG_EXQ_NUM);
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_5, pubq_num);
	rtwb_setbits_4(sc, REG_RQPN_CTRL_2, 0, BIT_LD_RQPN);

	rtwb_write_2(sc, REG_FIFOPAGE_CTRL_2, fifo->rsvd_boundary);
	rtwb_setbits_1(sc, REG_FWHW_TXQ_CTRL + 2, 0, BIT_EN_WR_FREE_TAIL >> 16);

	rtwb_write_2(sc, REG_BCNQ_BDNY_V1, fifo->rsvd_boundary);
	rtwb_write_2(sc, REG_FIFOPAGE_CTRL_2 + 2, fifo->rsvd_boundary);
	rtwb_write_2(sc, REG_BCNQ1_BDNY_V1, fifo->rsvd_boundary);
	rtwb_write_4(sc, REG_RXFF_BNDY, RTW8822B_RXFF_SIZE - C2H_PKT_BUF - 1);

	rtwb_setbits_1(sc, REG_AUTO_LLT_V1, 0, BIT_AUTO_INIT_LLT_V1);

	if (!check_hw_ready(sc, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0))
		return (EBUSY);

	rtwb_write_1(sc, REG_CR + 3, 0);

	return (0);
}

static int
init_h2c(struct rtwb_softc *sc)
{
	struct rtw_fifo_conf *fifo = &sc->sc_fifo;
	uint8_t value8;
	uint32_t value32;
	uint32_t h2cq_addr;
	uint32_t h2cq_size;
	uint32_t h2cq_free;
	uint32_t wp, rp;

	h2cq_addr = fifo->rsvd_h2cq_addr << TX_PAGE_SIZE_SHIFT;
	h2cq_size = RSVD_PG_H2CQ_NUM << TX_PAGE_SIZE_SHIFT;

	value32 = rtwb_read_4(sc, REG_H2C_HEAD);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtwb_write_4(sc, REG_H2C_HEAD, value32);

	value32 = rtwb_read_4(sc, REG_H2C_READ_ADDR);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtwb_write_4(sc, REG_H2C_READ_ADDR, value32);

	value32 = rtwb_read_4(sc, REG_H2C_TAIL);
	value32 &= 0xFFFC0000;
	value32 |= (h2cq_addr + h2cq_size);
	rtwb_write_4(sc, REG_H2C_TAIL, value32);

	value8 = rtwb_read_1(sc, REG_H2C_INFO);
	value8 = (uint8_t)((value8 & 0xFC) | 0x01);
	rtwb_write_1(sc, REG_H2C_INFO, value8);

	value8 = rtwb_read_1(sc, REG_H2C_INFO);
	value8 = (uint8_t)((value8 & 0xFB) | 0x04);
	rtwb_write_1(sc, REG_H2C_INFO, value8);

	value8 = rtwb_read_1(sc, REG_TXDMA_OFFSET_CHK + 1);
	value8 = (uint8_t)((value8 & 0x7f) | 0x80);
	rtwb_write_1(sc, REG_TXDMA_OFFSET_CHK + 1, value8);

	wp = rtwb_read_4(sc, REG_H2C_PKT_WRITEADDR) & 0x3FFFF;
	rp = rtwb_read_4(sc, REG_H2C_PKT_READADDR) & 0x3FFFF;
	h2cq_free = wp >= rp ? h2cq_size - (wp - rp) : rp - wp;

	if (h2cq_size != h2cq_free) {
		device_printf(sc->sc_dev, "H2C queue mismatch\n");
		return (EINVAL);
	}

	return (0);
}

static int
rtw_init_trx_cfg(struct rtwb_softc *sc)
{
	int ret;

	ret = txdma_queue_mapping(sc);
	if (ret)
		return (ret);

	ret = priority_queue_cfg(sc);
	if (ret)
		return (ret);

	return (init_h2c(sc));
}

static int
rtw_drv_info_cfg(struct rtwb_softc *sc)
{
	uint8_t value8;

	rtwb_write_1(sc, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
	value8 = rtwb_read_1(sc, REG_TRXFF_BNDY + 1);
	value8 &= 0xF0;
	/* For rxdesc len = 0 issue */
	value8 |= 0xF;
	rtwb_write_1(sc, REG_TRXFF_BNDY + 1, value8);
	rtwb_setbits_4(sc, REG_RCR, 0, BIT_APP_PHYSTS);
	rtwb_setbits_4(sc, REG_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9), 0);

	return (0);
}

/*
 * rtw_mac_init().  rtw_hci_interface_cfg() (rtw_pci_interface_cfg) does
 * nothing for the 8822B, so it is left out.
 */
int
rtw_mac_init(struct rtwb_softc *sc)
{
	int ret;

	ret = rtw_init_trx_cfg(sc);
	if (ret)
		return (ret);

	ret = rtw8822b_mac_init(sc);
	if (ret)
		return (ret);

	return (rtw_drv_info_cfg(sc));
}
