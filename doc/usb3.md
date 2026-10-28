# USB3

USB3 support (SuperSpeed, 5Gbps) is intended for LiteX Acorn Baseboard Mini deployments: one of
the baseboard SFP cages is used as a USB3 port through an SFP-to-USB3 adapter and the USB3 device
is fully implemented in the FPGA with [USB3-PIPE](https://github.com/enjoy-digital/usb3_pipe)
(no external USB chip).

> **Status: experimental, not yet validated on hardware.** USB3-PIPE itself is validated on the
> Acorn CLE-215+ (same Artix7 GTP, same SFP2USB adapter); the M2SDR integration, libm2sdr backend
> and SoapySDR support are new.

## Overview

The 4 GTP transceivers of the M.2 connector are classical Artix7 transceivers: as for Ethernet and
SATA, one of them (routed to a baseboard SFP cage) is used for USB3 instead of PCIe. A Xillybus
[SFP2USB](http://xillybus.com/sfp2usb-module) adapter turns the SFP cage into a USB3 port, connected
to the host with a USB3 cable.

The USB3 device (vendor class, `1d50:605a` by default) provides:

| Endpoint | Use |
|----------|-----|
| EP0 (Control) | CSR/SoC bus access through vendor requests (LiteX `CommUSB` protocol, `litex_server --usb`), with burst extension (up to 128 words per request). |
| EP1 Bulk IN | RFIC RX samples (crossbar port 3), 8KB frames (optional 16-byte header + samples), as with PCIe. |
| EP2 Bulk OUT | RFIC TX samples (crossbar port 3), 256KB FIFO primed (half full) before feeding the RFIC. |

The USB3 PLL is the second PLL of the shared GTP QPLL (100MHz oscillator through a BUFG on
GTGREFCLK, 2.5GHz VCO), so USB3 can be combined with PCIe Gen2 X1 (baseboard M.2 lane 3), but not
with Ethernet, SATA or White Rabbit (yet).

## Bandwidth

USB3-PIPE reaches ~446MB/s IN / ~449MB/s OUT (~376 + 400MB/s full duplex) on Linux xHCI hosts:

| Configuration | Per direction | USB3 |
|---------------|---------------|------|
| 1T1R or 2T2R @ 30.72MSPS, SC16 | 245MB/s | OK (full duplex) |
| 2T2R @ 61.44MSPS, SC8 (`bitmode=8`) | 245MB/s | OK (full duplex) |
| 2T2R @ 61.44MSPS, SC16 | 491MB/s | Too high |

SoapySDR defaults to SC8 above 40MSPS on USB (`bitmode=16` to override). See
[RFIC Transport Formats](rfic-transport-formats.md) for the SC16/SC8 trade-offs.

## Build And Bring-Up

Requirements: [USB3-PIPE](https://github.com/enjoy-digital/usb3_pipe) with the EP0 Wishbone bridge
(`usb3_core.wishbone`) and external PLL support, LiteICLink with the GTP fixes required by
USB3-PIPE (see its debugging guide).

```bash
# USB3 only (SFP0):
./litex_m2sdr.py --variant=baseboard --with-usb3 --usb3-sfp=0 --build --load

# USB3 + PCIe Gen2 X1 (PCIe also available for bring-up/debug):
./litex_m2sdr.py --variant=baseboard --with-pcie --with-usb3 --with-usb3-debug --build --load

lsusb -t       # 5000M link.
lsusb -d 1d50:605a
```

`--with-usb3-debug` adds the USB3-PIPE PHY debug CSRs (LTSSM state/history, SerDes counters),
readable over PCIe, JTAGBone or USB itself. `--usb3-vid/--usb3-pid` change the USB IDs.

User access to the device (udev):

```bash
sudo cp litex_m2sdr/software/99-litex-m2sdr-usb3.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Large asynchronous transfers need more usbfs memory than the Linux default (16MB) when RX and TX
run together with other USB devices:

```bash
echo 64 | sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb
```

## Host Access

CSRs with the LiteX tools (pyusb):

```bash
litex_server --usb --usb-vid 0x1d50 --usb-pid 0x605a
litex_cli --regs
```

Utilities and libm2sdr with the `usb:` device identifier:

| Identifier | Device |
|------------|--------|
| `usb:` | First `1d50:605a` device. |
| `usb:1d50:605a` | First device with this VID:PID. |
| `usb:serial=<serial>` | Device with this USB serial string. |
| `usb:path=2-1.3` | Device on this USB bus/port path (as in sysfs). |

```bash
cd litex_m2sdr/software/user
make                                # LiteUSB built when libusb-1.0 is found (LITEUSB=0 to disable).
./m2sdr_util -d usb: info
./m2sdr_rf -d usb: --sample-rate 30.72e6
./m2sdr_record -d usb: capture.bin 100000000
```

SoapySDR: `driver=LiteXM2SDR,usb=` (or `usb=serial=...`), USB devices are also listed by
`SoapySDRUtil --find` (`usb_discovery=0` to disable).

## Implementation Notes

- Gateware: `litex_m2sdr/gateware/usb3.py` (SerDes on a `SharedQPLL` channel, USB3-PIPE PHY + Device
  Core, EP0 Wishbone bridge, RX/TX streamers), `usb3` CSRs (`control`: `rx_enable`/`tx_enable`,
  `status`: `ready`/`link_up`/`tx_started`), capability feature bit 10 and `board_info.usb3_sfp`.
- RX (re)start: `rx_enable` flushes the Bulk IN packets of a previous run and aligns the RX header
  on the stream start, then the host clears the endpoint halt (Sequence Numbers reset, packets sent
  before dropped): the first received buffer always starts a frame.
- Host: `litex_m2sdr/software/user/libliteusb` (libusb-1.0, Control transfers for CSRs, buffer rings
  over asynchronous Bulk transfers), `m2sdr_hal_liteusb.c` and the `M2SDR_TRANSPORT_LITEUSB`
  branches of `m2sdr_stream.c`. Legacy raw handles (`m2sdr_get_handle()`) accept LiteUSB
  connections.
- Not yet supported over USB: hardware timed TX (SoapySDR falls back to the software timeline),
  RX overflow/TX underflow reporting (the RFIC FIFOs absorb the host jitter, a stalled host
  overflows them silently).
