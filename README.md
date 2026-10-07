# rtwb

Native FreeBSD net80211 driver for the Realtek RTL8822BE PCIe
802.11a/b/g/n/ac wireless adapter (PCI ID `10ec:b822`), without LinuxKPI.
The hardware code is ported from the Linux rtw88 driver.

Status: station and monitor mode, WPA/WPA2 (software crypto), 802.11n
(HT20/HT40) and 802.11ac (VHT80), firmware rate adaptation. Tested on
FreeBSD 15.1-RELEASE (amd64) with a Lenovo ThinkPad A475.

Limitations: Bluetooth cannot use the shared antenna while the interface
is up; no TX aggregation, hardware crypto or power save yet.

## Install

Build the port in `ports/net/rtwb-kmod` (needs `/usr/src` and the ports
tree), or install a released package with `pkg install`. The firmware
comes from `wifi-firmware-rtw88-kmod-rtw8822b`. See `pkg-message` for the
`/etc/rc.conf` settings.

## License

BSD-2-Clause for the driver's own code (`LICENSE`), BSD-3-Clause for the
code ported from rtw88 (`src/port/LICENSE`).
