# Out-of-tree build: run `make` on the target (uses /usr/src/sys by default).
SYSDIR?=	/usr/src/sys

KMOD=	if_rtwb
SRCS=	if_rtwb.c rtwb_dma.c
SRCS+=	rtw88_mac.c rtw88_efuse.c rtw88_pci.c rtw88_fw.c rtw88_phy.c rtw88_tx.c rtw88_txpwr.c rtw88_coex.c rtw88_ra.c rtw8822b.c rtw8822b_table.c
SRCS+=	rtw8822b_pwrseq.c
.PATH:	${.CURDIR}/port
SRCS+=	device_if.h bus_if.h pci_if.h opt_wlan.h

.include <bsd.kmod.mk>
