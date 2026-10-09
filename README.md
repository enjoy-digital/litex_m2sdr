                            __   _ __      _  __    __  ______  _______  ___
                           / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                          / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                         /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                                  LiteX based M2 SDR FPGA board.
                               Copyright (c) 2024-2026 Enjoy-Digital.

[![](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml/badge.svg)](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml) ![License](https://img.shields.io/badge/License-BSD%202--Clause-orange.svg) [![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/enjoy-digital/litex_m2sdr) [![Buy Hardware](https://img.shields.io/badge/Buy-Hardware-00A6B2)](https://enjoy-digital-shop.myshopify.com/)

**LiteX-M2SDR** is an open-source SDR board in the **M.2 2280 Key M** form factor, pairing an
**ADI AD9361** RFIC (2T2R, 70 MHz – 6 GHz) with a **Xilinx Artix-7 XC7A200T** FPGA. Gateware,
drivers, C API and tools are all open, built on the [LiteX](https://github.com/enjoy-digital/litex)
framework. Pop it in an M.2 slot, connect antennas, and stream or record I/Q in about 5 minutes
with our tools or any SoapySDR-compatible software.

<div align="center">
  <img src="https://github.com/user-attachments/assets/c3007b14-0c55-4863-89fa-749082692b4f" alt="LiteX M2 SDR annotated" width="100%">
</div>

## Highlights

- **2T2R, 12-bit up to 61.44 MSPS** — and up to **122.88 MSPS with ~100 MHz of analog bandwidth**
  ([Wide Analog Bandwidth](doc/wide-bandwidth.md)).
- **PCIe Gen2 x4** (~14 Gbps) with [LitePCIe](https://github.com/enjoy-digital/litepcie): MMAP +
  DMA streaming, Linux driver, [low-latency mode](doc/low-latency.md) and
  [hardware timed TX](doc/timed-tx.md).
- **Ethernet 1G/2.5G** ([LiteEth](https://github.com/enjoy-digital/liteeth)) and **SATA** record/replay
  ([LiteSATA](https://github.com/enjoy-digital/litesata)) on the LiteX Acorn Baseboard Mini
  ([Ethernet](doc/ethernet.md), [SATA](doc/sata-workflows.md)).
- **Timing & sync**: external 10 MHz / PPS, [PCIe PTM](doc/pcie-ptm.md),
  [Ethernet PTP](doc/ptp/README.md), [White Rabbit](doc/white-rabbit.md).
- **Software**: [`libm2sdr` C API](litex_m2sdr/doc/libm2sdr/README.md),
  [CLI utilities](litex_m2sdr/software/user/README.md),
  [SoapySDR driver](litex_m2sdr/software/soapysdr/README.md) (GQRX, GNU Radio, srsRAN, ...).
- **Room to grow**: the base design uses only a fraction of the XC7A200T, leaving space for your
  own RF processing; multiboot for safe remote updates; powerful debug through LiteX
  [host bridges](https://github.com/enjoy-digital/litex/wiki/Use-Host-Bridge-to-control-debug-a-SoC)
  and [LiteScope](https://github.com/enjoy-digital/litescope).

## Contents

1. [Why Another AD936x SDR?](#why-another-ad936x-sdr)
2. [Hardware & Availability](#hardware--availability)
3. [Capabilities](#capabilities)
4. [Quick Start](#quick-start)
5. [Architecture](#architecture)
6. [Documentation](#documentation)
7. [Contact](#contact)

## Why Another AD936x SDR?

We know what you'll first ask when discovering this project: what's the RFIC? 🤔 Yes — another
**AD936x**-based SDR! 😄

We've been designing FPGA-based projects for clients with this chip for almost 10 years and still
think it has capabilities that haven't been fully tapped by open-source projects. Paired with the
[LiteX](https://github.com/enjoy-digital/litex) framework, it makes for a minimalist, flexible SDR:
a compact M.2 board with a minimal on-board RF frontend that can be specialized externally, a large
FPGA, and SerDes lanes that can be used for PCIe, Ethernet, SATA or inter-board links
([LiteICLink](https://github.com/enjoy-digital/liteiclink)), down to coherent multi-board MIMO
setups on a PCIe M.2 carrier. 🚀

<div align="center">
  <img src="https://github.com/user-attachments/assets/dec9bbd6-532d-4596-805b-94078df426a2" width="100%">
</div>

Yes, this project is also a showcase for LiteX capabilities 😅 — rest assured, we'll do our best
to gather and implement your requests to make this SDR as flexible and versatile as possible, and
to keep a welcoming, friendly community. 🤗

This board is proudly developed in France 🇫🇷 by [Enjoy-Digital](http://enjoy-digital.fr/),
managing the project and gateware/software development, and our partner
[Lambdaconcept](https://shop.lambdaconcept.com/) designing the hardware. 🥖🍷

## Hardware & Availability

The LiteX-M2SDR board is available from the
[Enjoy-Digital Shop](https://enjoy-digital-shop.myshopify.com). It has been tested with several
SoapySDR-compatible applications as well as with our own C utilities.

Two variants are offered, both in the same **M.2 2280 Key M** form factor:

- **SI5351C variant** — flexible clocking from the local XO or an external 10 MHz reference (uFL
  or FPGA-generated). **Recommended for general usage.**
  [More details](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351c)
- **SI5351B variant** — clocked from the local XO with an FPGA-controlled VCXO for
  software-regulated loops; mostly for advanced users with specialized clock control requirements.
  [More details](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351b)

The board fits directly into an M.2 slot. Mounted on the **LiteX Acorn Baseboard Mini**, it also
gets 1000BASE-X/2500BASE-X Ethernet (SFP) and SATA to record/play samples directly to/from an SSD:

<div align="center">
  <img src="https://github.com/user-attachments/assets/fb75aeeb-4e99-45b5-9582-0c4dbd079af6" width="100%">
</div>

Pinout, I/O voltages, PPS input and LED behavior: see the [Hardware Reference](doc/hardware.md).

## Capabilities

| Feature                          | Mounted in M.2 Slot         | Mounted in Baseboard         | Parameter(s) to Enable                        |
|----------------------------------|------------------------------|-----------------------------|-----------------------------------------------|
| **SDR Functionality**           |                              |                              |                                               |
| SDR TX (AD9361)                 | ✅                           | ✅                           | (always included)                             |
| SDR RX (AD9361)                 | ✅                           | ✅                           | (always included)                             |
| Oversampling (122.88MSPS)       | ✅  (PCIe Gen2 x2/x4 only)   | ❌                           | `--with-pcie --pcie-lanes=2|4`                |
| C API + Utilities               | ✅                           | ✅                           | (included in software build)                  |
| SoapySDR Support                | ✅                           | ✅                           | (via optional SoapySDR driver)                |
|                                 |                              |                              |                                               |
| **Connectivity**                |                              |                              |                                               |
| PCIe (up to Gen2 x4)            | ✅                           | ✅ (x1 only)                 | `--with-pcie --pcie-lanes=1|2|4`              |
| Ethernet (1G/2.5G)              | ❌                           | ✅                           | `--with-eth`                                  |
| ├─ Ethernet RX (LiteEth)        | ❌                           | ✅                           | (included with `--with-eth`)                  |
| └─ Ethernet TX (LiteEth)        | ❌                           | ✅                           | (included with `--with-eth`)                  |
|                                 |                              |                              |                                               |
| **Timing & Sync**               |                              |                              |                                               |
| PTM (Precision Time Measurement)| ✅ (PCIe Gen2 x1 only)       | ✅ (PCIe Gen2 x1 only)       | `--with-pcie --pcie-lanes=1 --with-pcie-ptm`  |
| Ethernet PTP Time Discipline    | ❌                           | ✅                           | `--with-eth --with-eth-ptp`                   |
| Ethernet PTP RFIC Ref Clock     | ❌                           | ✅                           | `--with-eth --with-eth-ptp --with-eth-ptp-rfic-clock` |
| White Rabbit Support            | ❌                           | ✅                           | `--with-white-rabbit`                         |
| External Clocking               | ✅ (SI5351C: ext. 10MHz)     | ✅ (SI5351C: ext. 10MHz)     | (SI5351B VCXO mode in dev for PTM regulation) |
|                                 |                              |                              |                                               |
| **Storage**                     |                              |                              |                                               |
| SATA                            | ❌                           | ✅ (source build)            | `--with-sata`                                 |
|                                 |                              |                              |                                               |
| **System Features**             |                              |                              |                                               |
| Multiboot / Remote Update       | ✅                           | ✅                           | (always included)                             |
| GPIO                            | ✅                           | ✅                           | (always included)                             |

Build flags are passed to `./litex_m2sdr.py`; see
[Building And Loading The Gateware](doc/building-gateware.md). Prebuilt images for the common
configurations are published on the [Releases](https://github.com/enjoy-digital/litex_m2sdr/releases)
page and can be flashed with [`scripts/flash_release.py`](doc/flash-release.md).

## Quick Start

On a fresh Ubuntu system, with the board in an M.2 slot and antennas connected:

1. **Install the prerequisites:**
   ```bash
   sudo apt install build-essential cmake git \
     pkg-config libsdl2-dev libgl1-mesa-dev \
     libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
     gnuradio gnuradio-dev libgnuradio-soapy3.10.9t64 gqrx-sdr \
     libsndfile1-dev libsamplerate0-dev
   ```

2. **Clone, build and install** the kernel driver, utilities, `libm2sdr` and SoapySDR module:
   ```bash
   git clone https://github.com/enjoy-digital/litex_m2sdr
   cd litex_m2sdr/litex_m2sdr/software
   sudo ./build.py
   sudo modprobe m2sdr   # or reboot
   ```

3. **Check the board** is detected:
   ```bash
   cd user
   ./m2sdr_util info
   SoapySDRUtil --probe="driver=LiteXM2SDR"
   ```

4. **Launch your SDR software** (GQRX, GNU Radio, ...) and select the LiteX-M2SDR device through
   SoapySDR. 📡

> [!TIP]
> No I/Q samples in your SDR application? Set the IOMMU to passthrough mode (`iommu=pt` on x86).
> Overflows/underflows at high sample rates? Switch the CPU governor to `performance`.
> Details in [Host Setup](doc/host-setup.md).

More: [build options and manual install](doc/building-software.md) ·
[Raspberry Pi 5](doc/hosts/raspberry-pi-5.md) · [Orange Pi 5 Max](doc/hosts/orangepi-5-max.md) ·
[Jetson Orin](doc/hosts/jetson-orin.md) · [Ethernet setup](doc/ethernet.md) ·
[C API](litex_m2sdr/doc/libm2sdr/README.md).

## Architecture

### PCIe SoC

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/df5eb55e-16b2-4724-b4c1-28e06c45279c" width="100%">
</div>

The PCIe design is the default: no baseboard required. Most of the complexity is handled by LiteX
and LitePCIe; the SoC itself exposes an MMAP interface and DMA streams, and integrates the SDR/RFIC
cores. [LitePCIe](https://github.com/enjoy-digital/litepcie) and its Linux driver have been
battle-tested on several commercial projects. The design is validated at 2T2R @ 61.44 MSPS and
handles 2T2R @ 122.88 MSPS oversampling (7.9 Gbps on the PCIe bus). Debug is available over PCIe or
JTAG (MMAP peek & poke, LiteScope).

### Ethernet SoC

<div align="center">
  <img src="https://github.com/user-attachments/assets/bbcc0c79-4ae8-4e5b-94d8-aa7aff89bae2" width="100%">
</div>

On the Acorn Baseboard Mini, the M.2 SerDes lanes can be used for Ethernet instead of PCIe:
[LiteEth](https://github.com/enjoy-digital/liteeth) provides the 1000BASE-X/2500BASE-X PHY, the
UDP/IP hardware stack and the streaming/Etherbone front-ends, for control and RX/TX sample streaming
bounded by the link bandwidth. See [Ethernet](doc/ethernet.md).

## Documentation

The full index is in [`doc/README.md`](doc/README.md).

| Getting started | Using the board | Features | Timing & sync | Development |
|---|---|---|---|---|
| [Building the software](doc/building-software.md) | [User utilities](litex_m2sdr/software/user/README.md) | [Ethernet](doc/ethernet.md) | [PCIe PTM](doc/pcie-ptm.md) | [Building the gateware](doc/building-gateware.md) |
| [Host setup](doc/host-setup.md) | [SoapySDR driver](litex_m2sdr/software/soapysdr/README.md) | [SATA](doc/sata-workflows.md) | [Ethernet PTP](doc/ptp/README.md) | [Debugging guide](doc/debugging-guide.md) |
| [Flashing releases](doc/flash-release.md) | [GNU Radio](litex_m2sdr/software/gnuradio/README.md) | [Wide bandwidth](doc/wide-bandwidth.md) | [White Rabbit](doc/white-rabbit.md) | [Hardware reference](doc/hardware.md) |
| [Platform guides](doc/host-setup.md) | [libm2sdr C API](litex_m2sdr/doc/libm2sdr/README.md) | [Low latency](doc/low-latency.md) | | [Changelog](CHANGELOG.md) |
| | [Kernel driver](litex_m2sdr/software/kernel/README.md) | [Timed TX](doc/timed-tx.md) | | |

## Contact

Got a unique idea or need a tweak? Whether it's custom FPGA/software development or hardware
adjustments (like adapter boards) for your LiteX-M2SDR, we're here to help! Feel free to drop us a
line or visit our website. We'd love to hear from you!

E-mail: florent@enjoy-digital.fr
Website: http://enjoy-digital.fr/

<div align="center">
  <img src="https://github.com/user-attachments/assets/1cf8a5fd-a9bb-4efe-9e50-24eb944bd971" width="100%">
</div>
