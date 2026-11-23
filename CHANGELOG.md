# CHANGELOG

## Introduction

The LiteX M2 SDR project is actively under development. We maintain this changelog to allow users and potential clients of the hardware to follow the project's progress and stay up to date with the latest features and improvements. Date-named bitstream archive releases complement the development summaries below.

[> 2026-05-15 First Date-Named Release
--------------------------------------
**Feature Status at Release**
- Release artifacts: first date-named, timing-checked archives are provided for the supported core M.2 PCIe and Acorn Baseboard Ethernet images, with CSR exports and per-build manifests.
- M.2 PCIe: PCIe Gen2 x1 and x2 images are provided for the M.2 form factor; PCIe-only images keep the 125MHz system clock.
- Baseboard Ethernet: 1000BaseX Etherbone control and LiteEth UDP RX/TX sample streaming are supported on the Acorn Baseboard Mini.
- Baseboard PCIe + Ethernet: a combined PCIe Gen2 x1 + 1000BaseX Ethernet image is provided and uses a 100MHz system clock for timing margin.
- Ethernet PTP: the baseboard Ethernet PTP image disciplines the board `time_gen` clock for timestamps, PPS, and host-visible board time.
- PTP RFIC reference: the dedicated baseboard PTP RFIC-reference image exposes the FPGA `clk10` MMCM discipline loop and SI5351C FPGA-fed `10MHz` input mode.
- Host software: `libm2sdr`, `m2sdr_util`, SoapySDR, `m2sdr_play`, and the FM helper utilities include the matching Ethernet/PTP/RFIC-reference controls and examples.
- Source-build options: White Rabbit, SATA, VRT, 2.5G Ethernet, PCIe x4, and oversampling images remain available from source but are outside this first release artifact set.

[> 2026 Q4 to date (Oct - Dec)
------------------------------
**PCIe Resources**
- Reduced the FPGA resources of the PCIe core: the completions of all the request tags share one block RAM and the DMA descriptor tables are in block RAM (LitePCIe `cmp_bufs_shared` and `dma_table_buffered`). In a standalone synthesis of the PCIe endpoint, this saves about 1300 LUTs and 9 block RAM tiles at x1 and x2, and about 1400 LUTs and 17 block RAM tiles at x4. It needs a LiteX and a LitePCIe that provide these options.

**Host Software Layout**
- Changed the layout of `litex_m2sdr/software/user/`: its top level now holds the utilities only. Their support code (`m2sdr_cli`, `m2sdr_tool`, `m2sdr_host_queue`, `m2sdr_sata_*`) is in `libm2sdr/`, private to the build (in `libm2sdr.a`, not in `libm2sdr.so` nor in the installed headers); `kissfft` and the `cimgui` submodule are in `third_party/`; the Python programs are in `python/`; `imgui.ini` is no longer tracked. In an existing clone, run `git submodule sync` then `git submodule update --init --recursive litex_m2sdr/software/user/third_party/cimgui`, and delete the old `litex_m2sdr/software/user/cimgui` directory.

**Low Sample Rates**
- Fixed RX aliasing below `2.5 MSPS`: the AD9361 RX and TX FIRs now use their dedicated tables, x4 below `1.25 MSPS` and x2 from `1.25` to `2.5 MSPS`, in `libm2sdr` as in the SoapySDR driver. The RX level is about `6 dB` higher below `1.25 MSPS` than above, with the same signal to noise ratio; between `1.25` and `2.5 MSPS` it differs from earlier versions of `libm2sdr`.

**USB3 Host Link**
- Added hardware timed TX over USB3: the USB3 TX stream carries the TX header, and the timed-TX gate holds each timed buffer as on PCIe (capability bit `usb3_tx_header`; an older USB3 bitstream keeps the software timeline).
- Added the rates above `61.44 MSPS` over USB3 in 1T1R on the standard bitstream, up to `122.88 MSPS` in SC8; the SoapySDR driver accepts such a rate before the stream is set up.
- Added stream loss counters at the RFIC (RX overflow, TX underflow, capability bit `stream_counters`): the stream statistics of `libm2sdr` and the SoapySDR stream status report the samples lost in the FPGA over USB3 and Ethernet.
- Fixed the USB3 TX rate with small writes (one buffer per call: `279` to `308 MB/s` in full duplex) and the `m2sdr_util` AD9361 loopback and PRBS tests over USB3.

**SoapySDR Driver**
- Fixed the board with SigDigger: its device selection (`device=LiteXM2SDR,serial=...`) was taken for a network host name, and its request for the external clock reference, made on every device, left the RFIC without a reference when none was connected. `device` is an identifier only when it looks like one, `serial` selects a board among those found, and the `external` and `fpga` clock sources are refused when no clock reaches the reference input (`m2sdr_rf --sync external` stops with the reason instead of timing out).
- Fixed two devices open on one board in a process (SigDigger lists the board while it opens it): they are opened one after the other, the RFIC is initialised by the first and powered down by the last.
- Fixed several boards open in one process with the SoapySDR driver: the AD9361 commands of every board went to the board opened first (the SPI identifier of a board was read at the wrong place, an empty C structure having a size in C++ only).

**Signal Analysis**
- Added `m2sdr_analyzer`: spectrum with traces, markers and a waterfall, channel power, ACPR, occupied bandwidth, tone figures (SNR, SINAD, SFDR, THD) and time-domain statistics, on the board RX or on a SigMF or raw capture, in a Dear ImGui window or as a text or JSON report without a display. The measurements are in libm2sdr (`m2sdr_analysis.h`).
- Added the phase noise of a carrier to `m2sdr_analyzer`: L(f) and amplitude noise against the offset, integrated phase error, jitter, residual FM and spurs, with a floor of the board stored per board and drawn under the trace.
- Added the demodulation of single-carrier PSK and QAM signals to `m2sdr_analyzer` (BPSK to 256QAM, symbol rate, roll-off and modulation found from the signal): constellation, eye diagrams, EVM, MER, magnitude and phase errors, origin offset, I/Q gain imbalance and quadrature error.
- Added the analysis of an LTE downlink carrier to `m2sdr_analyzer` (FDD, normal cyclic prefix): cell identity, MIB decoded from the PBCH, carrier offset, RSRP, EVM of the reference signals and of the PBCH, resource grid, and the modulation, EVM and constellation of each block of data, for a cell of one or two antenna ports. With two ports, a block in transmit diversity or with one precoded layer is read; a block of two layers is reported as in use and not read.
- Added the reception of a DVB-T multiplex to `m2sdr_analyzer` (2k and 8k, EN 300 744): mode and guard interval, carrier offset (1/6 MHz network offsets included), TPS (constellation, code rates, hierarchy, cell), MER over all carriers and per carrier against the C/N needed, channel response and echo profile, and a live MER trace to place an antenna.
- Added `m2sdr_gen --signal qam`: random PSK or QAM symbols under a root raised cosine, as a test source. `m2sdr_gen` now sends at the sample rate over Ethernet, where it used to send as fast as the host could and the board dropped most of the datagrams, and reports the TX underflows counted by the FPGA.

**FM Utilities**
- Reworked `m2sdr_fm_tx` and `m2sdr_fm_rx` on a modulator and a demodulator in `libm2sdr` (`m2sdr_fm.h`), with offline tests (`make test-fm`): about 14 times less CPU time at `1 MSPS`.
- Added stereo decoding to `m2sdr_fm_rx` (pilot PLL): `--mode stereo` wrote the mono signal on both channels. Added a channel filter and a 15 kHz audio low-pass, which removes the pilot from the audio.
- Fixed `m2sdr_fm_tx`: deviation steps of `244 Hz` that silenced quiet audio, a pre-emphasis that kept oscillating at half the sample rate after a step in the audio, text written on stdout in front of the I/Q samples, and audio above 15 kHz reaching the pilot. The audio is now limited to the deviation after the pre-emphasis.
- Fixed `m2sdr_fm_radio.py`: its `m2sdr_rf` options were not accepted, so the frequency and the gain were not applied.
- Added RDS (EN 50067 / IEC 62106) to `m2sdr_fm_tx` and `m2sdr_fm_rx`: the transmitter sends PI, PTY, the PS name, RadioText and the clock time (`--rds-pi`, `--rds-ps`, `--rds-rt`, `--rds-pty`, `--rds-ct`, groups 0A, 2A and 4A) on a 57 kHz subcarrier locked on the pilot, and `m2sdr_fm_rx --rds` decodes and prints them. Checked offline, not yet on a receiver nor on a broadcast station.

**Calibration**
- Added `m2sdr_cal`: scans bands for LTE and GSM carriers, measures the error of the reference clock on LTE carriers (PSS and cyclic prefix) and GSM carriers (FCCH), trims the reference and stores the result in a file or in the SPI flash. `m2sdr_rf --refclk-ppm stored` uses the record, and `m2sdr_cal measure --refclk-ppm stored` checks it. The estimators and the record are in libm2sdr (`m2sdr_cal.h`).

**Ethernet Host Link**
- Added hardware timed TX over Ethernet (capability bit `eth_tx_header`) and RX timestamps on the raw UDP stream: the gateware finds the TX header at the start of a datagram, and `libm2sdr` aligns its RX buffers on the frames of the FPGA, through lost datagrams.
- Qualified 1000BASE-X at `116 MB/s` in full duplex (2T2R `14.5 MSPS` in SC16, `29 MSPS` in SC8).
- Added flow control of the Ethernet TX stream (capability bit `eth_tx_status`): the gateware gives the level of its TX FIFO and the datagrams it dropped, and `libm2sdr` holds `m2sdr_sync_tx()` and `m2sdr_submit_buffer()` back on that level, so the host sends at the rate of the sample clock of the board and no datagram is dropped. Without it, `m2sdr_play` and `m2sdr_gen` sent as fast as the host could and the signal was sent in pieces.
- With an older gateware, `m2sdr_play` sends at the sample rate on the clock of the host, from the SigMF metadata or `--sample-rate`.
- Fixed the start of an Ethernet RX stream: about `110 KB` of old samples, left in the RFIC RX FIFO, opened it with compressed timestamps. The gateware drains them while the UDP streamer is disabled, and `libm2sdr` waits `10 ms` for it before enabling the streamer.
- Fixed a loss of the Ethernet control link: a full TX FIFO stalled the receive path of the whole LiteEth core, so the board stopped answering while the timed TX gate held a frame. The datagrams that do not fit are now dropped.
- Fixed the sample streams of several Ethernet boards on one host, and of two programs on one board: they all received on UDP port 2345, and one of them took the datagrams of the others. A stream that finds the port taken receives on a free one and tells it to the board.

[> 2026 Q2 to date (Apr - Jun)
------------------------------
**Runtime Host Stack, SATA Workflows, Wide RFIC Modes, and Stability Hardening**
- Added Ethernet PTP board-time discipline for baseboard Ethernet builds while keeping `time_gen` as the single exported logical hardware clock for timestamps, PPS, and host-visible time.
- Exposed PTP feature discovery and control across gateware and host software with capability bits, runtime status/tuning controls, learned master identity reporting, and host-side time ownership rules.
- Added an explicit SI5351C FPGA-fed `10MHz` `CLKIN` mode plus a `clk10` MMCM phase-discipline backend for PTP-referenced RFIC clock operation.
- Consolidated host access around `libm2sdr`, including runtime PCIe/Ethernet backend selection, shared device discovery/identifier parsing, CMake/pkg-config metadata, and SoapySDR integration through the common library.
- Added date-named release archives with timing-gated packaging, manifests, and a GitHub release helper for the first supported PCIe/Ethernet image set.
- Reworked SATA into userspace workflows: PCIe DMA host-copy ioctls, Ethernet utility streaming, named capture/import/export commands, SigMF metadata handling, workflow documentation, and transport validation tooling.
- Removed the obsolete in-kernel LiteSATA block path from `m2sdr.ko`; SATA host access now goes through the supported userspace `m2sdr_sata` paths.
- Added RFIC wide-bandwidth operation: rates above `61.44 MSPS` select the AD9361 wide data-port mode with widened baseband filters, PRBS-verified interface bring-up, and support for the 1T1R/2T2R channel layouts.
- Added rounded SC8 conversion and an experimental BFP8 RFIC transport format, with host/gateware support, format-loss evaluation tooling, documentation, plots, and regression tests.
- Improved SoapySDR streaming with timed TX/RX support for srsRAN-style scheduling, RX hardware timestamp headers, LiteEth UDP/VRT paths, constructor/setup rollback fixes, and capability-aware PCIe high-rate bit packing.
- Hardened gateware CDC and RFIC diagnostic paths with reusable value/strobe crossings, 64-bit CSR handshakes, PTP discipline crossings, 1T1R-aware PRBS TX/RX checks, SI5351 flush bounds, and clearer PCIe/header/backpressure behavior.
- Hardened kernel and host transport code with DMA memory barriers, atomic DMA locking, stricter mmap update validation, DMA ring resync, stale TX flush fixes, LiteEth datagram truncation checks, and non-spinning socket backpressure handling.
- Expanded regression coverage around PTP, `clk10` discipline, SI5351/LiteI2C interactions, release packaging, SATA host I/O, CDC helpers, RFIC sample-format loss, and Ethernet-enabled build defaults.

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
- Added early SATA host integration over PCIe DMA and improved stability in polling/IRQ paths.
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
