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

## Bluetooth coexistence on another board

Wi-Fi and Bluetooth share the antenna through the chip's arbiter (PTA).
`rtw_coex_runtime_setup()` in `src/port/rtw88_coex.c` programs a static
setup that was chosen by measurement on RFE option 5 with a shared
antenna; rtw88's own runtime paths behaved worse there.  On another
board, check it like this:

1. Test incoming connections, not just outgoing ones (a mouse or
   headphones reconnecting are incoming): from another Bluetooth host,
   open an L2CAP connection to PSM 1 of this machine, e.g. with Python
   on Linux:
   `socket.socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP).connect((addr, 1))`.
   "Connection refused" means the connection came up (no sdpd here);
   "Host is down" after 5.12 s is a page timeout.  Try with wlan0 down,
   with Wi-Fi idle and during an iperf3 upload, on 2.4 GHz and 5 GHz
   (`ifconfig wlan0 chanlist 1-13` or `36-165`, then down/up).
2. Read the current setup with the debug sysctls (`reg_addr`, then
   `reg_val`); the registers stay as written until the next channel
   switch:
   - GNT signals: write `0x800F0038` to `0x1700`, read `0x1708`.  Bits
     `0xcc00` are GNT_BT, `0x3300` GNT_WL; per two-bit field 0 = PTA,
     1 = forced low, 3 = forced high.  Write a new value to `0x1704`,
     then `0xC00F0038` to `0x1700`.
   - PTA table: `0x6c0` and `0x6c4` (rtw88's `table_sant_8822b[]`).
   - Antenna switch: low byte of `0xcb4` (`0x77` baseband, `0x66` PTA),
     bits 9:8 of `0xcbc` (= `0xcbd[1:0]`, the position).
   - Path owner: bit 26 of `0x70` (1 = Wi-Fi).
3. Change one thing at a time, repeat step 1 with enough tries (the
   2.4 GHz band is noisy), and compare the Wi-Fi throughput too.  Then
   make the result conditional on the RFE option in
   `rtw_coex_runtime_setup()`.

The Realtek vendor driver (e.g. github.com/morrownr/88x2bu-20210702,
`hal/btc/halbtc8822b1ant.c` and `halbtc8822b2ant.c`) documents more of
these registers, but it is GPL-2.0 only: read it, do not copy from it.

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
