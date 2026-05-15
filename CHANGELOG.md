# CHANGELOG

## Introduction

The LiteX M2 SDR project is actively under development. We maintain this changelog to allow users and potential clients of the hardware to follow the project's progress and stay up to date with the latest features and improvements. Date-named bitstream archive releases complement the quarterly development summaries below.

[> 2026-05-15 First Date-Named Release
--------------------------------------
**Core PCIe/Ethernet Images with Ethernet PTP Timing Support**
- Added a first date-named release flow that builds and archives the core M2/baseboard PCIe and Ethernet bitstreams with CSR exports and per-build manifests.
- Added final-timing checks to the release flow so setup/hold timing failures are rejected before packaging and the parsed timing summary is recorded in each manifest.
- Included the validated baseboard Ethernet PTP RFIC-reference image (`--with-eth --with-eth-ptp --with-eth-ptp-rfic-clock`) in the release matrix.
- Fixed the `clk10` MMCM phase-shift handshake crossing used by the PTP RFIC-reference clock path so the MMCM DPS port is driven in its `clk200` domain.
- Validated LiteEth TX/RX streaming paths with FPGA stream, AD9361 PHY, and AD9361 internal digital loopback checks; Ethernet TX is now documented as supported on the baseboard.
- Fixed the FM helper utilities for Ethernet use by building `m2sdr_fm_tx`/`m2sdr_fm_rx` in LiteEth user builds and keeping binary IQ/PCM pipes free of stdout status text.
- Documented the remaining release boundaries: White Rabbit, SATA, VRT, 2.5G Ethernet, PCIe x4, and oversampling images are source-build options rather than first-release artifacts.

[> 2026 Q2 (Apr - Jun)
----------------------
**Ethernet PTP Board Time Discipline and SI5351C Clocking Preparation**
- Added Ethernet PTP board-time discipline for baseboard Ethernet builds while keeping `time_gen` as the single exported logical hardware clock for timestamps, PPS, and host-visible time.
- Exposed the PTP feature cleanly across gateware and host software with capability bits, runtime status/tuning controls, learned master identity reporting, and host-side time ownership rules to prevent manual PHC writes from fighting the servo.
- Extended `libm2sdr`, `m2sdr_util`, and SoapySDR with PTP monitoring/control support, time-source reporting, and updated host-side documentation for the new workflow.
- Added an explicit SI5351C FPGA-fed `10MHz` `CLKIN` mode plus a `clk10` MMCM phase-discipline backend, preparing the SI5351C path for later physical clock steering without changing the default non-PTP operating mode.
- Expanded regression coverage around PTP, `clk10` discipline, and SI5351/LiteI2C interactions, including test-isolation fixes so SoC build tests remain stable regardless of execution order.

[> 2026 Q1 (Jan - Mar)
----------------------
**CI Hardening, Host API Consolidation, and Diagnostic Tooling**
- Expanded gateware simulation and CI coverage with broader edge-case/backpressure testing, consolidated gateware test execution, and software build checks for the kernel driver, user utilities, and SoapySDR module.
- Reorganized CI-safe simulations under `test/`, moved board-dependent helpers under `scripts/`, and refreshed the top-level, kernel, and software documentation to match the updated workflows.
- Introduced the public `libm2sdr` C API as the common host layer for user utilities and the SoapySDR module, including shared-library/pkg-config packaging, examples, and build/install integration.
- Hardened `libm2sdr` with stricter validation and error reporting, explicit backend/transport accessors, per-device RF state ownership, safer DMA header helpers, and dedicated unit/sanitizer/cppcheck coverage.
- Unified user-space CLI/device handling across PCIe and Etherbone modes and standardized common long-form options such as `--device`, `--sample-rate`, `--rx-freq`, `--tx-freq`, and `--format`.
- Added the `m2sdr_scan` wideband scanner with an ImGui/SDL/OpenGL UI, preset save/load support, headless CSV/PPM export, and cleaner UI support file organization.
- Added native RF diagnostics and generation utilities with `m2sdr_check` for IQ inspection and `m2sdr_gen` basic OFDM waveform support.
- Added SigMF support across `m2sdr_record`, `m2sdr_play`, and `m2sdr_check`, introduced the `m2sdr_sigmf` inspection/validation utility, and moved the shared SigMF/JSON support into `libm2sdr`.
- Integrated an optional PCIe link reset workaround to improve bring-up robustness on some host platforms.

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
