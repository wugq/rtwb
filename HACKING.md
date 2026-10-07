# Hacking on rtwb

## Layout

- `src/` -- the driver. `if_rtwb.c` (PCI attach, net80211 glue) and
  `rtwb_dma.c` (busdma) are the driver's own code, BSD-2-Clause.
- `src/port/` -- code ported from Linux rtw88, kept close to the original,
  BSD-3-Clause (`src/port/LICENSE`). `rtw88_compat.h` maps the Linux helpers
  it uses; `rtw8822b_table.c` is copied verbatim.
- `ports/net/rtwb-kmod/` -- the FreeBSD port.
- `scripts/make-distfile.sh` -- builds the port's distfile from a git ref.
- `.github/workflows/package.yml` -- CI: builds the package in a FreeBSD VM.

## Build and try it

Needs `/usr/src` matching the running kernel and the firmware package
`wifi-firmware-rtw88-kmod-rtw8822b`.

    cd src && make
    sudo kldload ./if_rtwb.ko
    sudo ifconfig wlan0 create wlandev rtwb0
    sysctl dev.rtwb.0          # counters and debug knobs

The LinuxKPI rtw88 driver must not own the device: set
`devmatch_blocklist="if_rtw88"` in `/etc/rc.conf` and reboot (unloading
if_rtw88 at run time can panic in rtw88 itself).

Useful debug sysctls: `debug` (prints the rate adaptation setup on
association), `tx_report` (asks the firmware for per-frame TX status),
`txpwr` (TX power index per rate), `reg_addr`/`reg_val` (register access),
`channel` (tune by hand).

## Release

1. Set `DISTVERSION` in `ports/net/rtwb-kmod/Makefile`, commit.
2. Tag and push: `git tag -a v<version> -m ... && git push --tags`.
   CI builds the package and attaches it to the GitHub release.
3. Update `ports/net/rtwb-kmod/distinfo` from GitHub's tarball of the tag
   (`make makesum` in the port, which fetches it) and commit it.  CI
   regenerates distinfo from `git archive`, whose checksum differs from
   GitHub's tarball, so the committed file is only needed for building the
   port outside CI.

Kernel modules are tied to the FreeBSD version they were built for (the
`.1501000`-style suffix of the package version): the CI matrix builds one
package per release.
