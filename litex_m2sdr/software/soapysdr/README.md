# LiteX M2SDR SoapySDR Driver

> [!Note]
> This directory contains a [SoapySDR](https://github.com/pothosware/SoapySDR) hardware driver for the **LiteX-M2SDR** board. It provides a straightforward way to discover, configure, and stream data from M2SDR devices within SoapySDR-compatible applications (GNU Radio, GQRX, etc.).

---

## Overview

The SoapySDR driver builds on the **LiteX-M2SDR** software stack, integrating with:
- **`litepcie`** kernel driver for DMA-based streaming over PCIe.
- **Etherbone/LiteEth UDP** routines for remote control and UDP/VRT streaming over Ethernet.
- M2SDR board-specific controls (sample rate, frequency, gains, etc.).

On Linux the module can be built with both PCIe and Ethernet support and selects the transport at runtime from the device arguments. On macOS and Windows, build Ethernet-only; PCIe depends on the Linux LitePCIe kernel driver and `/dev/m2sdrN`.

For building and installing instructions, **refer to the main LiteX-M2SDR README** which covers the general software setup. Once you have all dependencies and environment ready, build this module with CMake.

Ethernet-only standalone build:

```bash
cmake -S . -B build -DM2SDR_ENABLE_LITEPCIE=OFF -DM2SDR_ENABLE_LITEETH=ON
cmake --build build
```

In-tree build together with `libm2sdr`:

```bash
cmake -S ../user -B ../user/build-soapy -DM2SDR_ENABLE_LITEPCIE=OFF -DM2SDR_ENABLE_LITEETH=ON -DM2SDR_BUILD_SOAPY=ON
cmake --build ../user/build-soapy --target SoapyLiteXM2SDR
```

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

   PCIe device paths are only valid in Linux builds with LitePCIe enabled. Use `eth_ip=` or `dev_id=eth:...` on macOS/Windows.

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
- **Oversampling**: `oversampling=0|1`
- **AD9361 1x FIR profile**: `ad9361_fir_profile=legacy|bypass|match|wide`
  - Useful for `122.88 MSPS` oversampling experiments (these profiles affect the AD9361 `1x` FIR path used above `61.44 MSPS`).
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
  - `rate` is the default and submits TX UDP buffers according to the configured TX sample rate.
  - `none` keeps TX submissions unpaced for link stress tests.

Example:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,rx_agc_mode=fast,bitmode=8,oversampling=1"
```

Example (122.88 MSPS edge-flatness A/B test):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,bitmode=8,oversampling=1,ad9361_fir_profile=wide"
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

- **Single module**: A Linux build can handle PCIe DMA and Ethernet UDP/VRT paths. An Ethernet-only build handles only `eth_ip=` / `dev_id=eth:...`.
- **Multiple Boards**: If multiple M2SDR boards are present, SoapySDR enumerates PCIe devices and the configured Ethernet discovery targets. Specify which one to use via device arguments (e.g. `driver=LiteXM2SDR,path=/dev/m2sdr1` or `driver=LiteXM2SDR,dev_id=eth:192.168.1.50:1234`).
- **Ethernet/Etherbone**: Ethernet discovery is bounded to configured targets, not a subnet scan. Use `eth_ips=...` or `LITEXM2SDR_ETH_IPS=...` for non-default Ethernet addresses.

---

## Contributing & Feedback

We welcome feedback and contributions! If you find any issues or wish to add improvements, please open an issue or pull request on the [litex_m2sdr](https://github.com/enjoy-digital/litex_m2sdr) repository. 🤗

Enjoy your **LiteX-M2SDR** board with SoapySDR!
