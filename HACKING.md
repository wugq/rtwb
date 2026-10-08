# Hacking on rtwb

## Layout

- `src/` -- the driver. `if_rtwb.c` (PCI attach, net80211 glue) and
  `rtwb_dma.c` (busdma) are the driver's own code, BSD-2-Clause.
- `src/port/` -- code ported from Linux rtw88, kept close to the original,
  BSD-3-Clause (`src/port/LICENSE`). `rtw88_compat.h` maps the Linux helpers
  it uses; `rtw8822b_table.c` is copied verbatim.
- `src/rtwb.4` -- the manual page, installed by the port.
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
`devmatch_blocklist="${devmatch_blocklist} if_rtw88"` in `/etc/rc.conf`
and reboot (unloading if_rtw88 at run time can panic in rtw88 itself).

Useful debug sysctls: `debug` (prints the rate adaptation setup on
association), `tx_report` (asks the firmware for per-frame TX status),
`txpwr` (TX power index per rate), `reg_addr`/`reg_val` (register access),
`channel` (tune by hand).

## Locking

- `sc_sx` (sleepable) serializes hardware bring-up, shutdown and detach;
  it is taken before `sc_mtx`.
- `sc_mtx` protects everything else: rings, queues, registers while the
  hardware runs.  The interrupt handler drops it before handing frames to
  net80211, and `rtwb_newstate()` drops the net80211 lock before taking it.
- `rtwb_hw_init()` loads the BB/RF tables under `sc_sx` only, because
  they sleep (`msleep()` in `rtw88_compat.h` is `pause_sbt()`).  Until it
  sets `sc_running` at the end, the other paths leave the hardware alone.
  Do not call `msleep()` from code that may run under `sc_mtx`.

## Testing

- Style: `perl /usr/src/tools/build/checkstyle9.pl -f if_rtwb.c
  if_rtwbvar.h rtwb_dma.c` should be clean (`port/` keeps rtw88's style).
- Man page: `mandoc -Tlint src/rtwb.4`.
- Debug kernel (WITNESS, INVARIANTS): build `GENERIC-DEBUG` with
  `MAKEOBJDIRPREFIX=$HOME/obj make buildkernel KERNCONF=GENERIC-DEBUG`
  in `/usr/src`, install it next to the normal kernel with
  `INSTKERNNAME=kernel.debug`, boot it once with
  `sudo nextboot -k kernel.debug`, and build the module against it so
  the `KASSERT`s are compiled in:
  `make KERNBUILDDIR=$HOME/obj/usr/src/amd64.amd64/sys/GENERIC-DEBUG`.
  Then check `dmesg` for "lock order reversal" after down/up, load/unload
  and traffic.
- Suspend/resume of just the adapter: `devctl suspend rtwb0`,
  `devctl resume rtwb0`; `pciconf -r pci0:3:0:0 0x44` shows the PCI
  power state (low bits 3 = D3, 0 = D0), and wlan0 should reassociate.

## Release

1. Set `DISTVERSION` in `ports/net/rtwb-kmod/Makefile`, commit.
2. Tag and push: `git tag -a v<version> -m ... && git push --tags`.
   CI builds the package and attaches it to the GitHub release.
3. Update `ports/net/rtwb-kmod/distinfo` from GitHub's tarball of the tag
   (`make makesum` in the port, which fetches it) and commit it.  Then
   check a port install from a fresh clone (`make reinstall clean` if an
   older version is installed).  CI
   regenerates distinfo from `git archive`, whose checksum differs from
   GitHub's tarball, so the committed file is only needed for building the
   port outside CI.

Kernel modules are tied to the FreeBSD version they were built for (the
`.1501000`-style suffix of the package version): the CI matrix builds one
package per release.
