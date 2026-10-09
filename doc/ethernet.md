# Ethernet

Ethernet support is intended for LiteX Acorn Baseboard Mini deployments and is bandwidth-limited
by the selected 1000BASE-X/2500BASE-X link.

## Overview

The M.2 connector carries 4 SerDes transceivers that are in most cases used for... PCIe :) But
these are 4 classical GTP transceivers of the Artix7 FPGA that are connected to the PCIe hardened
PHY in the case of a PCIe application but can be used for any other SerDes-based protocol:
Ethernet 1000BaseX/2500BaseX, SATA, etc...

In the Ethernet design, the PCIe core is replaced with [LiteEth](https://github.com/enjoy-digital/liteeth),
providing the 1000BaseX or 2500BaseX PHY but also the UDP/IP hardware stack + Streaming/Etherbone
front-end cores.

The Ethernet SoC design supports control plus RX/TX sample streaming over the LiteEth UDP path.
The achievable 2T2R sample rate is capped by link bandwidth, so Ethernet builds also cap the RFIC
clock to the link-speed streaming budget for 2T2R SC8: 122.88MHz with `1000basex` and 245.76MHz
with `2500basex`. PCIe builds keep the full 245.76MHz/491.52MHz non-oversample/oversample options.
See [RFIC Transport Formats](rfic-transport-formats.md) for the SC16/SC8 trade-offs.

## Build And Bring-Up

1000BASE-X (default PHY):

```bash
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build --load
ping 192.168.1.50
```

2.5GBASE-X:

```bash
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --eth-phy=2500basex --build --load
ping 192.168.1.50
```

The 2.5GBASE-X mode has been hardware-validated in SFP0/J3 with a LianGuo LG 2.5GE copper SFP, the
Acorn Baseboard Mini's JP1 and JP4 fitted, and the board reachable at `192.168.1.50`.
`--eth-phy=2500basex` selects the 125MHz GTP reference, MMCM PHY clocking, Clause-37 timing, and
gearbox constraints needed by the copper module. SFP EEPROM I2C is not required for link or packet
traffic. On M2SDR r02, fit `R82`/`R83` only when the optional M.2 SMBus path is needed; an absent
or NACKing EEPROM must not be treated as a link failure.

Then use the host tools with `-i 192.168.1.50` / `--device eth:192.168.1.50:1234`, or SoapySDR with
`driver=LiteXM2SDR,eth_ip=192.168.1.50`.

## Validating The Stream Path

After loading an Ethernet image, use the `m2sdr_util` loopback tests to exercise the stream path
before starting SoapySDR/GQRX:

```bash
cd litex_m2sdr/software/user
make m2sdr_util
./m2sdr_util -i 192.168.1.50 --duration 4 --pace=rx --sample-rate 1920000 --window 32 fpga-phy-loopback-test
./m2sdr_util -i 192.168.1.50 --duration 8 --pace=rx --sample-rate 1920000 --window 32 ad9361-loopback-test
```

The loopback tests reset FPGA stream state at startup/cleanup, so they can be run after
SoapySDR/GQRX sessions. Add `--verbose` to show detailed RF setup logs and LiteEth counters. See
[Ethernet Loopback Diagnostics](debugging-guide.md#ethernet-loopback-diagnostics) for the full
workflow.

## Combining With Other Features

- **SATA**: Ethernet-only baseboard builds can also enable SATA storage with `--with-sata`, using
  SATA on PCIe lane 0 and Ethernet on the selected SFP lane. PCIe, Ethernet/White-Rabbit, and SATA
  cannot all be enabled in one image because the shared QPLL exposes two channels. See
  [SATA Workflows](sata-workflows.md).
- **PTP**: `--with-eth --with-eth-ptp` makes LiteEth PTP discipline the existing `time_gen`
  timebase instead of replacing it. This keeps PPS generation, VRT timestamps, RX/TX headers, and
  the PCIe PTM/PHC view on the same logical board clock while sourcing that time from Ethernet PTP.
  While PTP discipline is active, host-side time writes are rejected to avoid two masters steering
  the same clock. Ethernet PTP and White Rabbit are mutually exclusive. On SI5351C boards,
  `--with-eth-ptp-rfic-clock` adds an optional low-bandwidth PTP-to-FPGA-10MHz discipline loop;
  software must still select the FPGA clock input with `--sync fpga` / `clock_source=fpga` before
  the AD9361 reference is derived from that path. See [Ethernet PTP Bring-Up](ptp/README.md).
- **VRT**: `--with-eth --with-eth-vrt` enables an Ethernet RX VRT UDP streamer in hardware
  (`--vrt-dst-ip`, `--vrt-dst-port`). A simple host receiver is available at
  `litex_m2sdr/software/user/m2sdr_vrt_rx.py`.
