# LiteX-M2SDR Documentation

Back to the [project README](../README.md).

## Getting Started

| Page | Description |
|------|-------------|
| [Building And Installing The Software](building-software.md) | Kernel driver, utilities, `libm2sdr`, SoapySDR module: build, install, dev builds. |
| [Host Setup](host-setup.md) | IOMMU, CPU governor, PCIe lanes, runtime PCIe/Ethernet device selection. |
| [Raspberry Pi 5](hosts/raspberry-pi-5.md) · [Orange Pi 5 Max](hosts/orangepi-5-max.md) · [Jetson Orin](hosts/jetson-orin.md) | Platform-specific setup guides. |
| [Flashing Release Images](flash-release.md) | Flash a prebuilt release over JTAG with `scripts/flash_release.py`. |

## Using The Board

| Page | Description |
|------|-------------|
| [User-Space Utilities](../litex_m2sdr/software/user/README.md) | `m2sdr_util`, `m2sdr_rf`, `m2sdr_record`, `m2sdr_play`, `m2sdr_sata`, ... |
| [SoapySDR Driver](../litex_m2sdr/software/soapysdr/README.md) | Device arguments, configuration knobs, srsRAN. |
| [GNU Radio Examples](../litex_m2sdr/software/gnuradio/README.md) | Ready-to-run flowgraphs. |
| [libm2sdr C API](../litex_m2sdr/doc/libm2sdr/README.md) | Public C API, examples, versioning. |
| [Kernel Driver](../litex_m2sdr/software/kernel/README.md) | Build, install, module usage. |

## Features

| Page | Description |
|------|-------------|
| [Ethernet](ethernet.md) | 1000BASE-X/2500BASE-X streaming on the Acorn Baseboard Mini, VRT. |
| [SATA Workflows](sata-workflows.md) | Record/replay I/Q to/from an SSD with `m2sdr_sata`. |
| [SATA Hardware Validation](sata-validation.md) | Measured SATA throughput and validity results. |
| [Wide Analog Bandwidth](wide-bandwidth.md) | 122.88 MSPS RFIC overclock with ~100 MHz analog bandwidth. |
| [Low-Latency Streaming](low-latency.md) | Shallow DMA ring, RX wake, TX fill lead for real-time loops. |
| [Hardware Timed TX](timed-tx.md) | Deterministic on-air transmit timing from buffer timestamps. |
| [RFIC Transport Formats](rfic-transport-formats.md) | SC16/SC8 sample packing trade-offs. |

## Timing And Synchronization

| Page | Description |
|------|-------------|
| [PCIe PTM Host Time Sync](pcie-ptm.md) | Board time following the host clock over PCIe. |
| [Ethernet PTP Bring-Up](ptp/README.md) | LiteEth PTP time discipline and PTP-referenced RFIC clock. |
| [White Rabbit](white-rabbit.md) | White Rabbit integration on the Acorn Baseboard Mini. |

## Development

| Page | Description |
|------|-------------|
| [Building And Loading The Gateware](building-gateware.md) | Build variants, host bridges, flashing over PCIe, release archives. |
| [Debugging Guide](debugging-guide.md) | Hardware/software debug workflow, LiteScope, regression tests. |
| [Changelog](../CHANGELOG.md) | Release history. |

## Hardware Reference

| Page | Description |
|------|-------------|
| [Hardware Reference](hardware.md) | M.2 keying, I/O voltages, sideband pinout, PPS input, user LED. |
| [AD9361 documents](ad9361/) · [SI5351 documents](si5351/) | Vendor datasheets and application notes. |
| [Acorn Baseboard Mini schematic](litex_acorn_baseboard_mini_schematic.pdf) | Baseboard schematic. |

## Engineering Notes

Dated investigations kept for reference: [SATA bandwidth investigation](notes/sata-bandwidth-investigation.md).
