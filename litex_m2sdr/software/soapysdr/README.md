# LiteX M2SDR SoapySDR Driver

> [!Note]
> This directory contains a [SoapySDR](https://github.com/pothosware/SoapySDR) hardware driver for the **LiteX-M2SDR** board. It provides a straightforward way to discover, configure, and stream data from M2SDR devices within SoapySDR-compatible applications (GNU Radio, GQRX, etc.).

---

## Overview

The SoapySDR driver builds on the **LiteX-M2SDR** software stack, integrating with:
- **`litepcie`** kernel driver for DMA-based streaming over PCIe.
- **Etherbone/LiteEth UDP** routines for remote control and UDP/VRT streaming over Ethernet.
- M2SDR board-specific controls (sample rate, frequency, gains, etc.).

The module is built once and selects PCIe or Ethernet at runtime from the device arguments.

For building and installing instructions, **refer to the main LiteX-M2SDR README** which covers the general software setup. Once you have all dependencies and environment ready, simply build this module with your usual CMake workflow.

---

## Usage

Once installed, the driver will be automatically loaded by SoapySDR. You can then:

1. **Find/Probe SoapySDR**
   ```bash
   SoapySDRUtil --find="driver=LiteXM2SDR"
   ```
   This enumerates PCIe devices and probes the default Ethernet target
   (`192.168.1.50:1234`) so a mixed PCIe/Ethernet setup can appear in one
   Soapy device list.

   To scan specific Ethernet targets during discovery, pass a semicolon-separated
   list or set the matching environment variable:
   ```bash
   SoapySDRUtil --find="driver=LiteXM2SDR,eth_ips=192.168.1.50;192.168.1.51:1234"
   LITEXM2SDR_ETH_IPS=192.168.1.50,192.168.1.51 SoapySDRUtil --find="driver=LiteXM2SDR"
   ```

   ```bash
   SoapySDRUtil --probe="driver=LiteXM2SDR"
   ```
   This probes the first discovered M2SDR board and should list the capabilities and configuration parameters.

   For an Ethernet board, pass the board IP explicitly:
   ```bash
   SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
   ```

   For a specific PCIe node, pass the device path explicitly:
   ```bash
   SoapySDRUtil --probe="driver=LiteXM2SDR,path=/dev/m2sdr1"
   ```

2. **Run SoapySDR Applications**
   - **GNU Radio**: Load `Soapy` blocks in GRC or run `gnuradio-companion`. Select `SoapySDR` source/sink blocks with `driver=LiteXM2SDR`.
   - **GQRX**: Configure Soapy as the input device, specifying the `LiteXM2SDR` driver if multiple Soapy devices are present.
   - **Custom Tools**: Use the standard SoapySDR C++/Python API to open the `LiteXM2SDR` device, set parameters, and read/write streams.

---

## Configuration Knobs (SoapySDR Args)

You can pass device arguments to configure the driver. These are most useful when probing or selecting the device:

- **RX AGC mode**: `rx_agc_mode=slow|fast|hybrid|mgc`
- **Device selection**: `path=/dev/m2sdr0` for PCIe, `eth_ip=192.168.1.50` for Ethernet, or `dev_id=pcie:/dev/m2sdr0` / `dev_id=eth:192.168.1.50:1234` for an explicit libm2sdr identifier.
- **Ethernet discovery**: default discovery probes `192.168.1.50:1234` after PCIe. Use `eth_ips=ip[;ip[:port]...]` or `LITEXM2SDR_ETH_IPS` to override the discovery list, and `eth_discovery=0` to disable Ethernet discovery.
- **Antenna selection**: RX uses `A_BALANCED`, TX uses `A`
  The driver intentionally exposes only the board-connected RF ports, not the full AD9361 antenna enum.
- **Per-channel antenna**: `rx_antenna0=A_BALANCED`, `rx_antenna1=A_BALANCED`, `tx_antenna0=A`, `tx_antenna1=A`
- **Bit mode**: `bitmode=8|16`
- **AD9361 1x FIR profile**: `ad9361_fir_profile=legacy|bypass|match|wide`
  - Rates above `61.44 MSPS` automatically use the wide-bandwidth data-port mode; these profiles affect the AD9361 `1x` FIR path used by that mode.
- **PCIe high-rate packing**: `bitmode=8`
  - Useful above `61.44 MSPS` when DMA bandwidth matters. The driver no longer silently overrides PCIe to 8-bit mode, so request `bitmode=8` or a `CS8` stream explicitly.
- **Clock source**: `clock_source=internal|external|fpga`
  - `internal` keeps the local XO path.
  - `external` selects the SI5351C `10MHz` CLKIN from the uFL connector.
  - `fpga` selects the SI5351C `10MHz` CLKIN from the FPGA `clk10` path.
    On Ethernet PTP builds that also enable `--with-eth-ptp-rfic-clock`, use
    `m2sdr_util -i 192.168.1.50 ptp-clock10-config enable on` and wait for
    the clk10 loop to lock before opening SoapySDR with `clock_source=fpga`.
- **Ethernet RX mode** (Ethernet devices): `eth_mode=udp|vrt`
  - `vrt` enables FPGA VRT RX streaming and Soapy RX will parse/strip VRT signal headers.
  - TX streaming remains raw-UDP only; `eth_mode=vrt` is RX-focused.
- **VRT UDP port override** (Etherbone + `eth_mode=vrt`): `vrt_port=4991`
- **LiteEth TX pacing**: `tx_pacing=rate|none`
  - `rate` is the default and submits TX UDP buffers according to the configured TX sample rate. Pacing follows the board clock (periodically re-anchored through hardware time reads) so long runs do not drift with the host oscillator.
  - `none` keeps TX submissions unpaced for link stress tests.
- **Timed TX** (software placement on the board-time axis): `timed_tx=software|off`
  - `software` (default) honors `SOAPY_SDR_HAS_TIME` on `writeStream()`: gaps up to the requested timestamp are zero-filled and writes later than `tx_late_margin_ns` return `SOAPY_SDR_TIME_ERROR`.
  - `tx_lead_buffers=N` (default `0`): schedules the timeline `N` MTUs ahead of board time at activation. The hardware emits samples as soon as DMA delivers them, so any lead shifts actual emission that far **before** the requested timestamps — keep `0` for applications that need absolute TX timing (srsRAN) and already write ahead of time.
  - `tx_latency_ns=K` (default `0`, negative allowed): constant trim for the DMA/RFIC pipeline latency, calibratable with an RF/FPGA loopback.
  - `tx_late_margin_ns=M`: lateness tolerance before a timed write is rejected (default: one MTU duration).
- **RX DMA headers** (PCIe): `rx_dma_header=1|0`
  - `1` (default) uses the FPGA per-buffer hardware timestamps for RX time reporting when the gateware supports them (probed at open; falls back to software accounting otherwise).

Example:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,rx_agc_mode=fast,bitmode=8"
```

Example (122.88 MSPS edge-flatness A/B test):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,bitmode=8,ad9361_fir_profile=wide"
```

Example (Etherbone control + Soapy RX over FPGA VRT):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50,eth_mode=vrt,vrt_port=4991"
```

Example (Ethernet PTP-disciplined FPGA 10MHz RFIC reference):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50,clock_source=fpga"
```

When the bitstream was built with `--with-eth --with-eth-ptp`, the Soapy driver also exposes:

- Time sources: `internal`, `ptp`
- Device sensors: `ptp_locked`, `ptp_time_locked`, `ptp_holdover`, `ptp_state`, `ptp_master_ip`, `ptp_master_clock_id`, `ptp_master_port`, `ptp_master_port_id`, `ptp_last_error_ns`

Example:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR"
```
The probe output will list the available time sources and the additional PTP sensors when Ethernet PTP support is present in the FPGA image.

For PCIe/PTM host-time synchronization, build the gateware with
`--with-pcie --pcie-lanes=1 --with-pcie-ptm` and run
`scripts/m2sdr_pcie_time_sync.py` on the host. The helper drives the M2SDR PHC
from `CLOCK_REALTIME` through `phc2sys`; Soapy then observes the same board
hardware time as the rest of the host stack.

---

## srsRAN 4G

The driver works with srsRAN 4G (srsENB/srsUE) through srsRAN's SoapySDR RF
adapter. srsRAN streams CF32, schedules every TX subframe ~4 ms ahead with
`SOAPY_SDR_HAS_TIME` on the first write of each send, and derives its TX time
from the RX timestamps — both paths are served by the hardware RX timestamps
(PCIe DMA headers / Ethernet VRT) and the software timed-TX placement.

Example `enb.conf` / `ue.conf` RF section (PCIe):

```ini
[rf]
device_name = soapy
device_args = driver=LiteXM2SDR
tx_gain = 60
rx_gain = 40
# time_adv_nsamples = auto    # increase if "L" (late) indicators appear
```

For an Ethernet board use `device_args = driver=LiteXM2SDR,eth_ip=192.168.1.50`.
PCIe is recommended for LTE: raw-UDP RX carries no hardware timestamps
(`eth_mode=vrt` does, but VRT is RX-only).

Notes:
- All LTE sample rates (1.92 / 3.84 / 5.76 / 11.52 / 15.36 / 23.04 MSPS) are
  within the supported range; the AD9361 FIR decimation is selected
  automatically.
- Do **not** pass LimeSDR-style `rxant=`/`txant=` antenna arguments: the board
  exposes `A_BALANCED` (RX) and `A` (TX) only, and unknown names make the
  driver throw at open.
- RX gain defaults to manual mode, so srsRAN's gain settings apply directly.
- Console indicators: `L`/`T` = TX timed write arrived late (raise
  `time_adv_nsamples` or check CPU load), `U` = TX underflow, `O` = RX
  overflow. They should be absent or rare in steady state.
- Keep `tx_lead_buffers=0` (the default): srsRAN's own scheduling advance
  provides the queue slack, and a non-zero lead shifts the actual emission
  ahead of the requested timestamps, breaking the DL/UL subframe alignment.

---

## Test Utilities

This repository includes several Python utilities to help test and demonstrate the capabilities of the LiteX-M2SDR SoapySDR driver:

- **test_time.py**
  Sets and reads the LiteXM2SDR hardware time (in nanoseconds). It can set the hardware time to the current time (or a specified value) and then repeatedly read and display the hardware time along with its local date/time representation.

  *Usage Example:*
  ```bash
  ./test_time.py --set now --interval 0.2 --duration 5
  ```

- **test_play.py**
  Transmits I/Q samples using the SoapySDR driver. It supports two modes:
  - **Tone Generation**: Generates and transmits a continuous tone.
  - **File Playback**: Plays back a file of raw CF32 samples, with support for looping.

  *Usage Example:*
  ```bash
  ./test_play.py --samplerate 4e6 --bandwidth 56e6 --freq 2.4e9 --att 20 --channel 0 --tone-freq 1e6 --ampl 0.8 --secs 5
  ```

- **test_record.py**
  Records I/Q samples and writes them as raw CF32 data to a file. Optionally, it can check and print timestamp information, including differences between consecutive timestamps.

  *Usage Example:*
  ```bash
  ./test_record.py --samplerate 4e6 --bandwidth 56e6 --freq 2.4e9 --gain 20 --channel 0 --secs 5 --check-ts output.bin
  ```
---

## File Structure

```
./
├── CMakeLists.txt
├── LiteXM2SDRDevice.cpp
├── LiteXM2SDRDevice.hpp
├── LiteXM2SDRRegistration.cpp
├── LiteXM2SDRStreaming.cpp
├── test_play.py
├── test_record.py
└── test_time.py
```

- **CMakeLists.txt**
  Defines the build steps and dependencies for the SoapySDR module.

- **LiteXM2SDRDevice.cpp/hpp**
  Main SoapySDR device class, providing sample rate/frequency/gain setups, device controls, etc.

- **LiteXM2SDRRegistration.cpp**
  Handles SoapySDR plugin registration and device enumeration.

- **LiteXM2SDRStreaming.cpp**
  Implements SoapySDR streaming methods (activateStream, readStream, writeStream…) with runtime dispatch between PCIe DMA and LiteEth UDP/VRT paths.

- **test_play.py, test_record.py, test_time.py**
  Python scripts to test and demonstrate transmission, recording, and hardware time functionality using the LiteXM2SDR SoapySDR driver.

---

## Notes & Tips

- **Single module**: The installed SoapySDR module handles PCIe DMA and Ethernet UDP/VRT paths. Select the transport with `path=`, `eth_ip=`, or `dev_id=`.
- **Multiple Boards**: If multiple M2SDR boards are present, SoapySDR enumerates PCIe devices and the configured Ethernet discovery targets. Specify which one to use via device arguments (e.g. `driver=LiteXM2SDR,path=/dev/m2sdr1` or `driver=LiteXM2SDR,dev_id=eth:192.168.1.50:1234`).
- **Ethernet/Etherbone**: Ethernet discovery is bounded to configured targets, not a subnet scan. Use `eth_ips=...` or `LITEXM2SDR_ETH_IPS=...` for non-default Ethernet addresses.

---

## Contributing & Feedback

We welcome feedback and contributions! If you find any issues or wish to add improvements, please open an issue or pull request on the [litex_m2sdr](https://github.com/enjoy-digital/litex_m2sdr) repository. 🤗

Enjoy your **LiteX-M2SDR** board with SoapySDR!
