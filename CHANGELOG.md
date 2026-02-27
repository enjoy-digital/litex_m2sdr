# CHANGELOG

## Introduction

The LiteX M2 SDR project is actively under development and does not yet have formal releases. However, we maintain this changelog to allow users and potential clients of the hardware to follow the project's progress and stay up to date with the latest features and improvements. The updates below are summarized by quarter, highlighting major advancements to provide a clear overview of the project's evolution.

[> 2026 Q1 (Jan - Mar)
----------------------
**CI Hardening, Broader Simulation Coverage, and PCIe Reliability**
- Expanded gateware simulation coverage with additional edge cases, randomized backpressure, mode toggles, and framing invariants.
- Consolidated CI simulation execution into a single gateware test suite for simpler maintenance and reporting.
- Added CI software build checks for Linux kernel driver, user-space utilities, and SoapySDR module compilation.
- Improved test organization by moving board-dependent scripts to `scripts/` and keeping CI-safe simulations in `test/`.
- Added per-test docstrings and aligned test separators/comments with the project code style.
- Integrated optional PCIe link reset workaround logic to improve link bring-up robustness on some host platforms.
- Refreshed README and kernel documentation (CI badge, paths, module naming, and test structure clarifications).

[> 2025 Q4 (Oct - Dec)
----------------------
**PTM Time Sync, SATA Host Integration, and Clocking Refinements**
- Integrated PCIe PTM/PTP support across gateware and kernel/user software, including improved hardware time generation and reporting.
- Added/adapted LiteSATA Linux block-driver support for host operation over PCIe DMA and improved stability in polling/IRQ paths.
- Enhanced kernel compatibility and build robustness across Linux versions (including newer and older kernel API changes).
- Improved Ethernet and SoapySDR paths with UDP RX/TX integration, stream cleanup fixes, and safer DMA buffer teardown.
- Expanded SI5351/RefClk handling with cleaned default configurations, additional 40MHz options, and source/frequency selection support.
- Improved target/debug workflows with PCIe probe visibility, LTSSM test support, and configurable probe depth.

[> 2025 Q3 (Jul - Sep)
----------------------
**White Rabbit Integration, FM Utilities, and LiteI2C Migration**
- Added initial White Rabbit support with PCIe PTM, WR firmware, WR console.
- Introduced FM utilities (m2sdr_fm_tx/rx) with stereo, pre-emphasis, piping modes; replaced Python with C.
- Migrated SI5351 to LiteI2C with sequencer, adaptations, presence checks, and register utilities.
- Enhanced README with diagram, contents table, capabilities overview, and prerequisites.
- Made GPIO optional/default-disabled.
- Fixes: Overtemp power down, AD9361 signals, SI5351 for WR 10MHz, optimized PPS, timing cleanups.

[> 2025 Q2 (Apr - Jun)
----------------------
**GPIO Integration, Streaming Enhancements, and Stability Improvements**
- Added GPIO gateware with packing/unpacking and utilities incl. PPS output.
- Enhanced SoapySDR with LiteEth TX, dynamic oversampling, scaling/deinterleaving fixes, and build configs.
- Improved installation with Makefile rules for binaries/libs/headers and external platform reuse.
- Updated OrangePi/RPi docs with restructured kernel instructions.
- Integrated Capability module for API/hardware info and device reset support.
- Optimized Etherbone with bulk transfers, verification, and remote probing.
- Fixed timing constraints, CRG paths, PCIe rescan, and GPIO mapping.

[> 2025 Q1 (Jan - Mar)
----------------------
**Enhanced Hardware and Software Integration with New Diagnostic Tools**
- Enhanced gateware with optional CSR in measurement module and added simultaneous clock latching capability.
- Improved software ecosystem by introducing `libliteeth` for Etherbone files and adapting SoapySDR driver to use it.
- Optimized power usage by defaulting to 2 PCIe lanes for the M2 variant.
- Extended sample rate support down to 0.5 Msps with FIR Interpolation/Decimation on AD9361, including LTE/5G NR options.
- Added initial dashboard and test utilities (e.g., AGC, DMA, clock panels) for better hardware monitoring and debugging.
- Integrated hardware time generation with `TimeGenerator` module and PPS support for timestamp coherency.
- Improved documentation and installation process, including Raspberry Pi 5 support and updated README for commercial availability.

[> 2024 Q4 (Oct - Dec)
----------------------
**Networking Enhancements and Sample Rate Flexibility**
- Introduced Ethernet-based streaming and control options alongside PCIe, with dynamic IP configuration.
- Enhanced sample rate handling with 122.88MSPS support and oversampling tests for AD9361.
- Added runtime switching between 1T1R and 2T2R modes in SoapySDR driver.
- Fixed key bugs (e.g., RX vs TX mix-up) and improved stability with mutexes for gain/bandwidth settings.
- Expanded hardware support with additional pullups on SI5351 I2C and IOMMU passthrough tips.

[> 2024 Q3 (Jul - Sep)
----------------------
**Ethernet Streaming and Multi-Protocol Support**
- Added initial UDP streaming for RF RX samples over Ethernet.
- Implemented crossbar in target to streamline stream multiplexing/demultiplexing between Comms and RFIC.
- Enabled external 10MHz ClkIn support via uFL for SI5351 and tested SI5351C configuration.
- Supported simultaneous PCIe, Ethernet (1000BaseX/2500BaseX), and SATA with SharedQPLL module.
- Added OrangePi 5 Max documentation for broader platform compatibility.

[> 2024 Q2 (Apr - Jun)
----------------------
**Performance Boosts and Mode Flexibility**
- Introduced multiboot bitstream flashing and switched to 4 PCIe lanes by default for improved performance.
- Added 8-bit mode support in gateware and software, with automatic TX/RX delay calibration.
- Integrated SPI Flash support for updates over PCIe and AD9361 temperature sensor in SoapySDR.
- Enhanced clocking with SI5351_EN control and 2500BaseX Ethernet support.
- Improved RFIC oversampling options and timestamp handling with 64-bit counter optimization.

[> 2024 Q1 (Jan - Mar)
----------------------
**Project Foundation and Connectivity Setup**
- Initial project setup with SI5351 clocking (148.5MHz/148.35MHz) and 1000BaseX Etherbone support.
- Added AD9361 SPI integration and basic RFIC connectivity with debug tools (LiteScope).
- Established PCIe X1 support and foundational software utilities (e.g., `litex_m2sdr_rf`, `liblitexm2sdr`).
- Laid groundwork with platform and target definitions, including JTAG and SFP support.
