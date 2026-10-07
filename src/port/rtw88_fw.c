/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Ported from Linux rtw88 mac.c (firmware download for 3081-WCPU chips),
 * fw.c (rtw_fw_write_data_rsvd_page, H2C packets), main.c
 * (rtw_dump_hw_feature) and util.c (check_hw_ready, LTE coex
 * indirect access, register backup/restore).
 * Copyright(c) 2018-2019  Realtek Corporation
 * Original license: GPL-2.0 OR BSD-3-Clause; see LICENSE in this directory.
 *
 * Changes: 8822B/PCIe only (no legacy 8051 path, no USB/SDIO quirks);
 * firmware comes as a flat buffer instead of struct firmware; Linux errno
 * values became positive ones.  The reserved-page boundary is still 0
 * during the download because rtw_mac_init() has not run yet, exactly as
 * in rtw88.
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

struct rtw_backup_info {
	uint8_t		len;
	uint32_t	reg;
	uint32_t	val;
};

#define DLFW_RESTORE_REG_NUM	6

static inline uint32_t
le32_at(const uint8_t *p)
{
	return (le32dec(p));
}

/* rtw_read32_mask(): masked field, shifted down to bit 0. */
static uint32_t
rtw_read32_mask(struct rtwb_softc *sc, uint32_t addr, uint32_t mask)
{
	return ((rtwb_read_4(sc, addr) & mask) >> (ffs(mask) - 1));
}

/* util.c: poll up to 10 ms for a register field to reach 'target'. */
bool
check_hw_ready(struct rtwb_softc *sc, uint32_t addr, uint32_t mask,
    uint32_t target)
{
	int cnt;

	for (cnt = 0; cnt < 1000; cnt++) {
		if (rtw_read32_mask(sc, addr, mask) == target)
			return (true);
		DELAY(10);
	}
	return (false);
}

bool
ltecoex_read_reg(struct rtwb_softc *sc, uint16_t offset, uint32_t *val)
{
	if (!check_hw_ready(sc, LTECOEX_ACCESS_CTRL, LTECOEX_READY, 1))
		return (false);

	rtwb_write_4(sc, LTECOEX_ACCESS_CTRL, 0x800F0000 | offset);
	*val = rtwb_read_4(sc, LTECOEX_READ_DATA);
	return (true);
}

bool
ltecoex_reg_write(struct rtwb_softc *sc, uint16_t offset, uint32_t value)
{
	if (!check_hw_ready(sc, LTECOEX_ACCESS_CTRL, LTECOEX_READY, 1))
		return (false);

	rtwb_write_4(sc, LTECOEX_WRITE_DATA, value);
	rtwb_write_4(sc, LTECOEX_ACCESS_CTRL, 0xC00F0000 | offset);
	return (true);
}

static void
rtw_restore_reg(struct rtwb_softc *sc, const struct rtw_backup_info *bckp,
    uint32_t num)
{
	uint32_t i;

	for (i = 0; i < num; i++, bckp++) {
		switch (bckp->len) {
		case 1:
			rtwb_write_1(sc, bckp->reg, (uint8_t)bckp->val);
			break;
		case 2:
			rtwb_write_2(sc, bckp->reg, (uint16_t)bckp->val);
			break;
		case 4:
			rtwb_write_4(sc, bckp->reg, bckp->val);
			break;
		}
	}
}

/* fw.c */
static int
rtw_fw_write_data_rsvd_page(struct rtwb_softc *sc, uint16_t pg_addr,
    const uint8_t *buf, uint32_t size)
{
	uint8_t bckp[3];
	uint8_t val;
	uint16_t rsvd_pg_head;
	int ret;

	if (size == 0)
		return (EINVAL);

	bckp[2] = rtwb_read_1(sc, REG_BCN_CTRL);

	pg_addr &= BIT_MASK_BCN_HEAD_1_V1;
	pg_addr |= BIT_BCN_VALID_V1;
	rtwb_write_2(sc, REG_FIFOPAGE_CTRL_2, pg_addr);

	val = rtwb_read_1(sc, REG_CR + 1);
	bckp[0] = val;
	val |= BIT_ENSWBCN >> 8;
	rtwb_write_1(sc, REG_CR + 1, val);

	rtwb_write_1(sc, REG_BCN_CTRL,
	    (bckp[2] & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);

	/* PCIe */
	val = rtwb_read_1(sc, REG_FWHW_TXQ_CTRL + 2);
	bckp[1] = val;
	val &= ~(BIT_EN_BCNQ_DL >> 16);
	rtwb_write_1(sc, REG_FWHW_TXQ_CTRL + 2, val);

	ret = rtw_pci_write_data_rsvd_page(sc, buf, size);
	if (ret) {
		device_printf(sc->sc_dev,
		    "failed to write data to rsvd page\n");
		goto restore;
	}

	if (!check_hw_ready(sc, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1, 1)) {
		device_printf(sc->sc_dev, "error beacon valid\n");
		ret = EBUSY;
	}

restore:
	/* 0 during the firmware download, which runs before rtw_mac_init(). */
	rsvd_pg_head = sc->sc_fifo.rsvd_boundary;
	rtwb_write_2(sc, REG_FIFOPAGE_CTRL_2, rsvd_pg_head | BIT_BCN_VALID_V1);
	rtwb_write_1(sc, REG_BCN_CTRL, bckp[2]);
	rtwb_write_1(sc, REG_FWHW_TXQ_CTRL + 2, bckp[1]);
	rtwb_write_1(sc, REG_CR + 1, bckp[0]);

	return (ret);
}

/* mac.c from here on. */

static bool
check_firmware_size(const uint8_t *data, uint32_t size)
{
	uint32_t dmem_size, imem_size, emem_size, real_size;

	if (size < FW_HDR_SIZE)
		return (false);

	dmem_size = le32_at(data + FW_HDR_DMEM_SIZE);
	imem_size = le32_at(data + FW_HDR_IMEM_SIZE);
	emem_size = (data[FW_HDR_MEM_USAGE] & BIT(4)) ?
	    le32_at(data + FW_HDR_EMEM_SIZE) : 0;

	dmem_size += FW_HDR_CHKSUM_SIZE;
	imem_size += FW_HDR_CHKSUM_SIZE;
	emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;
	real_size = FW_HDR_SIZE + dmem_size + imem_size + emem_size;
	return (real_size == size);
}

static void
wlan_cpu_enable(struct rtwb_softc *sc, bool enable)
{
	if (enable) {
		/* cpu io interface enable */
		rtwb_setbits_1(sc, REG_RSV_CTRL + 1, 0, BIT_WLMCU_IOIF);
		/* cpu enable */
		rtwb_setbits_1(sc, REG_SYS_FUNC_EN + 1, 0, BIT_FEN_CPUEN);
	} else {
		/* cpu io interface disable */
		rtwb_setbits_1(sc, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN, 0);
		/* cpu disable */
		rtwb_setbits_1(sc, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF, 0);
	}
}

static void
download_firmware_reg_backup(struct rtwb_softc *sc,
    struct rtw_backup_info *bckp)
{
	uint8_t tmp;
	int bckp_idx = 0;

	/* set HIQ to hi priority */
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_TXDMA_PQ_MAP + 1;
	bckp[bckp_idx].val = rtwb_read_1(sc, REG_TXDMA_PQ_MAP + 1);
	bckp_idx++;
	tmp = RTW_DMA_MAPPING_HIGH << 6;
	rtwb_write_1(sc, REG_TXDMA_PQ_MAP + 1, tmp);

	/* DLFW only use HIQ, map HIQ to hi priority */
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_CR;
	bckp[bckp_idx].val = rtwb_read_1(sc, REG_CR);
	bckp_idx++;
	bckp[bckp_idx].len = 4;
	bckp[bckp_idx].reg = REG_H2CQ_CSR;
	bckp[bckp_idx].val = BIT_H2CQ_FULL;
	bckp_idx++;
	tmp = BIT_HCI_TXDMA_EN | BIT_TXDMA_EN;
	rtwb_write_1(sc, REG_CR, tmp);
	rtwb_write_4(sc, REG_H2CQ_CSR, BIT_H2CQ_FULL);

	/* Config hi priority queue and public priority queue page number */
	bckp[bckp_idx].len = 2;
	bckp[bckp_idx].reg = REG_FIFOPAGE_INFO_1;
	bckp[bckp_idx].val = rtwb_read_2(sc, REG_FIFOPAGE_INFO_1);
	bckp_idx++;
	bckp[bckp_idx].len = 4;
	bckp[bckp_idx].reg = REG_RQPN_CTRL_2;
	bckp[bckp_idx].val = rtwb_read_4(sc, REG_RQPN_CTRL_2) | BIT_LD_RQPN;
	bckp_idx++;
	rtwb_write_2(sc, REG_FIFOPAGE_INFO_1, 0x200);
	rtwb_write_4(sc, REG_RQPN_CTRL_2, bckp[bckp_idx - 1].val);

	/* Disable beacon related functions */
	tmp = rtwb_read_1(sc, REG_BCN_CTRL);
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_BCN_CTRL;
	bckp[bckp_idx].val = tmp;
	bckp_idx++;
	tmp = (uint8_t)((tmp & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);
	rtwb_write_1(sc, REG_BCN_CTRL, tmp);

	KASSERT(bckp_idx == DLFW_RESTORE_REG_NUM, ("wrong backup number"));
}

static void
download_firmware_reset_platform(struct rtwb_softc *sc)
{
	rtwb_setbits_1(sc, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16, 0);
	rtwb_setbits_1(sc, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8, 0);
	rtwb_setbits_1(sc, REG_CPU_DMEM_CON + 2, 0, BIT_WL_PLATFORM_RST >> 16);
	rtwb_setbits_1(sc, REG_SYS_CLK_CTRL + 1, 0, BIT_CPU_CLK_EN >> 8);
}

static int
iddma_enable(struct rtwb_softc *sc, uint32_t src, uint32_t dst, uint32_t ctrl)
{
	rtwb_write_4(sc, REG_DDMA_CH0SA, src);
	rtwb_write_4(sc, REG_DDMA_CH0DA, dst);
	rtwb_write_4(sc, REG_DDMA_CH0CTRL, ctrl);

	if (!check_hw_ready(sc, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
		return (EBUSY);
	return (0);
}

static int
iddma_download_firmware(struct rtwb_softc *sc, uint32_t src, uint32_t dst,
    uint32_t len, bool first)
{
	uint32_t ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;

	if (!check_hw_ready(sc, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
		return (EBUSY);

	ch0_ctrl |= len & BIT_MASK_DDMACH0_DLEN;
	if (!first)
		ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;

	return (iddma_enable(sc, src, dst, ch0_ctrl));
}

static bool
check_fw_checksum(struct rtwb_softc *sc, uint32_t addr)
{
	uint8_t fw_ctrl;

	fw_ctrl = rtwb_read_1(sc, REG_MCUFW_CTRL);

	if (rtwb_read_4(sc, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
		if (addr < OCPBASE_DMEM_88XX) {
			fw_ctrl |= BIT_IMEM_DW_OK;
			fw_ctrl &= ~BIT_IMEM_CHKSUM_OK;
		} else {
			fw_ctrl |= BIT_DMEM_DW_OK;
			fw_ctrl &= ~BIT_DMEM_CHKSUM_OK;
		}
		rtwb_write_1(sc, REG_MCUFW_CTRL, fw_ctrl);
		device_printf(sc->sc_dev, "invalid fw checksum\n");
		return (false);
	}

	if (addr < OCPBASE_DMEM_88XX)
		fw_ctrl |= (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
	else
		fw_ctrl |= (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
	rtwb_write_1(sc, REG_MCUFW_CTRL, fw_ctrl);

	return (true);
}

static int
download_firmware_to_mem(struct rtwb_softc *sc, const uint8_t *data,
    uint32_t src, uint32_t dst, uint32_t size)
{
	const uint32_t desc_size = RTW8822B_TX_PKT_DESC_SZ;
	const uint32_t max_size = RTWB_FW_CHUNK_SIZE;
	uint32_t mem_offset, residue_size, pkt_size, val;
	bool first_part;
	int ret;

	mem_offset = 0;
	first_part = true;
	residue_size = size;

	val = rtwb_read_4(sc, REG_DDMA_CH0CTRL);
	val |= BIT_DDMACH0_RESET_CHKSUM_STS;
	rtwb_write_4(sc, REG_DDMA_CH0CTRL, val);

	while (residue_size) {
		pkt_size = MIN(residue_size, max_size);

		/* Host -> reserved page (PCIe DMA, beacon queue). */
		ret = rtw_fw_write_data_rsvd_page(sc, (uint16_t)(src >> 7),
		    data + mem_offset, pkt_size);
		if (ret) {
			device_printf(sc->sc_dev,
			    "failed to download rsvd page\n");
			return (ret);
		}

		/* Reserved page -> IMEM/DMEM (on-chip DDMA). */
		ret = iddma_download_firmware(sc,
		    OCPBASE_TXBUF_88XX + src + desc_size,
		    dst + mem_offset, pkt_size, first_part);
		if (ret)
			return (ret);

		first_part = false;
		mem_offset += pkt_size;
		residue_size -= pkt_size;
	}

	if (!check_fw_checksum(sc, dst))
		return (EINVAL);

	return (0);
}

static int
start_download_firmware(struct rtwb_softc *sc, const uint8_t *data)
{
	const uint8_t *cur_fw;
	uint32_t imem_size, dmem_size, emem_size, addr;
	uint16_t val;
	int ret;

	dmem_size = le32_at(data + FW_HDR_DMEM_SIZE);
	imem_size = le32_at(data + FW_HDR_IMEM_SIZE);
	emem_size = (data[FW_HDR_MEM_USAGE] & BIT(4)) ?
	    le32_at(data + FW_HDR_EMEM_SIZE) : 0;
	dmem_size += FW_HDR_CHKSUM_SIZE;
	imem_size += FW_HDR_CHKSUM_SIZE;
	emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;

	val = (uint16_t)(rtwb_read_2(sc, REG_MCUFW_CTRL) & 0x3800);
	val |= BIT_MCUFWDL_EN;
	rtwb_write_2(sc, REG_MCUFW_CTRL, val);

	cur_fw = data + FW_HDR_SIZE;
	addr = le32_at(data + FW_HDR_DMEM_ADDR) & ~BIT(31);
	ret = download_firmware_to_mem(sc, cur_fw, 0, addr, dmem_size);
	if (ret)
		return (ret);

	cur_fw = data + FW_HDR_SIZE + dmem_size;
	addr = le32_at(data + FW_HDR_IMEM_ADDR) & ~BIT(31);
	ret = download_firmware_to_mem(sc, cur_fw, 0, addr, imem_size);
	if (ret)
		return (ret);

	if (emem_size) {
		cur_fw = data + FW_HDR_SIZE + dmem_size + imem_size;
		addr = le32_at(data + FW_HDR_EMEM_ADDR) & ~BIT(31);
		ret = download_firmware_to_mem(sc, cur_fw, 0, addr,
		    emem_size);
		if (ret)
			return (ret);
	}

	return (0);
}

static int
download_firmware_validate(struct rtwb_softc *sc)
{
	uint32_t fw_key;

	if (!check_hw_ready(sc, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
		fw_key = rtwb_read_4(sc, REG_FW_DBG7) & FW_KEY_MASK;
		if (fw_key == ILLEGAL_KEY_GROUP)
			device_printf(sc->sc_dev, "invalid fw key\n");
		return (EINVAL);
	}
	return (0);
}

static void
download_firmware_end_flow(struct rtwb_softc *sc)
{
	uint16_t fw_ctrl;

	rtwb_write_4(sc, REG_TXDMA_STATUS, BTI_PAGE_OVF);

	/* Check IMEM & DMEM checksum is OK or not */
	fw_ctrl = rtwb_read_2(sc, REG_MCUFW_CTRL);
	if ((fw_ctrl & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
		return;

	fw_ctrl = (fw_ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN;
	rtwb_write_2(sc, REG_MCUFW_CTRL, fw_ctrl);
}

/* __rtw_download_firmware(). The MAC must be powered on. */
int
rtw_download_firmware(struct rtwb_softc *sc, const uint8_t *data,
    uint32_t size)
{
	struct rtw_backup_info bckp[DLFW_RESTORE_REG_NUM];
	uint32_t ltecoex_bckp;
	int ret;

	if (!check_firmware_size(data, size)) {
		device_printf(sc->sc_dev, "firmware size mismatch\n");
		return (EINVAL);
	}

	if (!ltecoex_read_reg(sc, 0x38, &ltecoex_bckp))
		return (EBUSY);

	wlan_cpu_enable(sc, false);

	download_firmware_reg_backup(sc, bckp);
	download_firmware_reset_platform(sc);

	ret = start_download_firmware(sc, data);
	if (ret)
		goto dlfw_fail;

	rtw_restore_reg(sc, bckp, DLFW_RESTORE_REG_NUM);

	download_firmware_end_flow(sc);

	wlan_cpu_enable(sc, true);

	if (!ltecoex_reg_write(sc, 0x38, ltecoex_bckp)) {
		ret = EBUSY;
		goto dlfw_fail;
	}

	ret = download_firmware_validate(sc);
	if (ret)
		goto dlfw_fail;

	/* reset desc and index */
	rtw_pci_setup(sc);

	return (0);

dlfw_fail:
	/* Disable FWDL_EN */
	rtwb_setbits_1(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN, 0);
	rtwb_setbits_1(sc, REG_SYS_FUNC_EN + 1, 0, BIT_FEN_CPUEN);

	return (ret);
}

/* --- H2C packets and the HW feature report (fw.c, main.c). --- */

static void
le32_replace_bits(uint8_t *p, int word, uint32_t value, int lo, int hi)
{
	uint32_t mask = (hi == 31 ? 0xffffffffU : ((1U << (hi + 1)) - 1)) &
	    ~((1U << lo) - 1);
	uint32_t v = le32dec(p + 4 * word);

	v = (v & ~mask) | ((value << lo) & mask);
	le32enc(p + 4 * word, v);
}

#define SET_PKT_H2C_CATEGORY(p, v)	le32_replace_bits(p, 0, v, 0, 6)
#define SET_PKT_H2C_CMD_ID(p, v)	le32_replace_bits(p, 0, v, 8, 15)
#define SET_PKT_H2C_SUB_CMD_ID(p, v)	le32_replace_bits(p, 0, v, 16, 31)
#define SET_PKT_H2C_TOTAL_LEN(p, v)	le32_replace_bits(p, 1, v, 0, 15)
#define FW_OFFLOAD_H2C_SET_SEQ_NUM(p, v) le32_replace_bits(p, 1, v, 16, 31)
#define GENERAL_INFO_SET_FW_TX_BOUNDARY(p, v) le32_replace_bits(p, 2, v, 16, 23)
#define PHYDM_INFO_SET_REF_TYPE(p, v)	le32_replace_bits(p, 2, v, 0, 7)
#define PHYDM_INFO_SET_RF_TYPE(p, v)	le32_replace_bits(p, 2, v, 8, 15)
#define PHYDM_INFO_SET_CUT_VER(p, v)	le32_replace_bits(p, 2, v, 16, 23)
#define PHYDM_INFO_SET_RX_ANT_STATUS(p, v) le32_replace_bits(p, 2, v, 24, 27)
#define PHYDM_INFO_SET_TX_ANT_STATUS(p, v) le32_replace_bits(p, 2, v, 28, 31)

static void
rtw_h2c_pkt_set_header(uint8_t *h2c_pkt, uint8_t sub_id)
{
	SET_PKT_H2C_CATEGORY(h2c_pkt, H2C_PKT_CATEGORY);
	SET_PKT_H2C_CMD_ID(h2c_pkt, H2C_PKT_CMD_ID);
	SET_PKT_H2C_SUB_CMD_ID(h2c_pkt, sub_id);
}

static void
rtw_fw_send_h2c_packet(struct rtwb_softc *sc, uint8_t *h2c_pkt)
{
	int ret;

	RTWB_LOCK_ASSERT(sc);

	FW_OFFLOAD_H2C_SET_SEQ_NUM(h2c_pkt, sc->sc_h2c_seq);
	ret = rtw_pci_write_data_h2c(sc, h2c_pkt, H2C_PKT_SIZE);
	if (ret)
		device_printf(sc->sc_dev, "failed to send h2c packet\n");
	sc->sc_h2c_seq++;
}

void
rtw_fw_send_general_info(struct rtwb_softc *sc)
{
	struct rtw_fifo_conf *fifo = &sc->sc_fifo;
	uint8_t h2c_pkt[H2C_PKT_SIZE] = {0};
	uint16_t total_size = H2C_PKT_HDR_SIZE + 4;

	rtw_h2c_pkt_set_header(h2c_pkt, H2C_PKT_GENERAL_INFO);

	SET_PKT_H2C_TOTAL_LEN(h2c_pkt, total_size);

	GENERAL_INFO_SET_FW_TX_BOUNDARY(h2c_pkt,
	    fifo->rsvd_fw_txbuf_addr - fifo->rsvd_boundary);

	rtw_fw_send_h2c_packet(sc, h2c_pkt);
}

void
rtw_fw_send_phydm_info(struct rtwb_softc *sc)
{
	uint8_t h2c_pkt[H2C_PKT_SIZE] = {0};
	uint16_t total_size = H2C_PKT_HDR_SIZE + 8;
	uint8_t fw_rf_type, ant;

	fw_rf_type = sc->hal.rf_path_num == 2 ? FW_RF_2T2R : FW_RF_1T1R;
	ant = sc->hal.rf_path_num == 2 ? (BB_PATH_A | BB_PATH_B) : BB_PATH_A;

	rtw_h2c_pkt_set_header(h2c_pkt, H2C_PKT_PHYDM_INFO);

	SET_PKT_H2C_TOTAL_LEN(h2c_pkt, total_size);
	PHYDM_INFO_SET_REF_TYPE(h2c_pkt, sc->efuse.rfe_option);
	PHYDM_INFO_SET_RF_TYPE(h2c_pkt, fw_rf_type);
	PHYDM_INFO_SET_CUT_VER(h2c_pkt, sc->hal.cut_version);
	PHYDM_INFO_SET_RX_ANT_STATUS(h2c_pkt, ant);
	PHYDM_INFO_SET_TX_ANT_STATUS(h2c_pkt, ant);

	rtw_fw_send_h2c_packet(sc, h2c_pkt);
}

static uint8_t
hw_bw_cap_to_bitamp(uint8_t bw_cap)
{
	uint8_t bw = 0;

	switch (bw_cap) {
	case EFUSE_HW_CAP_IGNORE:
	case EFUSE_HW_CAP_SUPP_BW80:
		bw |= BIT(RTW_CHANNEL_WIDTH_80);
		/* FALLTHROUGH */
	case EFUSE_HW_CAP_SUPP_BW40:
		bw |= BIT(RTW_CHANNEL_WIDTH_40);
		/* FALLTHROUGH */
	default:
		bw |= BIT(RTW_CHANNEL_WIDTH_20);
		break;
	}
	return (bw);
}

/*
 * rtw_dump_hw_feature(): the firmware answers C2H_HW_FEATURE_DUMP (written
 * to REG_C2HEVT before the download) through the same register.
 */
int
rtw_dump_hw_feature(struct rtwb_softc *sc)
{
	uint8_t hw_feature[HW_FEATURE_LEN];
	uint32_t w1;
	uint8_t id;
	int i;

	id = rtwb_read_1(sc, REG_C2HEVT);
	if (id != C2H_HW_FEATURE_REPORT) {
		device_printf(sc->sc_dev,
		    "failed to read hw feature report (id %#x)\n", id);
		return (EBUSY);
	}

	for (i = 0; i < HW_FEATURE_LEN; i++)
		hw_feature[i] = rtwb_read_1(sc, REG_C2HEVT + 2 + i);

	rtwb_write_1(sc, REG_C2HEVT, 0);

	w1 = le32dec(hw_feature + 4);
	sc->sc_hw_cap_bw = hw_bw_cap_to_bitamp((w1 >> 16) & 0x7);
	sc->sc_hw_cap_nss = (w1 >> 19) & 0x3;
	sc->sc_hw_cap_ant_num = (w1 >> 21) & 0x7;
	sc->sc_hw_cap_ptcl = (w1 >> 26) & 0x3;

	if (sc->sc_hw_cap_nss == EFUSE_HW_CAP_IGNORE ||
	    sc->sc_hw_cap_nss > sc->hal.rf_path_num)
		sc->sc_hw_cap_nss = sc->hal.rf_path_num;

	return (0);
}

/* --- H2C commands through the mailbox registers (fw.c). --- */

#define REG_HMETFR		0x01CC
#define REG_HMEBOX0		0x01D0
#define REG_HMEBOX0_EX		0x01F0
#define H2C_CMD_MEDIA_STATUS_RPT 0x01

#define SET_H2C_CMD_ID_CLASS(p, v)		le32_replace_bits(p, 0, v, 0, 7)
#define MEDIA_STATUS_RPT_SET_OP_MODE(p, v)	le32_replace_bits(p, 0, v, 8, 8)
#define MEDIA_STATUS_RPT_SET_MACID(p, v)	le32_replace_bits(p, 0, v, 16, 23)

/*
 * rtw_fw_send_h2c_command(): four mailboxes used round-robin; wait until
 * the firmware has consumed the previous message in the chosen box.
 */
void
rtw_fw_send_h2c_command(struct rtwb_softc *sc, const uint8_t *h2c)
{
	uint32_t box_reg, box_ex_reg;
	uint8_t box, box_state;
	int i;

	RTWB_LOCK_ASSERT(sc);

	box = sc->sc_h2c_last_box;
	box_reg = REG_HMEBOX0 + box * 4;
	box_ex_reg = REG_HMEBOX0_EX + box * 4;

	/* read_poll_timeout_atomic(..., 100, 3000, ...) */
	for (i = 0; i < 30; i++) {
		box_state = rtwb_read_1(sc, REG_HMETFR);
		if (!((box_state >> box) & 0x1))
			break;
		DELAY(100);
	}
	if (i == 30) {
		device_printf(sc->sc_dev, "failed to send h2c command\n");
		return;
	}

	rtwb_write_4(sc, box_ex_reg, le32dec(h2c + 4));
	rtwb_write_4(sc, box_reg, le32dec(h2c));

	if (++sc->sc_h2c_last_box >= 4)
		sc->sc_h2c_last_box = 0;
}

void
rtw_fw_media_status_report(struct rtwb_softc *sc, uint8_t mac_id,
    bool connect)
{
	uint8_t h2c_pkt[H2C_PKT_SIZE] = {0};

	SET_H2C_CMD_ID_CLASS(h2c_pkt, H2C_CMD_MEDIA_STATUS_RPT);
	MEDIA_STATUS_RPT_SET_OP_MODE(h2c_pkt, connect);
	MEDIA_STATUS_RPT_SET_MACID(h2c_pkt, mac_id);

	rtw_fw_send_h2c_command(sc, h2c_pkt);
}
