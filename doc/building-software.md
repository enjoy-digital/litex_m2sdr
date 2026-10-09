# Building And Installing The Software

The host software is made of the Linux kernel driver, the user-space utilities, the public C API
(`libm2sdr`) and the SoapySDR module. It uses `make` and CMake under the hood, with a small Python
wrapper (`litex_m2sdr/software/build.py`) to build everything in one step.

## Prerequisites

On Ubuntu:

```bash
sudo apt install build-essential cmake git \
  pkg-config libsdl2-dev libgl1-mesa-dev \
  libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
  gnuradio gnuradio-dev libgnuradio-soapy3.10.9t64 gqrx-sdr \
  libsndfile1-dev libsamplerate0-dev
```

For other distributions (Fedora, Arch, ...), install the equivalent packages with your package
manager.

> [!WARNING]
> If an error related to DKMS appears during installation, run
> `sudo apt remove --purge xtrx-dkms dkms` and then re-run the installation command.

## Build With `build.py`

```bash
cd litex_m2sdr/software
./build.py          # build only (as a normal user)
sudo ./build.py     # build and install
```

- Builds the kernel driver, the user-space utilities, `libm2sdr`, and the SoapySDR driver.
- The default build is a runtime PCIe/Ethernet build: the same `libm2sdr`, user tools and
  SoapySDR module can open PCIe or Ethernet devices from the device arguments. Use
  `--interface=litepcie` or `--interface=liteeth` only when you want the legacy shorthand/default
  transport to favor one side during local testing.
- Builds are incremental; use `./build.py --clean` for a full rebuild.
- Run as a normal user, `./build.py` does not install. `sudo ./build.py` also installs the kernel
  driver (and loads it at boot), the user-space utilities, the `libm2sdr` development files and the
  SoapySDR module under `--prefix` (default `/usr`).
- Other options: `--prefix`, `--no-install`, `--skip-kernel` (see `./build.py --help`).

### Optional GUI Tools

`m2sdr_check` and `m2sdr_scan` are optional SDL/OpenGL GUI tools. They are built only when
SDL2/OpenGL development packages are installed and the pinned `cimgui` submodule is populated
(`m2sdr_scan` also needs libpng):

```bash
git submodule update --init --recursive litex_m2sdr/software/user/cimgui
```

If these optional GUI dependencies are absent, only the affected GUI tools are skipped; the CLI
tools, `libm2sdr`, and the SoapySDR module still build normally.

## Manual Install

If you did not use `sudo ./build.py`, install each component manually.

Kernel driver:

```bash
cd litex_m2sdr/software/kernel
sudo make install
sudo insmod m2sdr.ko # Optional if you do not want to reboot yet.
```

SoapySDR module:

```bash
cd litex_m2sdr/software/soapysdr/build
sudo make install
```

## Installing `libm2sdr` For External Applications

`sudo ./build.py` also installs the public C API headers, libraries and pkg-config/CMake metadata
under `--prefix` (default `/usr`). To install them manually or under another prefix:

```bash
cd litex_m2sdr/software/user
make
sudo make install_dev PREFIX=/usr/local
sudo ldconfig
```

External applications then only need to link `libm2sdr`. See the
[libm2sdr documentation](../litex_m2sdr/doc/libm2sdr/README.md).

## Development Builds And Tests

Rebuild and reload the kernel driver:

```bash
cd litex_m2sdr/software/kernel
make clean all
sudo make install
sudo insmod m2sdr.ko # To avoid having to reboot the machine.
```

Rebuild and try the user-space utilities:

```bash
cd litex_m2sdr/software/user
make clean all
./m2sdr_util info
./m2sdr_rf --sample-rate=30720000 --tx-freq=2400000000 --rx-freq=2400000000
./m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1000000 --amplitude 0.5
```

Build and run the `libm2sdr` examples:

```bash
cd litex_m2sdr/software/user
make examples
../../doc/libm2sdr/example_sync_rx > /tmp/rx.iq
../../doc/libm2sdr/example_tone_tx
```

`libm2sdr` is the common host interface used by the user utilities and the SoapySDR module, so the
example code is the reference starting point for new host applications.

Check SoapySDR detection and run the GNU Radio FM receiver example:

```bash
SoapySDRUtil --probe="driver=LiteXM2SDR"
gnuradio-companion litex_m2sdr/software/gnuradio/test_fm_rx.grc
```

### Test Layout

- Gateware simulation/unit tests live in `test/` and are CI-safe (no hardware needed):
  `python3 -m pytest -v test`.
- Board control/debug scripts live in `scripts/` and require a running board/server, e.g.
  `python3 scripts/test_xadc.py` or `python3 scripts/test_dashboard.py`.
- CI runs the software build checks (kernel/user/SoapySDR) and the simulation tests.

See [Regression Tests](debugging-guide.md#regression-tests) in the Debugging Guide for the
per-layer test commands.
