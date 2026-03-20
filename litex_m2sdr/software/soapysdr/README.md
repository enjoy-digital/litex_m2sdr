# LiteX M2SDR SoapySDR Driver

> [!Note]
> This directory contains a [SoapySDR](https://github.com/pothosware/SoapySDR) hardware driver for the **LiteX-M2SDR** board. It provides a straightforward way to discover, configure, and stream data from M2SDR devices within SoapySDR-compatible applications (GNU Radio, GQRX, SDR++, CubicSDR, SDRangel, etc.).

---

## Overview

The SoapySDR driver builds on the **LiteX-M2SDR** software stack, integrating with:
- **[libm2sdr](../../doc/libm2sdr/README.md)** — public C API for device control and streaming.
- **`litepcie`** kernel driver for DMA-based streaming over PCIe.
- **Etherbone** routines for optional remote or UDP-based data transfer.
- M2SDR board-specific controls (sample rate, frequency, gains, etc.).

---

## Build & Install

### Prerequisites

- [SoapySDR](https://github.com/pothosware/SoapySDR) development libraries (`libsoapysdr-dev` on Debian/Ubuntu)
- CMake ≥ 3.0
- `libm2sdr` and the kernel driver must be built first (see the [top-level README](../../../README.md#quick-start))

### Using the top-level build script (recommended)

```bash
cd litex_m2sdr/software
./build.py              # builds everything including the SoapySDR module
sudo ./build.py         # builds and installs (kernel + SoapySDR)
```

### Manual build

```bash
cd litex_m2sdr/software/soapysdr
mkdir -p build && cd build
cmake ..
make
sudo make install
```

### Verify installation

```bash
SoapySDRUtil --find="driver=LiteXM2SDR"
```

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
   - **SDR++**: Select `SoapySDR` source and pick the LiteXM2SDR device.
   - **Custom Tools**: Use the standard SoapySDR C++/Python API to open the `LiteXM2SDR` device, set parameters, and read/write streams.

---

## Configuration Knobs (SoapySDR Args)

You can pass device arguments to configure the driver. These are most useful when probing or selecting the device:

- **RX AGC mode**: `rx_agc_mode=slow|fast|hybrid|mgc`
- **Antenna lists**: `rx_antenna_list=A_BALANCED,B_BALANCED` and `tx_antenna_list=A,B`
- **Per-channel antenna**: `rx_antenna0=...`, `rx_antenna1=...`, `tx_antenna0=...`, `tx_antenna1=...`
- **Bit mode**: `bitmode=8|16`
- **Oversampling**: `oversampling=0|1`
- **AD9361 1x FIR profile**: `ad9361_fir_profile=legacy|bypass|match|wide`
  - Useful for `122.88 MSPS` oversampling experiments (these profiles affect the AD9361 `1x` FIR path used above `61.44 MSPS`).
- **Ethernet RX mode** (Etherbone builds): `eth_mode=udp|vrt`
  - `vrt` enables FPGA VRT RX streaming and Soapy RX will parse/strip VRT signal headers.
  - TX streaming remains raw-UDP only; `eth_mode=vrt` is RX-focused.
- **VRT UDP port override** (Etherbone + `eth_mode=vrt`): `vrt_port=4991`

Example:
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,rx_agc_mode=fast,rx_antenna_list=A_BALANCED,tx_antenna_list=A,bitmode=8,oversampling=1"
```

Example (122.88 MSPS edge-flatness A/B test):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,bitmode=8,oversampling=1,ad9361_fir_profile=wide"
```

Example (Etherbone control + Soapy RX over FPGA VRT):
```bash
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50,eth_mode=vrt,vrt_port=4991"
```

---

## Test Utilities

This directory includes several Python utilities to help test and demonstrate the capabilities of the LiteXM2SDR SoapySDR driver:

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
soapysdr/
├── CMakeLists.txt                # Build configuration and dependencies
├── LiteXM2SDRDevice.cpp/hpp      # Main SoapySDR device class (config, RF, gains)
├── LiteXM2SDRRegistration.cpp    # Plugin registration and device enumeration
├── LiteXM2SDRStreaming.cpp       # Streaming (activateStream, readStream, writeStream)
├── test_play.py                  # Python TX test utility
├── test_record.py                # Python RX test utility
└── test_time.py                  # Python hardware time test utility
```

---

## Notes & Tips

- **Multiple Boards**: If multiple M2SDR boards are present, SoapySDR enumerates each. Specify which one to use via device arguments (e.g. `driver=LiteXM2SDR,device=1`).
- **Ethernet/Etherbone**: For network-based streaming, confirm the appropriate IP or addresses if you plan to use Etherbone.
- **TX Attenuation**: TX control uses positive attenuation values (`ATT` gain element in SoapySDR), consistent with the CLI tools (`--tx-att`).

---

## Contributing & Feedback

We welcome feedback and contributions! If you find any issues or wish to add improvements, please open an issue or pull request on the [litex_m2sdr](https://github.com/enjoy-digital/litex_m2sdr) repository. 🤗

Enjoy your **LiteX-M2SDR** board with SoapySDR!
