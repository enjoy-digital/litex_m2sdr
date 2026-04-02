# LiteX M2SDR SoapySDR Driver

> [!Note]
> This directory contains a [SoapySDR](https://github.com/pothosware/SoapySDR) hardware driver for the **LiteX-M2SDR** board. It provides a straightforward way to discover, configure, and stream data from M2SDR devices within SoapySDR-compatible applications (GNU Radio, GQRX, etc.).

---

## Overview

The SoapySDR driver builds on the **LiteX-M2SDR** software stack, integrating with:
- **`litepcie`** kernel driver for DMA-based streaming over PCIe.
- **Etherbone** routines for optional remote or UDP-based data transfer.
- M2SDR board-specific controls (sample rate, frequency, gains, etc.).

For building and installing instructions, **refer to the main LiteX-M2SDR README** which covers the general software setup. Once you have all dependencies and environment ready, simply build this module with your usual CMake workflow.

---

## Usage

Once installed, the driver will be automatically loaded by SoapySDR. You can then:

1. **Probe SoapySDR**
   ```bash
   SoapySDRUtil --probe="driver=LiteXM2SDR"
   ```
   This should list the capabilities and configuration parameters of your M2SDR board.

2. **Run SoapySDR Applications**
   - **GNU Radio**: Load `Soapy` blocks in GRC or run `gnuradio-companion`. Select `SoapySDR` source/sink blocks with `driver=LiteXM2SDR`.
   - **GQRX**: Configure Soapy as the input device, specifying the `LiteXM2SDR` driver if multiple Soapy devices are present.
   - **Custom Tools**: Use the standard SoapySDR C++/Python API to open the `LiteXM2SDR` device, set parameters, and read/write streams.

---

## Configuration Knobs (SoapySDR Args)

You can pass device arguments to configure the driver. These are most useful when probing or selecting the device:

- **RX AGC mode**: `rx_agc_mode=slow|fast|hybrid|mgc`
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
- **Ethernet RX mode** (Etherbone builds): `eth_mode=udp|vrt`
  - `vrt` enables FPGA VRT RX streaming and Soapy RX will parse/strip VRT signal headers.
  - TX streaming remains raw-UDP only; `eth_mode=vrt` is RX-focused.
- **VRT UDP port override** (Etherbone + `eth_mode=vrt`): `vrt_port=4991`

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

When the bitstream was built with `--with-eth --with-eth-ptp`, the Soapy driver also exposes:

- Time sources: `internal`, `ptp`
- Device sensors: `ptp_locked`, `ptp_time_locked`, `ptp_holdover`, `ptp_state`, `ptp_master_ip`, `ptp_master_clock_id`, `ptp_master_port`, `ptp_master_port_id`, `ptp_last_error_ns`

Example:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR"
```
The probe output will list the available time sources and the additional PTP sensors when Ethernet PTP support is present in the FPGA image.

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
  Implements SoapySDR streaming methods (activateStream, readStream, writeStream…) using the PCIe DMA path, with the optional network receive path handled in the current driver sources.

- **test_play.py, test_record.py, test_time.py**
  Python scripts to test and demonstrate transmission, recording, and hardware time functionality using the LiteXM2SDR SoapySDR driver.

---

## Notes & Tips

- **Multiple Boards**: If multiple M2SDR boards are present, SoapySDR enumerates each. Specify which one to use via device arguments (e.g. `driver=LiteXM2SDR,device=1`).
- **Ethernet/Etherbone**: For network-based streaming, confirm the appropriate IP or addresses if you plan to use Etherbone.

---

## Contributing & Feedback

We welcome feedback and contributions! If you find any issues or wish to add improvements, please open an issue or pull request on the [litex_m2sdr](https://github.com/enjoy-digital/litex_m2sdr) repository. 🤗

Enjoy your **LiteX-M2SDR** board with SoapySDR!
