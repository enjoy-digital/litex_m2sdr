# CLAUDE.md - LiteX-M2SDR Development Guide

## Project Overview

LiteX-M2SDR is a fully open-source, LiteX-based M.2 2280 Software-Defined Radio (SDR) FPGA board. It combines an FPGA gateware design (Python/Migen), a Linux kernel module (C), user-space utilities (C), and a SoapySDR driver (C++) into one integrated stack.

**Key hardware**: Xilinx Artix-7 XC7A200T FPGA, ADI AD9361 RFIC, SI5351 clock generator, PCIe Gen2 interface.
**License**: BSD-2-Clause
**Copyright**: 2024-2025 Enjoy-Digital
**Status**: Alpha (active development, quarterly releases)

## Repository Structure

```
litex_m2sdr/
├── litex_m2sdr.py              # Main SoC builder (gateware generation + CLI)
├── litex_m2sdr_platform.py     # FPGA platform definition (pin assignments, I/O constraints)
├── release.py                  # Multi-configuration release builder
├── setup.py                    # Python package setup (requires litex)
│
├── litex_m2sdr/                # Main Python package
│   ├── gateware/               # HDL modules (Python/Migen)
│   │   ├── ad9361/             # AD9361 RFIC interface (core, phy, spi, bitmode, prbs, agc)
│   │   ├── si5351.py           # SI5351 clock generator (I2C/sequencer)
│   │   ├── capability.py       # Hardware capabilities reporting
│   │   ├── gpio.py             # GPIO packer/unpacker for DMA
│   │   ├── qpll.py             # Shared QPLL for PCIe/Eth/SATA transceivers
│   │   ├── time.py             # Hardware nanosecond time generator
│   │   ├── pps.py              # Pulse-per-second generator
│   │   ├── header.py           # TX/RX header insertion
│   │   └── measurement.py      # Multi-clock frequency measurement
│   │
│   └── software/               # Software stack
│       ├── build.py            # Orchestrated build (kernel + user + soapysdr)
│       ├── flash.py            # Bitstream flashing over PCIe
│       ├── rescan.py           # PCIe bus rescan utility
│       ├── autotest.py         # Automated hardware validation
│       │
│       ├── kernel/             # Linux kernel module (m2sdr.ko)
│       │   ├── Makefile        # Kernel module build
│       │   ├── main.c          # Module entry point
│       │   ├── liteuart.c/h    # UART driver
│       │   ├── litesata.c/h    # SATA block device driver
│       │   ├── csr.h, soc.h    # Generated headers (from gateware build)
│       │   └── litepcie.h      # LitePCIe definitions (generated)
│       │
│       ├── user/               # User-space utilities and libraries
│       │   ├── Makefile        # Builds binaries and static libraries
│       │   ├── m2sdr_util.c    # Board info, DMA test, flash operations
│       │   ├── m2sdr_rf.c      # RF front-end (AD9361) configuration
│       │   ├── m2sdr_gen.c     # Signal generator (tone/noise/PRBS)
│       │   ├── m2sdr_play.c    # TX samples from file
│       │   ├── m2sdr_record.c  # RX samples to file
│       │   ├── m2sdr_gpio.c    # GPIO control
│       │   ├── m2sdr_fm_tx.c   # FM modulation TX
│       │   ├── m2sdr_fm_rx.c   # FM demodulation RX
│       │   ├── liblitepcie/    # LitePCIe user library (DMA, helpers)
│       │   ├── libliteeth/     # LiteEth user library (Etherbone, UDP)
│       │   ├── libm2sdr/       # M2SDR board library (SI5351, AD9361, flash)
│       │   └── ad9361/         # AD9361 driver library (ADI-derived)
│       │
│       ├── soapysdr/           # SoapySDR plugin driver
│       │   ├── CMakeLists.txt  # CMake build
│       │   ├── LiteXM2SDRDevice.cpp/hpp      # Device class
│       │   ├── LiteXM2SDRStreaming.cpp        # RX/TX streaming
│       │   ├── LiteXM2SDRRegistration.cpp     # Plugin registration
│       │   └── LiteXM2SDRUDPRx.cpp/hpp        # Optional UDP RX
│       │
│       └── gnuradio/           # GNU Radio test flowgraphs (.grc files)
│
├── test/                       # Unit/integration tests (Python, use litex RemoteClient)
│   ├── test_time.py            # Hardware time generator test
│   ├── test_clks.py            # Clock frequency measurement test
│   ├── test_pcie_ltssm.py      # PCIe link training test
│   ├── test_agc.py             # AGC saturation test
│   ├── test_xadc.py            # FPGA power/temperature monitoring
│   ├── test_header.py          # TX/RX header validation
│   └── test_dashboard.py       # Real-time monitoring dashboard
│
└── doc/                        # Documentation and datasheets
    ├── ad9361/                 # AD9361 datasheets and app notes
    ├── si5351/                 # SI5351 datasheets
    └── hosts/                  # Platform-specific setup guides (RPi5, OrangePi)
```

## Build System

### Gateware (FPGA Bitstream)

The gateware is built with the LiteX framework, which uses Python/Migen to generate Verilog, then synthesizes with Xilinx Vivado.

```bash
# Build a specific configuration
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=2 --build

# Load bitstream via JTAG
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=2 --load

# Flash bitstream to SPI flash
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=2 --flash

# Build all release configurations
python3 release.py
```

**Key build options for `litex_m2sdr.py`**:
- `--variant`: `m2` (M.2 slot) or `baseboard` (Acorn Baseboard Mini)
- `--with-pcie`, `--pcie-lanes` (1/2/4), `--pcie-gen` (1/2)
- `--with-eth`, `--eth-phy` (1000basex/2500basex)
- `--with-sata`, `--sata-gen` (1/2/3)
- `--with-gpio`
- `--with-white-rabbit`
- `--with-pcie-ptm` (Precision Time Measurement)
- Debug probes: `--with-pcie-probe`, `--with-ad9361-spi-probe`, etc. (mutually exclusive)

Build output goes to `build/<build_name>/gateware/`. Generated driver headers go to `litex_m2sdr/software/kernel/`.

### Software Stack

All software is built via `litex_m2sdr/software/build.py`:

```bash
cd litex_m2sdr/software

# Build everything (kernel module + user tools + SoapySDR) for PCIe interface
python3 build.py --interface=litepcie

# Build for Ethernet interface
python3 build.py --interface=liteeth
```

#### Kernel Module

```bash
cd litex_m2sdr/software/kernel
make clean all                    # Build m2sdr.ko
sudo make install                 # Install + udev rules + auto-load
sudo make uninstall               # Remove module and config files
```

Requires Linux kernel headers. Module name: `m2sdr.ko`. Composed of: `main.o`, `liteuart.o`, `litesata.o`.

#### User-Space Utilities

```bash
cd litex_m2sdr/software/user
make clean INTERFACE=USE_LITEPCIE all    # PCIe build (default)
make clean INTERFACE=USE_LITEETH all     # Ethernet build
make install PREFIX=/usr/local           # Install binaries
make install_dev PREFIX=/usr/local       # Install libraries + headers
```

The `INTERFACE` variable controls which binaries and link dependencies are built. PCIe mode builds all utilities; Ethernet mode omits `m2sdr_gen`, `m2sdr_fm_tx`, `m2sdr_fm_rx`.

FM utilities require: `libsndfile1-dev`, `libsamplerate0-dev`.

#### SoapySDR Plugin

```bash
cd litex_m2sdr/software/soapysdr
mkdir build && cd build
cmake ../ -DCMAKE_INSTALL_PREFIX=/usr    # Add -DUSE_LITEETH=ON for Ethernet
make clean all
sudo make install
```

Requires: SoapySDR >= 0.2.1, C++17 compiler. Depends on user-space libraries being built first.

### Flashing Bitstream over PCIe

```bash
cd litex_m2sdr/software
python3 flash.py <bitstream.bin> [-o offset] [-c device_num] [-r]
```

Default flash offset: `0x00800000` (operational image). Prompts for confirmation before flashing.

## Languages and Frameworks

| Language | Usage | Key Framework |
|----------|-------|---------------|
| Python 3 | Gateware HDL, build scripts, test utilities | LiteX, Migen |
| C | Kernel module, user utilities, AD9361 driver | Linux kernel API |
| C++ | SoapySDR plugin | SoapySDR API (C++17) |
| CMake | SoapySDR build | CMake >= 3.16 |
| Make | Kernel module and user utility builds | GNU Make |

## Testing

### Unit Tests (test/ directory)

Tests in `test/` use `litex.RemoteClient` to communicate with the FPGA over Etherbone/UART. They require a running FPGA with the appropriate gateware loaded.

```bash
# Individual tests (require hardware)
python3 test/test_time.py
python3 test/test_clks.py
python3 test/test_xadc.py
```

### Automated Hardware Tests

```bash
cd litex_m2sdr/software
python3 autotest.py
```

Runs a comprehensive test suite: PCIe device detection, board info validation (voltages, temperatures, IDs), VCXO characterization, RF init across sample rates, DMA loopback, and RFIC loopback. Individual tests can be disabled with `--disable-pcie`, `--disable-info`, `--disable-rf`, etc.

### SoapySDR Tests

```bash
cd litex_m2sdr/software/soapysdr
python3 test_time.py    # Hardware time test
python3 test_play.py    # TX test
python3 test_record.py  # RX test
```

### Signal Verification

```bash
cd litex_m2sdr/software/user
python3 tone_gen.py --frequency 1e6 --amplitude 0.8 --nchannels 2 --nbits 12 --nsamples 30720 tx_file.bin
python3 tone_check.py rx_file.bin --nchannels=2 --nbits=12 --samplerate=30720000 --plot
```

## Code Conventions

### Python (Gateware)

- Uses Migen HDL syntax: `Signal()`, `If()`, `Case()`, `self.comb +=`, `self.sync +=`
- Classes inherit from `LiteXModule` (or `Module` for older code)
- CSR registers use `CSRStorage`, `CSRStatus`, `CSRField`
- Stream interfaces use `stream.Endpoint(layout)` with `valid`/`ready` handshake
- Copyright header: `Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>`
- SPDX license: `SPDX-License-Identifier: BSD-2-Clause`
- LiteX alignment style: values aligned with spaces for readability

### C (Kernel/User)

- Uses LiteX CSR register access macros from generated headers (`csr.h`, `soc.h`)
- Follows Linux kernel coding style in kernel module
- User utilities use `getopt` for CLI argument parsing
- Libraries are built as static archives (`.a`)
- Cross-compilation supported via `CROSS_COMPILE` variable

### C++ (SoapySDR)

- C++17 standard
- Implements `SoapySDR::Device` interface
- Files prefixed with `LiteXM2SDR`

### General Conventions

- File headers include copyright and SPDX license identifier
- Horizontal rule comments: `# Section Name ---...---` (dashes to column ~80)
- Code uses explicit argument naming in function calls for clarity
- Generated files (csr.h, soc.h, mem.h, litepcie.h) should not be manually edited

## Dependencies

### Gateware Build

- Python >= 3.7
- LiteX ecosystem: `litex`, `migen`, `litepcie`, `liteeth`, `litesata`, `litescope`, `litei2c`
- Xilinx Vivado (for synthesis/place-and-route)

### Software Build

- GCC / build-essential
- Linux kernel headers (for kernel module)
- CMake >= 3.16 (for SoapySDR)
- SoapySDR >= 0.2.1 development files
- `libsndfile1-dev`, `libsamplerate0-dev` (for FM utilities, PCIe mode only)

### Optional

- GNU Radio (for `.grc` flowgraph testing)
- GQRX (SDR application)
- FFmpeg (for FM audio pipelines)

## Hardware Variants

| Variant | Form Factor | PCIe Lanes | Ethernet | SATA |
|---------|-------------|------------|----------|------|
| `m2` | M.2 2280 key M slot | 1, 2, or 4 | No | No |
| `baseboard` | Acorn Baseboard Mini | 1 | Yes (SFP) | Optional |

## Important Notes for Contributors

- **Generated headers**: `csr.h`, `soc.h`, `mem.h` in `software/kernel/` are generated by the gateware build. Do not edit them manually; they are overwritten by `./litex_m2sdr.py`.
- **Interface modes**: The codebase supports two interface backends (PCIe via `USE_LITEPCIE` and Ethernet via `USE_LITEETH`). Code paths are selected at compile time via preprocessor defines and Makefile `INTERFACE` variable.
- **Clock domains**: Gateware uses multiple clock domains (`sys`, `rfic`, `idelay`). Cross-domain signals require `BusSynchronizer` or CDC primitives.
- **AD9361 driver**: The `ad9361/` directory under user contains code adapted from the Analog Devices driver. Changes should be minimal and well-justified.
- **No CI/CD**: There is no automated CI pipeline. Builds and tests are run manually. The `autotest.py` script serves as the primary validation tool for hardware-connected testing.
- **SATA polling**: The kernel module defaults to `LITESATA_FORCE_POLLING=1`. Set to 0 to allow SATA MSIs.
- **Build output**: Gateware builds go to `build/<variant_name>/gateware/`. The build name follows the pattern `litex_m2sdr_<variant>_pcie_x<lanes>[_eth][_sata][_white_rabbit]`.
