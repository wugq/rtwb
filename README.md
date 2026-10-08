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

Download the package for your FreeBSD version from the
[releases](https://github.com/wugq/rtwb/releases) page and install it;
pkg pulls in the firmware package `wifi-firmware-rtw88-kmod-rtw8822b`:

    sudo pkg install ./rtwb-kmod-<version>.<osversion>.pkg

A kernel module only works on the FreeBSD version it was built for (the
`<osversion>` suffix, e.g. `1501000` for 15.1); reinstall a matching
package after upgrading FreeBSD.

Or build it from the port in this repository, which fetches the release
source from GitHub.  This needs the ports tree and `/usr/src` matching
the running kernel, and is also the way to get a package for a FreeBSD
version the releases do not cover:

    git clone https://github.com/wugq/rtwb
    cd rtwb/ports/net/rtwb-kmod
    sudo make install clean

Use `sudo make reinstall clean` instead to upgrade an installed version
(after `git pull`), or `make package` to only build the `.pkg`.

Then select the driver in `/etc/rc.conf` (the base system's rtw88 claims
the same device) and reboot:

    devmatch_blocklist="${devmatch_blocklist} if_rtw88"
    kld_list="${kld_list} if_rtwb"
    wlans_rtwb0="wlan0"
    create_args_wlan0="country XX"
    ifconfig_wlan0="WPA DHCP"

See `rtwb(4)` for the details, statistics and debugging knobs.

## License

BSD-2-Clause for the driver's own code (`LICENSE`), BSD-3-Clause for the
code ported from rtw88 (`src/port/LICENSE`).
