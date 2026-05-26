# Windows And macOS Ethernet Host Setup

This tutorial covers the native Windows and macOS host flow for LiteX-M2SDR
over Ethernet. PCIe remains Linux-only because it depends on the LitePCIe
kernel driver, `/dev/m2sdrN`, `ioctl`, and DMA `mmap` interfaces.

The supported Windows/macOS target is therefore:

- command-line utilities over LiteEth/Etherbone
- `libm2sdr` over LiteEth/Etherbone
- the LiteX-M2SDR SoapySDR module over LiteEth/Etherbone

The examples below assume an Ethernet bitstream built with `--with-eth`, a board
IP of `192.168.1.50`, and the default Etherbone/stream port `1234`. Replace the
address if your gateware or network setup uses a different one.

## Network Setup

Connect the board Ethernet port to the host or to the same private network as
the host. Configure the host interface on the same subnet as the board, for
example:

- board: `192.168.1.50`
- host: `192.168.1.10/24`

On Windows, allow the terminal or SDR application through the firewall on the
private network profile when prompted. If the firewall blocks UDP traffic,
Etherbone control or streaming can fail even when the build is correct.

## macOS

Install the build tools and SoapySDR with Homebrew:

```bash
brew install cmake git ninja soapysdr
```

GNU Radio is optional and can be installed separately:

```bash
brew install gnuradio
```

Clone and build the Ethernet-only host stack:

```bash
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr

cmake -S litex_m2sdr/software/user -B build/macos-liteeth -G Ninja \
  -DM2SDR_ENABLE_LITEPCIE=OFF \
  -DM2SDR_ENABLE_LITEETH=ON \
  -DM2SDR_BUILD_SOAPY=ON

cmake --build build/macos-liteeth
ctest --test-dir build/macos-liteeth --output-on-failure
```

If SoapySDR is not needed, or is not installed yet, configure with
`-DM2SDR_BUILD_SOAPY=OFF`. The CLI tools and `libm2sdr` still build.

Run the command-line tools from the build directory:

```bash
./build/macos-liteeth/m2sdr_util --device eth:192.168.1.50:1234 info
./build/macos-liteeth/m2sdr_rf --device eth:192.168.1.50:1234 --sample-rate=30.72M
```

For SoapySDR, either install the module into the Homebrew prefix:

```bash
cmake --install build/macos-liteeth --prefix "$(brew --prefix)"
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

or test the in-tree module without installing it:

```bash
export SOAPY_SDR_PLUGIN_PATH="$PWD/build/macos-liteeth/soapysdr"
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

## Windows With MSYS2/UCRT64

The MSYS2 UCRT64 environment is the simplest native Windows path when you also
want SoapySDR. Install MSYS2, open the "UCRT64" shell, then install the packages:

```bash
pacman -Syu
pacman -S --needed git \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-soapysdr
```

If MSYS2 asks you to close and reopen the shell after the first update, do that
before installing the remaining packages.

Clone and build the Ethernet-only host stack from the UCRT64 shell:

```bash
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr

cmake -S litex_m2sdr/software/user -B build/windows-liteeth -G Ninja \
  -DM2SDR_ENABLE_LITEPCIE=OFF \
  -DM2SDR_ENABLE_LITEETH=ON \
  -DM2SDR_BUILD_SOAPY=ON

cmake --build build/windows-liteeth
```

Native Windows CTest coverage is not enabled yet, so use the utility probe as
the first functional check:

```bash
./build/windows-liteeth/m2sdr_util.exe --device eth:192.168.1.50:1234 info
```

For SoapySDR, either install the module into the UCRT64 prefix:

```bash
cmake --install build/windows-liteeth --prefix /ucrt64
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

or test the in-tree module without installing it:

```bash
export SOAPY_SDR_PLUGIN_PATH="$PWD/build/windows-liteeth/soapysdr"
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

## Windows With Visual Studio

Visual Studio works for the Ethernet-only CLI tools and `libm2sdr` without
requiring MSYS2:

```powershell
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr

cmake -S litex_m2sdr/software/user -B build/windows-msvc `
  -DM2SDR_ENABLE_LITEPCIE=OFF `
  -DM2SDR_ENABLE_LITEETH=ON `
  -DM2SDR_BUILD_SOAPY=OFF

cmake --build build/windows-msvc --config Release
.\build\windows-msvc\Release\m2sdr_util.exe --device eth:192.168.1.50:1234 info
```

For Visual Studio plus SoapySDR, provide a SoapySDR CMake package through your
preferred package manager and configure with `-DM2SDR_BUILD_SOAPY=ON`. The MSYS2
flow above is usually the shorter Windows path for SoapySDR because it provides
the compiler, CMake, Ninja, and SoapySDR in one environment.

## Device Arguments

Use Ethernet device arguments on Windows and macOS:

```bash
m2sdr_util --device eth:192.168.1.50:1234 info
m2sdr_util -i 192.168.1.50 info
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
SoapySDRUtil --probe="driver=LiteXM2SDR,dev_id=eth:192.168.1.50:1234"
```

Do not use PCIe device arguments on Windows/macOS:

```bash
pcie:/dev/m2sdr0
driver=LiteXM2SDR,path=/dev/m2sdr0
```

Those require the Linux LitePCIe kernel driver and are intentionally not part of
the native Windows/macOS build.

## Troubleshooting

- If CMake cannot find SoapySDR, rebuild with `-DM2SDR_BUILD_SOAPY=OFF` to
  validate the core utilities first.
- If `SoapySDRUtil` cannot find `LiteXM2SDR`, set `SOAPY_SDR_PLUGIN_PATH` to
  the build `soapysdr` directory and retry the probe.
- If `m2sdr_util info` times out, check that the host and board are on the same
  subnet and that the OS firewall is not blocking UDP traffic.
- If multiple Ethernet boards are present, pass the explicit board IP with
  `--device eth:<ip>:1234`, `-i <ip>`, or Soapy `eth_ip=<ip>`.

Dependency package references:

- Homebrew SoapySDR formula: <https://formulae.brew.sh/formula/soapysdr>
- MSYS2 UCRT64 SoapySDR package: <https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-soapysdr>
