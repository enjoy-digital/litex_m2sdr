                            __   _ __      _  __    __  ______  _______  ___
                           / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                          / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                         /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                                  LiteX based M2 SDR FPGA board.
                               Copyright (c) 2024-2026 Enjoy-Digital.

[![](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml/badge.svg)](https://github.com/enjoy-digital/litex_m2sdr/actions/workflows/ci.yml) ![License](https://img.shields.io/badge/License-BSD%202--Clause-orange.svg) [![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/enjoy-digital/litex_m2sdr) [![Buy Hardware](https://img.shields.io/badge/Buy-Hardware-00A6B2)](https://enjoy-digital-shop.myshopify.com/)

[> TL;DR
---------
- **What?** LiteX‑based M.2 2280 SDR board featuring a Xilinx **Artix‑7 XC7A200T** FPGA and an **ADI AD9361** RFIC.
- **Why?** Open‑source gateware/software, up to 61.44 MSPS (122.88 MSPS†) over PCIe Gen2 ×4, hack‑friendly clocking & debug.
- **Who?** SDR tinkerers, FPGA devs, time‑sync enthusiasts, or anyone hitting the limits of other SDRs.
- **How fast?** `apt install …` → `./build.py` → **stream/record IQ in ≈5 min** with our C API/tools or any SoapySDR compatible software.

## C API (libm2sdr)

- Docs: `litex_m2sdr/doc/libm2sdr/README.md`
- Examples: `litex_m2sdr/doc/libm2sdr/example_sync_rx.c`, `litex_m2sdr/doc/libm2sdr/example_sync_tx.c`
- Install metadata: `litex_m2sdr/software/user/libm2sdr/m2sdr.pc`
- Current public library version: `1.0.0` (ABI `1`)
- Recent API additions: backend accessors `m2sdr_get_transport()` / `m2sdr_get_eb_handle()` and finer-grained `parse`/`range`/`state` error classes.

<div align="center">
  <img src="https://github.com/user-attachments/assets/c3007b14-0c55-4863-89fa-749082692b4f" alt="LiteX M2 SDR annotated" width="100%">
</div>

† Oversampling needs PCIe Gen2 ×2/×4 bandwidth.

[> Intro
--------
<a id="intro"></a>

We know what you'll first ask when discovering this new SDR project: what's the RFIC? 🤔 Let's answer straight away: Another **AD936X**-based SDR! 😄

Why yet another SDR based on this RFIC? Because we've been designing FPGA-based projects for clients with this chip for almost 10 years now and still think this RFIC has incredible capabilities and possibilities that haven't been fully tapped by open-source projects. We believe it can provide a fantastic and simple solution when paired with the [LiteX](https://github.com/enjoy-digital/litex) framework we're developing. 🚀

<div align="center">
  <img src="https://github.com/user-attachments/assets/dec9bbd6-532d-4596-805b-94078df426a2" width="100%">
</div>

Imagine a minimalist AD9361-based SDR with:
- A compact form factor (M2 2280). 📏
- Minimal on-board RF frontend that could be specialized externally.
- 2T2R / 12-bit @ 61.44MSPS (and 2T2R / 12-bit @ 122.88MSPS for those wanting to use/explore Cellwizard/BladeRF [findings](https://www.nuand.com/2023-02-release-122-88mhz-bandwidth/)).
- PCIe Gen 2 X4 (~14Gbps of TX/RX bandwidth) with [LitePCIe](https://github.com/enjoy-digital/litepcie), providing MMAP and several possible DMAs (for direct I/Q samples transfer or processed I/Q samples). ⚡
- A large XC7A200T FPGA where the base infrastructure only uses a fraction of the available resources, allowing you to integrate large RF processing blocks. 💪
- The option to reuse some of the PCIe lanes of the M2 connector for 1Gbps or 2.5Gbps Ethernet through [LiteEth](https://github.com/enjoy-digital/liteeth). 🌐
- Or ... for SATA through [LiteSATA](https://github.com/enjoy-digital/litesata). 💾
- Or ... for inter-board SerDes-based communication through [LiteICLink](https://github.com/enjoy-digital/liteiclink). 🔗
- Powerful debug capabilities through LiteX [Host <-> FPGA bridges](https://github.com/enjoy-digital/litex/wiki/Use-Host-Bridge-to-control-debug-a-SoC) and [LiteScope](https://github.com/enjoy-digital/litescope) logic analyzer. 🛠️
- Multiboot support to allow secure remote update over PCIe (or Ethernet).
- ...and we hope a welcoming/friendly community as we strive to encourage in LiteX! 🤗

OK, you probably also realized this project is a showcase for LiteX capabilities, haha. 😅 Rest assured, we'll do our best to gather and implement your requests to make this SDR as flexible and versatile as possible!

This board is proudly developed in France 🇫🇷 by [Enjoy-Digital](http://enjoy-digital.fr/), managing the project and litex_m2sdr/gateware/software development, and our partner [Lambdaconcept](https://shop.lambdaconcept.com/) designing the hardware. 🥖🍷

Ideal for SDR enthusiasts, this versatile board fits directly into an M2 slot or can team up with others in a PCIe M2 carrier for more complex projects, including coherent MIMO SDRs. 🔧

For Ethernet support with 1000BaseX/2500BaseX and SATA connectivity to directly record/play samples to/from an SSD, mount it on the LiteX Acorn Mini Baseboard! 💽

<div align="center">
  <img src="https://github.com/user-attachments/assets/fb75aeeb-4e99-45b5-9582-0c4dbd079af6" width="100%">
</div>

Unlock new possibilities in your SDR projects with this cutting-edge board—we'll try our best to meet your needs! 🎉

[> Contents
-----------

1. [Hardware Availability](#hardware-availability)
2. [Capabilities Overview](#capabilities-overview)
3. [M.2 / GPIO Voltage Levels](#m2-gpio-voltage-levels)
4. [PCIe SoC Design](#pcie-soc-design)
5. [Ethernet SoC Design (WIP)](#ethernet-soc-design)
6. [Quick Start](#quick-start)
7. [Contact](#contact)

[> Hardware Availability
------------------------
<a id="hardware-availability"></a>
The LiteX-M2SDR board is now fully commercialized and available for purchase from our webshop: [Enjoy-Digital Shop](https://enjoy-digital-shop.myshopify.com).

The hardware has been thoroughly tested with several SDR softwares compatible with SoapySDR as well as with our Bare metal C utilities.

*We offer two variants:*
- **SI5351C Variant** – Uses the SI5351C clock generator with flexible clocking (local XO or external 10MHz via FPGA/uFL). **Recommended for general usage.** [More details](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351c)
- **SI5351B Variant** – Uses the SI5351B clock generator, clocked from the local XO with FPGA-controlled VCXO for software-regulated loops. [More details](https://enjoy-digital-shop.myshopify.com/products/litex-m2-sdr-si5351b)

*Note: The differences between the variants are relevant only for specific use cases. The SI5351B variant is mostly intended for advanced users with specialized clock control requirements.*

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
| ├─ Ethernet RX (LiteEth)        | ❌                           | ✅                           | (included with `--with-eth`)                  |
| └─ Ethernet TX (LiteEth)        | ❌                           | ⚠️ (in development)          | (included with `--with-eth`)                  |
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

### User LED Behavior

The board exposes a single monochrome `user_led`, so the gateware uses it as a layered status indicator rather than a simple on/off flag:

- **Not ready yet**: double-heartbeat while time is still invalid or while an enabled PCIe/Ethernet transport is not ready.
  PCIe becomes ready when the link is up and DMA/PPS synchronization is established; Ethernet becomes ready when the link is up.
- **Idle / ready state**: gentle low-amplitude breathing.
- **PPS event**: short bright accent pulse over the base animation.
- **RF or Ethernet RX/TX activity**: bright accent pulse.

When PCIe is not enabled in the build, the PCIe-specific states are naturally skipped and the LED falls back to the generic timing/activity behavior.

[> M.2 / GPIO Voltage Levels
----------------------------
<a id="m2-gpio-voltage-levels"></a>

LiteX-M2SDR does **not** use a single M.2 I/O voltage:
- FPGA banks **13/14/15/16** on the SDR are powered at **3.3V**.
- FPGA banks **34/35** on the SDR are powered at **1.8V**.
- The general-purpose sideband signals routed directly from the M.2 connector to the FPGA on LiteX-M2SDR (`PPS`, `Synchro_GPIO`, `PERST#`, optional `PEWAKE#`, `SUSCLK`, `PEDET`) sit on **3.3V FPGA banks on the SDR side**.
- The M.2 `SMB_CLK` / `SMB_DATA` pins are a special case: on LiteX-M2SDR r02 they reach the FPGA bank-16 pins through optional resistors `R82` / `R83`, which are **not mounted by default**.
- PCIe lanes and the PCIe reference clock are transceiver signals, not single-ended 1.8V/3.3V GPIOs.

Additional notes:
- M.2 pin **44** (`ALERT#` / `SMB_ALERT#`) is currently **not routed to the FPGA** on LiteX-M2SDR r02.
- M.2 pin **52** (`CLKREQ#`) is pulled up to `3V3_PCIe` and is **not** routed to the FPGA.
- M.2 pin **10** (`LED#`) is **not connected** on the FPGA side.
- The dedicated FPGA JTAG/config pins and the Acorn JTAG header are separate **3.3V** JTAG paths.
- When discussing M.2 sideband voltages, distinguish the **FPGA bank voltage on the SDR** from the **connector-side voltage expected by a host/baseboard**. For example, the Acorn baseboard implements the M.2 SMBus pins as a **1.8V SMBus domain** with translation to **3.3V** for the SFP modules.

| Signal | Connector Location | FPGA Pin | Bank | Voltage On SDR Side | Notes |
|--------|--------------------|----------|------|---------------------|-------|
| `GPIO0` | `TP1` | `E22` | 16 | 3.3V | General-purpose test point (`FPGA_GPIO0`). |
| `GPIO1` | `TP2` | `D22` | 16 | 3.3V | General-purpose test point (`FPGA_GPIO1`). |
| `PPS_IN` | M.2 pin 22 (`NC22`) | `K18` | 15 | 3.3V | Routed to the FPGA. |
| `PPS_OUT` | M.2 pin 24 (`NC24`) | `Y18` | 14 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO1` | M.2 pin 28 (`NC28`) | `A19` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO2` | M.2 pin 30 (`NC30`) | `A18` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO3` | M.2 pin 32 (`NC32`) | `A21` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO4` | M.2 pin 34 (`NC34`) | `A20` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO5` | M.2 pin 36 (`NC36`) | `B20` | 16 | 3.3V | Routed to the FPGA. |
| `SMB_CLK` | M.2 pin 40 | `A13` | 16 | 3.3V FPGA bank on SDR | Optional path through `R82`, not mounted by default; connector-level SMBus compatibility depends on the host/baseboard. |
| `SMB_DATA` | M.2 pin 42 | `A14` | 16 | 3.3V FPGA bank on SDR | Optional path through `R83`, not mounted by default; connector-level SMBus compatibility depends on the host/baseboard. |
| `ALERT#` / `SMB_ALERT#` | M.2 pin 44 | - | - | Host-defined sideband | Not routed to the FPGA on LiteX-M2SDR r02. |
| `PERST#` | M.2 pin 50 | `A15` | 16 | 3.3V | Routed to the FPGA. |
| `CLKREQ#` | M.2 pin 52 | - | - | 3.3V | Pulled up to `3V3_PCIe` with `R59`; not routed to the FPGA. |
| `PEWAKE#` | M.2 pin 54 | `B16` | 16 | 3.3V | Optional path through `R88`, not mounted by default. |
| `SUSCLK` | M.2 pin 68 | `B17` | 16 | 3.3V | Routed through `R84` (0R). |
| `PEDET` / `PRESENT` | M.2 pin 69 | `A16` | 16 | 3.3V | Routed through `R85` (0R). |
| `LED#` | M.2 pin 10 | - | - | Host-defined sideband | Not connected on LiteX-M2SDR. |

[> PCIe SoC Design
------------------
<a id="pcie-soc-design"></a>

The PCIe design is the first variant developed for the board and does not require an additional baseboard. Just pop the M2SDR into a PCIe M2 slot, connect your antennas, and you're ready to go! 🚀

The SoC has the following architecture:

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/df5eb55e-16b2-4724-b4c1-28e06c45279c" width="100%">
</div>

- The SoC is built with the LiteX framework, allowing highly efficient HDL coding and integration. 💡
- You'll also find that most of the complexity is managed by LiteX and LitePCIe. The SoC itself only has an MMAP interface, DMA interface, and integrates the specific SDR/RFIC cores and features. ⚙️
- It provides debugging over PCIe or JTAG for MMAP peek & poke or LiteScope. 🛠️
- [LitePCIe](https://github.com/enjoy-digital/litepcie) and its Linux driver (sorry, we only provide Linux support for now 😅) have been battle-tested on several commercial projects. 🏆

The PCIe design has already been validated at the maximum AD9361 specified sample rate: 2T2R @ 61.44MSPS (and also seems to correctly handle the oversampling at 2T2R @ 122.88MSPS with 7.9 Gbps of bandwidth on the PCIe bus; this oversampling feature is already in place and more tests/experiments will be done with it in the future).

[> Ethernet SoC Design (1/2.5Gbps x 1 or 2).
--------------------------------------------
<a id="ethernet-soc-design"></a>

> [!WARNING]
>
> **WiP** 🧪 Still in the lab, RX only for now!

<div align="center">
  <img src="https://github.com/user-attachments/assets/bbcc0c79-4ae8-4e5b-94d8-aa7aff89bae2" width="100%">
</div>

The Ethernet design variant gives flexibility when deploying the SDR. The PCIe connector has 4 SerDes transceivers that are in most cases used for... PCIe :) But these are 4 classical GTP transceivers of the Artix7 FPGA that are connected to the PCIe hardened PHY in the case of a PCIe application but can be used for any other SerDes-based protocol: Ethernet 1000BaseX/2500BaseX, SATA, etc...

In this design, the PCIe core will then be replaced with [LiteEth](https://github.com/enjoy-digital/liteeth), providing the 1000BaseX or 2500BaseX PHY but also the UDP/IP hardware stack + Streaming/Etherbone front-end cores.

The Ethernet SoC design is RX capable only for now. TX support will come soon.

[> Getting Started
------------------
<a id="quick-start"></a>

### For SDR Enthusiasts

If you are an SDR enthusiast looking to get started with the LiteX-M2SDR board, follow these simple steps to get up and running quickly:

1. **Install Prerequisite Packages:**
   - On a fresh Ubuntu system, install the required development and SDR packages to ensure compatibility with the LiteX-M2SDR software:
   ```bash
   sudo apt install build-essential cmake git \
     pkg-config libsdl2-dev libgl1-mesa-dev \
     libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
     gnuradio gnuradio-dev libgnuradio-soapy3.10.9t64 gqrx-sdr \
     libsndfile1-dev libsamplerate0-dev
   ```
   - **Note**: For non-Ubuntu Linux distributions (e.g., Fedora, Arch), install the equivalent packages using your distribution's package manager (e.g., `dnf` for Fedora or `pacman` for Arch).

2. **Connect the Board:**
   - Insert the LiteX-M2SDR board into an available M2 slot on your Linux computer and connect your antennas.

> [!WARNING]
>
> If an error related to DKMS appears during installation, run sudo apt remove --purge xtrx-dkms dkms and then re-execute the installation command.

3. **Clone the Repository:**
   - Clone the LiteX-M2SDR repository using the following command:
   ```
   git clone https://github.com/enjoy-digital/litex_m2sdr
   ```

4. **Build Software:**
    Software build uses `make` and CMake for the C kernel driver and utilities, but since we also like Python 😅, we created a small script on top of it to simplify development and installation:
   ```
   cd litex_m2sdr/software
   ./build.py
   ```
   - This builds the kernel driver, the user-space utilities, `libm2sdr`, and the SoapySDR driver.
   - If you also want the optional SDL/OpenGL GUI tools (`m2sdr_check` / `m2sdr_scan`), first populate the pinned `cimgui` checkout with:
   ```
   cd litex_m2sdr/software
   ./fetch_cimgui.py
   ```
   - Or in a single step:
   ```
   cd litex_m2sdr/software
   ./build.py --fetch-cimgui
   ```
   - By default, `./build.py` builds incrementally; use `./build.py --clean` when you want a full rebuild. Run `sudo ./build.py` if you also want it to install the kernel driver and the SoapySDR module.
   - `m2sdr_check` and `m2sdr_scan` are optional SDL/OpenGL GUI tools. They are built only when SDL2/OpenGL development packages are installed and `litex_m2sdr/software/user/cimgui/` has been populated. If `cimgui` is absent, only those two GUI tools are skipped; the CLI tools, `libm2sdr`, and the SoapySDR module still build normally.

5. **Install the Built Software:**
   - Install the kernel driver:
   ```
   cd litex_m2sdr/software/kernel
   sudo make install
   sudo insmod m2sdr.ko # Optional if you do not want to reboot yet.
   ```
   - Install the public C API headers/library for external applications:
   ```
   cd litex_m2sdr/software/user
   make
   sudo make install_dev PREFIX=/usr/local
   sudo ldconfig
   ```
   - Install the SoapySDR module:
   ```
   cd litex_m2sdr/software/soapysdr/build
   sudo make install
   ```
   - If you already used `sudo ./build.py`, the kernel and SoapySDR install steps above are already done. `libm2sdr` still needs `sudo make install_dev ...` if you want to develop external applications against the public C API.
   - 🚀 Ready for launch!

6. **Run Your SDR Software:**
   - Now, you can launch your preferred SDR software (like GQRX or GNU Radio) and select the LiteX-M2SDR board through SoapySDR. 📡

### Host Requirements & Expectations

- **IOMMU / DMA**: For PCIe streaming, set IOMMU to passthrough mode. If you don't see I/Q data streams in your SDR app, this is the first thing to check.
- **PCIe Gen & Lanes**: Oversampling (122.88 MSPS) requires PCIe Gen2 x2/x4 bandwidth. Gen2 x1 is enough for standard 61.44 MSPS.
- **Ethernet VRT (optional RX path)**: Build with `--with-eth --with-eth-vrt` to enable an Ethernet RX VRT UDP streamer in hardware. A simple host receiver utility is available at `litex_m2sdr/software/user/m2sdr_vrt_rx.py`.
- **Ethernet / SATA (WIP)**: Ethernet SoC is RX-only for now; TX support is in development. SATA support is in development. Both require the LiteX Acorn Baseboard Mini.

> [!TIP]
> If you don't see I/Q data streams in your SDR app, make sure IOMMU is set to passthrough mode. Add the following to your GRUB configuration:
>
> **x86/PC**:
> ```bash
> # Add to GRUB config (/etc/default/grub):
> GRUB_CMDLINE_LINUX="iommu=pt"
> sudo update-grub && sudo reboot
> ```
>
> **ARM (ex NVIDIA Jetson/Orin)**:
> ```bash
> # Add to extlinux.conf (/boot/extlinux/extlinux.conf):
> APPEND ... iommu.passthrough=1 arm-smmu.disable=1
> sudo reboot
> ```

> [!WARNING]
> For intel CPU: if a *kernel panic* occurs with the message **Corrupted page table at address**,
> add `intel_iommu=off` to `GRUB_CMDLINE_LINUX`. (This has been observed on
> an *11th Gen Intel(R) Core(TM) i7-11700B @ 3.20GHz*)

### Tutorials for your platform

> [!WARNING]
>
> **WiP** 🧪 Content below is more our memo as developers than anything useful to read 😅. This will be reworked/integrated differently soon.

For some platforms we created detailed tutorials. For everything else, please follow the earlier *Getting Started* tutorial.

- [Use LiteX-M2SDR on OrangePI 5 Max](doc/hosts/orangepi-5-max.md)
- [Use LiteX-M2SDR on Raspberry Pi 5](doc/hosts/raspberry-pi-5.md)

### For Software Developers

For those who want to dive deeper into development with the LiteX-M2SDR board, follow these additional steps after completing the SDR enthusiast steps:

1. **Test Structure (CI-safe vs hardware scripts):**
   - Gateware simulation/unit tests live in `test/` and are CI-safe (no hardware needed):
   ```
   pytest -v test
   ```
   - Board control/debug scripts live in `scripts/` and require a running board/server:
   ```
   python3 scripts/test_xadc.py
   python3 scripts/test_dashboard.py
   ```
   - CI runs both software build checks and simulation tests with:
   ```
   # Software build checks (kernel/user/SoapySDR) are run in CI.
   python3 -m pytest -v test
   ```

2. **Run Software Tests:**
   - Test the kernel:
   ```
   cd litex_m2sdr/software/kernel
   make clean all
   sudo make install
   sudo insmod m2sdr.ko (To avoid having to reboot the machine)
   ```
   - Test the user-space utilities:
   ```
   cd litex_m2sdr/software/user
   make clean all
   ./m2sdr_util info
   ./m2sdr_rf --sample-rate=30720000 --tx-freq=2400000000 --rx-freq=2400000000
   ./m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1000000 --amplitude 0.5
   ```
   - C API (libm2sdr) quick start and examples:
   ```
   See litex_m2sdr/doc/libm2sdr/README.md
   cd litex_m2sdr/software/user
   make examples
   ../../doc/libm2sdr/example_sync_rx > /tmp/rx.iq
   ../../doc/libm2sdr/example_tone_tx
   ```
   - `libm2sdr` is the common host interface used by the user utilities and the SoapySDR module, so example code there is the reference starting point for new host applications.

3. **SoapySDR Detection/Probe:**
   - Detect the LiteX-M2SDR board:
   ```
   SoapySDRUtil --probe="driver=LiteXM2SDR"
   ```

4. **Run GNU Radio FM Test:**
   - Open and run the GNU Radio FM test:
   ```
   gnuradio-companion litex_m2sdr/software/gnuradio/test_fm_rx.grc
   ```

5. **Enable Debugging in Kernel:**
    - Enable debugging:
    ```
    sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
    ```

### For Software & FPGA Developers

For those who want to explore the full potential of the LiteX-M2SDR board, including FPGA development, follow these additional steps after completing the software developer steps:

1. **Install LiteX:**
   - Follow the installation instructions from the LiteX Wiki: [LiteX Installation](https://github.com/enjoy-digital/litex/wiki/Installation). 📘


2. **Ethernet and PCIe Tests:**
   - For Ethernet tests, if the board is mounted in an Acorn Mini Baseboard:
   ```
   ./litex_m2sdr.py --with-eth --eth-sfp=0 --build --load
   ping 192.168.1.50
   ```
   - For PCIe tests, if the board is mounted directly in an M2 slot:
   ```
   ./litex_m2sdr.py --with-pcie --variant=m2 --build --load
   lspci
   ```

   - For PCIe tests, if the board is mounted directly in a LiteX Acorn Baseboard:
   ```
   ./litex_m2sdr.py --with-pcie --variant=baseboard --build --load
   lspci
   ```

3. **White Rabbit (Baseboard):**
   - White Rabbit is supported on the baseboard variant only:
   ```
   ./litex_m2sdr.py --with-pcie --with-white-rabbit --variant=baseboard --build
   ```
   - `--wr-sfp` is optional; when omitted, the first available `sfp` index is auto-selected.
   - Firmware path lookup order:
     1. `--wr-firmware`
     2. `--wr-nic-dir`
     3. `LITEX_WR_NIC_DIR`
     4. auto-discovery of `../litex_wr_nic` and `../../litex_wr_nic`
   - If a stale local `wr-cores/` checkout is detected, refresh it:
   ```
   mv wr-cores wr-cores.old
   ./litex_m2sdr.py --with-pcie --with-white-rabbit --variant=baseboard --build
   ```

4. **Use JTAGBone/PCIeBone:**
    - Start the LiteX server for JTAG or PCIe:
    ```
    litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg # JTAGBone
    sudo litex_server --pcie --pcie-bar=04:00.0              # PCIeBone (Adapt bar)
    ```

5. **Flash the Board Over PCIe:**
    - Flash the board:
    ```
    cd litex_m2sdr/software
    ./flash.py ../build/litex_m2sdr_platform/litex_m2sdr/gateware/litex_m2sdr_platform.bin
    ```

6. **Reboot or Rescan PCIe Bus:**
    - Rescan the PCIe bus:
    ```
    echo 1 | sudo tee /sys/bus/pci/devices/0000\:0X\:00.0/remove # Replace X with actual value
    echo 1 | sudo tee /sys/bus/pci/rescan
    ```

[> Contact
----------
<a id="contact"></a>

Got a unique idea or need a tweak? Whether it's custom FPGA/software development or hardware adjustments (like adapter boards) for your LiteX M2 SDR, we're here to help! Feel free to drop us a line or visit our website. We'd love to hear from you!

E-mail: florent@enjoy-digital.fr
Website: http://enjoy-digital.fr/

<div align="center">
  <img src="https://github.com/user-attachments/assets/1cf8a5fd-a9bb-4efe-9e50-24eb944bd971" width="100%">
</div>
