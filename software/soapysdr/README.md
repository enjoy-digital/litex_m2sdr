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

## File Structure

```
./
├── CMakeLists.txt
├── etherbone.c
├── etherbone.h
├── LiteXM2SDRDevice.cpp
├── LiteXM2SDRDevice.hpp
├── LiteXM2SDRRegistration.cpp
├── LiteXM2SDRStreaming.cpp
├── LiteXM2SDRUDPRx.cpp
└── LiteXM2SDRUDPRx.hpp
```

- **CMakeLists.txt**
  Defines the build steps and dependencies for the SoapySDR module.

- **etherbone.c/h**
  Optional Etherbone (UDP-based) code for remote streaming/control when PCIe is unavailable or not desired.

- **LiteXM2SDRDevice.cpp/hpp**
  Main SoapySDR device class, providing sample rate/frequency/gain setups, device controls, etc.

- **LiteXM2SDRRegistration.cpp**
  Handles SoapySDR plugin registration and device enumeration.

- **LiteXM2SDRStreaming.cpp**
  Implements SoapySDR streaming methods (activateStream, readStream, writeStream…) using the PCIe DMA path.

- **LiteXM2SDRUDPRx.cpp/hpp**
  Implements optional UDP receive routines (via Etherbone or custom protocol).

---

## Notes & Tips

- **Multiple Boards**: If multiple M2SDR boards are present, SoapySDR enumerates each. Specify which one to use via device arguments (e.g. `driver=LiteXM2SDR,device=1`).
- **Ethernet/Etherbone**: For network-based streaming, confirm the appropriate IP or addresses if you plan to use Etherbone.

---

## Contributing & Feedback

We welcome feedback and contributions! If you find any issues or wish to add improvements, please open an issue or pull request on the [litex_m2sdr](https://github.com/enjoy-digital/litex_m2sdr) repository. 🤗

Enjoy your **LiteX-M2SDR** board with SoapySDR!
