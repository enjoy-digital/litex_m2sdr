                            __   _ __      _  __    __  ______  _______  ___
                           / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                          / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                         /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                                  LiteX based M2 SDR FPGA board.
                               Copyright (c) 2024-2026 Enjoy-Digital.

[![](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml/badge.svg)](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml) ![License](https://img.shields.io/badge/License-BSD%202--Clause-orange.svg) [![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/enjoy-digital/litex_m2sdr) [![Buy Hardware](https://img.shields.io/badge/Buy-Hardware-00A6B2)](https://enjoy-digital-shop.myshopify.com/)

[> TL;DR
---------
- **What?** A compact M.2 2280 SDR combining a Xilinx **Artix-7 XC7A200T** FPGA and **ADI AD9361** RFIC.
- **Why?** Open-source gateware + software stack with real FPGA headroom for custom DSP pipelines.
- **Performance?** 2T2R, 12-bit up to 61.44 MSPS (122.88 MSPS with PCIe Gen2 x2/x4 oversampling mode).
- **How fast to first signal?** Install deps, run `./build.py`, probe with `SoapySDRUtil`.

<div align="center">
  <img src="https://github.com/user-attachments/assets/c3007b14-0c55-4863-89fa-749082692b4f" alt="LiteX M2 SDR annotated" width="100%">
</div>

[> Why LiteX-M2SDR
------------------
<a id="why-litex-m2sdr"></a>

- **Open internals**: complete gateware/software stack in one repository.
- **Serious IO in small form factor**: PCIe Gen2 x1/x2/x4, optional Ethernet and SATA variants.
- **FPGA-first SDR platform**: base design leaves room for custom RF processing.
- **Production-proven building blocks**: based on LiteX/LitePCIe/LiteEth/LiteSATA ecosystem.
- **Flexible control and debug**: MMAP, DMA, LiteScope, JTAG/PCIe host bridges.

[> Status At A Glance
---------------------
<a id="status"></a>

| Area | Status | Notes |
|------|--------|-------|
| PCIe SDR (AD9361 TX/RX) | Stable | Main deployment path on M.2 hosts. |
| C API + user utilities | Stable | Built in CI (`software-build` job). |
| SoapySDR integration | Stable | Build-tested in CI. |
| Gateware simulation tests | Stable | `pytest -v test` in CI. |
| Ethernet SoC | Beta | RX path available, TX still evolving. |
| White Rabbit support | Beta | Available on baseboard-oriented configurations. |
| SATA stream path | Beta | Available on baseboard-oriented configurations. |

[> Common Use Cases
-------------------
<a id="use-cases"></a>

- SDR prototyping with GNU Radio / SoapySDR applications.
- FPGA offload experiments (filters, packetization, framing, timing).
- Host-controlled record/replay and DMA stress testing.
- Precision timing experiments with PTM-capable PCIe setups.
- Ethernet/SATA pipeline exploration on compatible baseboards.

[> First Success In 5 Minutes
-----------------------------
<a id="first-success"></a>

```bash
# 1) Install prerequisites (Ubuntu example)
sudo apt install build-essential cmake git \
  libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
  libsndfile1-dev libsamplerate0-dev

# 2) Clone + build software stack
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr/litex_m2sdr/software
./build.py

# 3) Load kernel module
cd kernel
sudo make install
sudo insmod m2sdr.ko

# 4) Probe device through SoapySDR
SoapySDRUtil --probe="driver=LiteXM2SDR"
```

[> Contents
-----------

1. [Hardware Availability](#hardware-availability)
2. [Capabilities Overview](#capabilities-overview)
3. [Getting Started by Audience](#quick-start)
4. [Host Requirements and Compatibility](#host-requirements)
5. [Troubleshooting](#troubleshooting)
6. [Architecture Notes](#architecture)
7. [Contact](#contact)

[> Choose Your Path
-------------------

- New user, first RF signal: [SDR Users](#1-sdr-users-fastest-path)
- Software integration/testing: [Software Developers](#2-software-developers)
- FPGA/gateware customization: [Gateware / FPGA Developers](#3-gateware--fpga-developers)

[> Hardware Availability
------------------------
<a id="hardware-availability"></a>

The LiteX-M2SDR board is available from the [Enjoy-Digital Shop](https://enjoy-digital-shop.myshopify.com).

Two hardware variants are offered:
- **SI5351C variant**: Flexible clocking (local XO or external 10MHz via FPGA/uFL). Recommended for most users.
  [Product page](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351c)
- **SI5351B variant**: Local XO + FPGA-controlled VCXO loops for specialized timing use cases.
  [Product page](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351b)

[> Capabilities Overview
------------------------
<a id="capabilities-overview"></a>

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
| Ethernet RX (LiteEth)           | ❌                           | ✅                           | (included with `--with-eth`)                  |
| Ethernet TX (LiteEth)           | ❌                           | ⚠️ (in development)          | (included with `--with-eth`)                  |
|                                 |                              |                              |                                               |
| **Timing & Sync**               |                              |                              |                                               |
| PTM (Precision Time Measurement)| ✅ (PCIe Gen2 x1 only)       | ✅ (PCIe Gen2 x1 only)       | `--with-pcie --pcie-lanes=1 --with-pcie-ptm`  |
| White Rabbit Support            | ❌                           | ✅                           | `--with-white-rabbit`                         |
| External Clocking               | ✅ (SI5351C: ext. 10MHz)     | ✅ (SI5351C: ext. 10MHz)     | (SI5351B VCXO mode in dev for PTM regulation) |
|                                 |                              |                              |                                               |
| **Storage**                     |                              |                              |                                               |
| SATA                            | ❌                           | ⚠️ (in development)          | `--with-sata`                                 |
|                                 |                              |                              |                                               |
| **System Features**             |                              |                              |                                               |
| Multiboot / Remote Update       | ✅                           | ✅                           | (always included)                             |
| GPIO                            | ✅                           | ✅                           | (always included)                             |

[> Getting Started By Audience
------------------------------
<a id="quick-start"></a>

### 1) SDR Users (Fastest Path)

1. Install prerequisites (Ubuntu example):
```bash
sudo apt install build-essential cmake git \
  libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
  gnuradio gnuradio-dev libgnuradio-soapy3.10.9t64 gqrx-sdr \
  libsndfile1-dev libsamplerate0-dev
```

2. Build/install software stack:
```bash
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr/litex_m2sdr/software
./build.py
```

3. Load the kernel module:
```bash
cd kernel
sudo make install
sudo insmod m2sdr.ko
```

4. Check device visibility:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR"
```

5. Launch GNU Radio test flowgraph:
```bash
cd ../gnuradio
gnuradio-companion test_fm_rx.grc
```

### 2) Software Developers

- CI-safe gateware simulation tests:
```bash
pytest -v test
```

- Hardware control/debug scripts (board required):
```bash
python3 scripts/test_xadc.py
python3 scripts/test_dashboard.py
```

- Build checks mirrored by CI:
```bash
cd litex_m2sdr/software/kernel && make clean all
cd ../user && make clean all
cd ../soapysdr && cmake -S . -B build && cmake --build build
```

- Basic software smoke checks:
```bash
cd litex_m2sdr/software/user
./m2sdr_util info
./m2sdr_rf init -samplerate=30720000
./tone_gen.py tone_tx.bin
./m2sdr_play tone_tx.bin 100000
```

### 3) Gateware / FPGA Developers

1. Install LiteX toolchain and dependencies:
- [LiteX Installation Guide](https://github.com/enjoy-digital/litex/wiki/Installation)

2. Build/load common variants:
```bash
# PCIe, M.2 mounted board
./litex_m2sdr.py --with-pcie --variant=m2 --build --load

# PCIe, baseboard setup
./litex_m2sdr.py --with-pcie --variant=baseboard --build --load

# Ethernet baseboard variant
./litex_m2sdr.py --with-eth --eth-sfp=0 --build --load
```

3. Use host bridges for debug/control:
```bash
litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg
sudo litex_server --pcie --pcie-bar=04:00.0
```

4. Flash over PCIe:
```bash
cd litex_m2sdr/software
./flash.py ../build/litex_m2sdr_platform/litex_m2sdr/gateware/litex_m2sdr_platform.bin
```

[> Host Requirements and Compatibility
--------------------------------------
<a id="host-requirements"></a>

- **IOMMU / DMA**: for PCIe streaming, use IOMMU passthrough mode.
- **PCIe lanes**: 122.88 MSPS oversampling requires PCIe Gen2 x2/x4.
- **Linux-first support**: software stack and drivers target Linux hosts.
- **Platform guides**:
  - [OrangePi 5 Max guide](doc/hosts/orangepi-5-max.md)
  - [Raspberry Pi 5 guide](doc/hosts/raspberry-pi-5.md)

### Platform Notes

| Platform Family | Notes |
|----------------|-------|
| x86 Linux hosts | Main PCIe deployment target. |
| Raspberry Pi 5 | Dedicated setup guide available (see link above). |
| OrangePi 5 Max | Dedicated setup guide available (see link above). |
| LattePanda-class mini PCs | Supported in practice; PCIe re-training workaround logic is available in gateware for difficult link bring-up cases. |

### What CI Validates

Each push/pull request currently validates:
- Gateware simulation suite: `pytest -v test`
- Linux kernel driver compilation: `litex_m2sdr/software/kernel`
- User-space utilities compilation: `litex_m2sdr/software/user`
- SoapySDR module compilation: `litex_m2sdr/software/soapysdr`

[> Troubleshooting
------------------
<a id="troubleshooting"></a>

- **No I/Q stream in SDR app**:
  Enable IOMMU passthrough.

  x86/PC (`/etc/default/grub`):
  ```bash
  GRUB_CMDLINE_LINUX="iommu=pt"
  sudo update-grub && sudo reboot
  ```

  ARM (`/boot/extlinux/extlinux.conf`):
  ```bash
  APPEND ... iommu.passthrough=1 arm-smmu.disable=1
  sudo reboot
  ```

- **Intel host kernel panic (`Corrupted page table`)**:
  Add `intel_iommu=off` to `GRUB_CMDLINE_LINUX`.

- **DKMS conflict during package install**:
  ```bash
  sudo apt remove --purge xtrx-dkms dkms
  ```

- **Kernel-side debug logs**:
  ```bash
  sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
  ```

[> Architecture Notes
---------------------
<a id="architecture"></a>

### PCIe SoC Design

This is the primary deployment mode and does not require an external baseboard.

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/df5eb55e-16b2-4724-b4c1-28e06c45279c" width="100%">
</div>

The design uses LiteX + LitePCIe and provides:
- host MMAP control,
- DMA streaming interfaces,
- optional debug over PCIe/JTAG.

### Ethernet SoC Design (WIP)

Ethernet variant targets baseboard deployments and currently focuses on RX path bring-up.

<div align="center">
  <img src="https://github.com/user-attachments/assets/bbcc0c79-4ae8-4e5b-94d8-aa7aff89bae2" width="100%">
</div>

[> Contact
----------
<a id="contact"></a>

For custom FPGA/software development or hardware adaptations around LiteX-M2SDR:

E-mail: florent@enjoy-digital.fr
Website: http://enjoy-digital.fr/

<div align="center">
  <img src="https://github.com/user-attachments/assets/1cf8a5fd-a9bb-4efe-9e50-24eb944bd971" width="100%">
</div>
