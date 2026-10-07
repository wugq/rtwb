PORTNAME=	rtwb
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.0
CATEGORIES=	net
PKGNAMESUFFIX=	-kmod

MAINTAINER=	wugq.dev@gmail.com
COMMENT=	Native net80211 driver for Realtek RTL8822BE wireless
WWW=		https://github.com/wugq/rtwb

LICENSE=	BSD2CLAUSE BSD3CLAUSE
LICENSE_COMB=	multi
LICENSE_FILE_BSD2CLAUSE=	${WRKSRC}/../LICENSE
LICENSE_FILE_BSD3CLAUSE=	${WRKSRC}/port/LICENSE

RUN_DEPENDS=	wifi-firmware-rtw88-kmod-rtw8822b>0:net/wifi-firmware-rtw88-kmod@rtw8822b

USES=		kmod uidfix

USE_GITHUB=	yes
GH_ACCOUNT=	wugq

WRKSRC_SUBDIR=	src

PLIST_FILES=	${KMODDIR}/if_rtwb.ko

.include <bsd.port.mk>
