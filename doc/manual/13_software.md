# Host software {#software}

This chapter describes the software that runs on the host: the kernel driver, the library, the
utilities and the helper scripts. It gives the overall picture: which layer does what, which
utility to use for which task and the main options. The SoapySDR driver has its own chapter,
[SDR frameworks](#frameworks).

Each part keeps a reference page in the repository, with every option and more examples. The
sections below name these files; open them when an option is not described here.

## At a glance {#sw-glance}

All the software lives in `litex_m2sdr/software/`. Building and installing it is described in
[Getting started](#getting-started).

| Part | Directory | Role |
|----------------|---------------------------------|---------------------------------------------|
| Kernel driver | `kernel/` | PCIe: registers, DMA, interrupts, clock device |
| Host link helpers | `user/liblitepcie/` | DMA helpers on top of the kernel driver |
| | `user/libliteeth/` | Etherbone register access, UDP streaming |
| | `user/libliteusb/` | USB3: register access, Bulk streaming, on libusb |
| RFIC driver | `user/ad9361/` | AD9361 configuration code |
| Library | `user/libm2sdr/` | Public C API: device, RF, streaming, time |
| Utilities | `user/` | Command-line tools named `m2sdr_*` |
| SoapySDR driver | `soapysdr/` | Module for SoapySDR applications |
| GNU Radio examples | `gnuradio/` | Flowgraphs using the SoapySDR driver |
| Build script | `build.py` | Builds and installs all of the above |

## Layers {#sw-layers}

The software is layered, as the figure below shows. The library is the single entry point to
the board (`libm2sdr`). The utilities and the SoapySDR driver are built on it, and so is any C
application. A feature added to the library is therefore available to the utilities and to
SoapySDR applications at the same time.

![Host software layers, drawn for PCIe and Ethernet. Everything goes through the library, which
selects the backend when the device is opened. Follow the cyan arrows: registers and DMA through
the kernel driver over PCIe, UDP over Ethernet.](sw_stack.svg)

The library is built once with its backends. The host link is selected at run time by the
device identifier given to the utility or to the application: no rebuild is needed to move a
program from PCIe to Ethernet or to USB3.

Over PCIe, the library reaches the board through the kernel driver: registers by MMAP, samples
by DMA. Over Ethernet, no kernel driver is involved. Registers are read and written with
Etherbone and samples travel in UDP datagrams, both on ordinary sockets.

Over USB3, no kernel driver is involved either. A third helper sits next to the two of the
figure (`libliteusb`): it uses libusb, with control transfers for the registers and Bulk
transfers for the samples. The gateware side of the three paths is described in
[Host links](#host-links).

## Device identifiers {#sw-device}

Every utility selects its board the same way. A device identifier names the host link and the
address of the board. The short options cover the common cases.

| Form | Meaning |
|--------------------------------------------------------|---------------------------------------|
| `--device pcie:/dev/m2sdr0` | PCIe board, explicit form |
| `--device /dev/m2sdr0` | PCIe board, short form |
| `-c 0`, `--device-num 0` | PCIe board `/dev/m2sdr0` |
| `--device eth:192.168.1.50:1234` | Ethernet board, explicit form |
| `--device 192.168.1.50` | Ethernet board, short form, port 1234 |
| `-i 192.168.1.50`, `--ip` | Ethernet board; `-p`, `--port` sets the port |
| `--device usb:` | USB3 board, the first one with the default identifiers |
| `--device usb:1d50:605a` | USB3 board, the first one with this vendor and product |
| `--device usb:serial=0001` | USB3 board with this serial string |
| `--device usb:path=2-1.3` | USB3 board on this bus and port path, as sysfs names it |

Without any device option, the default build opens `/dev/m2sdr0`. Scripts should always pass an
explicit device, so that they behave the same on every host.

The `usb:` forms need a library built with the USB3 backend ([USB3 helper](#sw-liteusb)). The
vendor and product of the second form are in hexadecimal; `1d50:605a` are the defaults of the
gateware.

The command below lists the boards the host can reach: the PCIe boards, the default Ethernet
address and the USB3 boards, each with the identifier that opens it.

```sh
m2sdr_util list
```

## Kernel driver {#sw-kernel}

The kernel driver is the LitePCIe driver adapted to the board (`m2sdr.ko`). It is needed for
PCIe only. It maps the registers of the SoC, allocates the DMA buffers, handles the interrupt
and creates one character device per DMA channel, named `/dev/m2sdr0`, `/dev/m2sdr1` and so on:
one per board with the default single channel.

| Function | How the host sees it |
|--------------------|---------------------------------------------------------------------------|
| Register access | `ioctl` on `/dev/m2sdrN`, used by the library |
| Sample streaming | DMA rings, by `read`/`write` or mapped in user space |
| Board time | PTP hardware clock named `m2sdr` (`/dev/ptpN`) |
| SATA host access | `LITEPCIE_IOCTL_SATA_DMA` request, when the bitstream has SATA |

The install target of the driver also enables loading at boot and adds a udev rule that gives
all users access to the device nodes.

```sh
cd litex_m2sdr/software/kernel
make
sudo make install
sudo modprobe m2sdr
dmesg | grep m2sdr
```

The DMA rings are set when the module is loaded. The defaults are tuned for throughput: 256
buffers of 8192 bytes and one interrupt every 8 buffers.

| Parameter | Default | Meaning |
|-------------------------------------|----------|------------------------------------------------|
| `dma_buffer_count` | 256 | Ring depth: a power of two, 2 to 256 |
| `dma_buffer_size` | 8192 | Buffer size in bytes, one value per DMA channel |
| `dma_buffer_per_irq` | 8 | Buffers per interrupt, one value per DMA channel |
| `ptm_offset_ns` | -500 | Offset applied to the board time read by PCIe PTM |

A buffer size is a multiple of 64 bytes, under 16 MiB, and either a multiple of the page size
or an exact divisor of it. The number of buffers per interrupt divides the ring depth and stays
below it. The driver checks these rules at load time and prints the geometry it uses.

```sh
# One DMA channel (default build).
sudo insmod m2sdr.ko dma_buffer_count=8 dma_buffer_per_irq=2
# Two DMA channels (--pcie-dmas=2): one value per channel.
sudo insmod m2sdr.ko dma_buffer_size=8192,512 dma_buffer_per_irq=8,1
dmesg | grep DMA
```

The number of values must not exceed the number of DMA channels of the build: the kernel refuses
an extra value and the module does not load.

The library and the utilities read the geometry from the driver at run time, so a change of
module parameters needs no rebuild. The low latency recipe of
[Workflows](#flow-low-latency) uses these parameters. The reference page is
`litex_m2sdr/software/kernel/README.md`.

> The header that carries the timestamps is sized for 8192-byte buffers after reset. The library
> programs the frame length for the buffer size of the channel each time a header is enabled, so
> nothing is needed with the utilities or the library. Software that bypasses the library must
> program it itself. See [Streaming](#streaming).

## Host link helpers {#sw-transports}

Three small libraries sit under the main library. Applications do not call them directly.

- `liblitepcie` comes from LitePCIe. It opens the DMA rings of the kernel driver and provides
  the read, write and zero-copy buffer functions (`litepcie_dma.c`, `litepcie_helpers.c`).
- `libliteeth` talks to the Ethernet gateware. It reads and writes registers with Etherbone
  (`etherbone.c`) and sends and receives the sample stream over UDP (`liteeth_udp.c`).
- `libliteusb` talks to the USB3 gateware through libusb. It reads and writes registers with
  control transfers (`liteusb.c`) and exchanges the sample streams over the two Bulk endpoints
  (`liteusb_stream.c`).

### USB3 helper {#sw-liteusb}

The USB3 backend is optional. The `Makefile` of the utilities builds it when pkg-config finds
libusb-1.0, and `make LITEUSB=0` leaves it out. The SoapySDR driver follows the same rule
(`ENABLE_LITEUSB`), and must be built the same way as the library it links.

```sh
sudo apt install libusb-1.0-0-dev
```

Without the backend, a `usb:` identifier is refused with an unsupported error. The gateware
side is described in [USB3 host link](#links-usb3).

| Item | Value |
|--------------------------|-------------------------------------------------------------------|
| Registers | One control transfer per access, or per block of up to 128 words |
| RX ring | 512 buffers of 8192 bytes, in transfers of 8 buffers |
| TX ring | 256 buffers of 8192 bytes, in transfers of 4 buffers |
| Events | Processed in the read and write calls: the helper has no thread |
| Interfaces | Claimed per stream: 0 for RX, 1 for TX |

Each direction claims its interface when its stream is opened. RX and TX can therefore belong
to two programs, such as `m2sdr_record` and `m2sdr_play`, and a second program that asks for a
stream already in use gets a state error. The register accesses claim nothing, so `m2sdr_util`
and `m2sdr_rf` work while another program streams.

A synchronous transmit call sends its last, partly filled transfer before it returns, so no
sample waits for the next call. `m2sdr_record` prints the count of received buffers and of
failed transfers at the end of a USB3 capture.

Two settings of the host are needed. The first gives users access to the device, with the udev
rule of the repository, which matches the default identifiers `1d50:605a`.

```sh
sudo cp litex_m2sdr/software/99-litex-m2sdr-usb3.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The second raises the memory that Linux gives to the USB transfers of user programs, 16 MB by
default. The two rings take about 6 MiB, so the default is short when RX and TX run together
with other USB devices.

```sh
echo 64 | sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb
```

The older functions that take a raw handle, `m2sdr_readl()` and `m2sdr_writel()`, accept the
handle of a USB3 device as they accept a PCIe or an Ethernet one.

The AD9361 configuration code in `user/ad9361/` is adapted from the Analog Devices driver. It
is linked inside the library, with its symbols bound locally, so that it does not collide with
the AD9361 code of another SDR driver loaded in the same process.

## The libm2sdr library {#sw-libm2sdr}

The library is the public C API of the board. It covers four things: opening a device,
configuring the RFIC, streaming samples, and reading or setting the board time. The header to
read is `libm2sdr/m2sdr.h`; every function is documented there.

### Build and link {#sw-lib-build}

```sh
cd litex_m2sdr/software/user
make
sudo make install_dev PREFIX=/usr
sudo ldconfig
```

The install target copies the headers, the static and the shared library, and the metadata for
pkg-config and CMake.

The target also installs the sources of the four example programs, in
`share/litex_m2sdr/examples` under the prefix ([Examples](#sw-lib-examples)).

```sh
pkg-config --modversion m2sdr
pkg-config --cflags --libs m2sdr
```

```cmake
find_package(m2sdr REQUIRED)
target_link_libraries(my_app PRIVATE m2sdr::m2sdr)
```

### Call sequence {#sw-lib-sequence}

A program follows the same steps as the example below, which receives one buffer of SC16
samples. Calls return 0 on success and a negative code on failure.

```c
struct m2sdr_dev *dev = NULL;
struct m2sdr_config cfg;
enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
unsigned n = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
int16_t *buf = m2sdr_alloc_buffer(format, n);

m2sdr_open(&dev, "pcie:/dev/m2sdr0");

m2sdr_config_init(&cfg);
cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
cfg.rx_freq        = 2400000000LL;
cfg.rx_gain1       = 20;
cfg.rx_gain2       = 20;
m2sdr_apply_config(dev, &cfg);

m2sdr_sync_config(dev, M2SDR_RX, format, 0, n, 0, 1000);
m2sdr_sync_rx(dev, buf, n, NULL, 1000);

m2sdr_free_buffer(buf);
m2sdr_close(dev);
```

The RF configuration is applied in one call at start-up (`m2sdr_apply_config()`). It initialises
the AD9361, the clock generator and the channel layout. Later changes use the setters, such as
`m2sdr_set_rx_frequency()` or `m2sdr_set_tx_att()`, which need an RFIC that is already
configured.

### Functions by group {#sw-lib-groups}

The list below gives the main functions of each group, without the `m2sdr_` prefix that they
all carry.

- Device: `open`, `close`, `get_device_list`, `get_device_info`.
- Capabilities: `get_capabilities`, `get_transport`.
- Registers: `reg_read`, `reg_write` and their bulk forms.
- RF setup: `config_init`, `apply_config`, `apply_config_if_needed`.
- RF at run time: `set_rx_frequency`, `set_tx_frequency`, `set_sample_rate`, `set_bandwidth`,
  `set_rx_gain`, `set_rx_gain_mode`, `set_tx_att`.
- Reference clock: `get_clock_info`, `set_refclk_ppm`, `get_refclk_ppm_window`.
- Streaming: `sync_config`, `sync_rx`, `sync_tx`.
- Zero-copy streaming: `get_buffer`, `submit_buffer`, `release_buffer`.
- Headers and timed TX: `set_rx_header`, `set_tx_header`, `set_tx_offset`, `set_rx_offset`.
- Statistics: `get_stream_stats`, `get_tx_underflow`.
- Time: `get_time`, `set_time`, `get_ptp_status`.
- Sensors: `get_fpga_dna`, `get_fpga_sensors`.
- Loopbacks: `set_dma_loopback`, `set_rfic_data_loopback`, `set_rfic_loopback`.

### Formats and buffers {#sw-lib-formats}

A stream is configured with a sample format. The formats are the transport formats of the
gateware, described in [The RFIC interface](#rfic).

| Format | Constant | Content |
|---------|---------------------------------------------|-----------------------------------------|
| SC16 | `M2SDR_FORMAT_SC16_Q11` | 16-bit I and Q, 12 significant bits |
| SC8 | `M2SDR_FORMAT_SC8_Q7` | 8-bit I and Q |
| BFP8 | `M2SDR_FORMAT_BFP8_Q11` | Encoded blocks: 8-bit mantissas and a block header |

BFP8 is an encoded block format, not a sample format. In the library one BFP8 "sample" is one
block of 1024 bytes (`M2SDR_BFP8_BLOCK_BYTES`), which holds 254 samples per channel. The library
has no decoder: a program that wants plain samples decodes the blocks itself.

> BFP8 is Experimental ([Status](#intro-status)). It does not exist on an oversampling build:
> the library refuses it there with `M2SDR_ERR_UNSUPPORTED`. Raw BFP8 blocks do not combine with
> the timestamp header.

The backend exchanges fixed buffers of 8192 bytes (`M2SDR_BUFFER_BYTES`). The buffer size given
to `m2sdr_sync_config()` is in samples and must match this size. A program that wants larger
batches asks for more samples in each receive or transmit call.

The 1T1R layout selects one channel of the RFIC, but the host stream keeps the same buffer
layout. A program that needs a stable sample layout uses the 2T2R layout and separates the
channels itself.

### Timestamps and timed TX {#sw-lib-time}

Each buffer can carry a 16-byte header with a timestamp (`M2SDR_HEADER_BYTES`). On RX, the
metadata returned by `m2sdr_sync_rx()` gives the board time of the first sample of the buffer.
On TX, a timestamp in the metadata asks the gateware to hold the buffer until the board time
reaches it. The mechanism is described in [Streaming](#streaming) and used in
[Timed TX](#flow-timed-tx).

The TX header exists on PCIe only: on another host link `m2sdr_set_tx_header()` returns
`M2SDR_ERR_UNSUPPORTED`, and so does a timed transmission.

TX timestamps are rounded to the 10 ns step of the board time (`M2SDR_TX_TIME_GRID_NS`).
Setting the board time from the host is refused, with `M2SDR_ERR_STATE`, when Ethernet PTP
disciplines it.

### Errors {#sw-lib-errors}

| Code | Meaning |
|-------------------------------------------|----------------------------------------------------|
| `M2SDR_ERR_INVAL` | Invalid argument |
| `M2SDR_ERR_IO` | Access to the board failed |
| `M2SDR_ERR_TIMEOUT` | No buffer within the timeout |
| `M2SDR_ERR_UNSUPPORTED` | Not available on this host link or this bitstream |
| `M2SDR_ERR_PARSE` | Malformed string, such as a device identifier |
| `M2SDR_ERR_RANGE` | Numeric value out of range |
| `M2SDR_ERR_STATE` | Wrong call order, such as streaming before configuration |
| `M2SDR_ERR_OVERFLOW` | The RX stream lost continuity |
| `M2SDR_ERR_UNDERFLOW` | The TX stream lost continuity |

`m2sdr_strerror()` returns a short text for a code.

### Examples {#sw-lib-examples}

Four short programs in `litex_m2sdr/doc/libm2sdr/` show the API. They are built with
`make examples` in `litex_m2sdr/software/user`. The reference page of the API is
`litex_m2sdr/doc/libm2sdr/README.md`.

| File | Shows |
|---------------------------------------|--------------------------------------------------------|
| `example_sync_rx.c` | Minimal SC16 reception |
| `example_sync_tx.c` | Minimal SC16 transmission |
| `example_tone_tx.c` | Tone transmission in SC16 or SC8 |
| `example_rx_n.c` | Reception of N buffers with their timestamps |

The utilities `m2sdr_record` and `m2sdr_play` are the complete examples of file-based reception
and transmission.

## Utilities {#sw-utilities}

The utilities are small command-line programs, one per task. They share the device options of
[Device identifiers](#sw-device) and the same option names: `--sample-rate`, `--rx-freq`,
`--tx-freq`, `--rx-gain`, `--tx-att`, `--format`. Each prints its options with `--help`.

| Utility | Task |
|--------------------------|---------------------------------------------------------------------|
| `m2sdr_util` | Board information, self tests, registers, clocks, PTP status, flash |
| `m2sdr_rf` | Configure the RFIC and the reference clock |
| `m2sdr_record` | Receive samples to a file or to a pipe, raw or SigMF |
| `m2sdr_play` | Transmit samples from a file or from a pipe, raw or SigMF |
| `m2sdr_gen` | Transmit a tone, noise, a PRBS or an OFDM signal |
| `m2sdr_check` | Inspect a recording in a window: time, spectrum, constellation |
| `m2sdr_sigmf` | Print and validate SigMF metadata |
| `m2sdr_sata` | Capture to, replay from and manage the SATA storage |
| `m2sdr_gpio` | Configure and drive the GPIO pins |
| `m2sdr_fm_tx` | FM modulate audio into an I/Q stream |
| `m2sdr_fm_rx` | FM demodulate an I/Q stream into audio |
| `m2sdr_scan` | Wideband scanner with spectrum and waterfall |

The usual order is to configure the RFIC once with `m2sdr_rf`, then to stream with
`m2sdr_record`, `m2sdr_play` or `m2sdr_gen`. The RFIC keeps its configuration between
commands. `m2sdr_scan` and the capture commands of `m2sdr_sata` configure the RFIC themselves.

TX power is set as an attenuation in dB, a positive number, in all utilities: a larger value
means less power. The reference page of the utilities is `litex_m2sdr/software/user/README.md`.

### Optional utilities {#sw-optional}

Some utilities need extra packages and are skipped by the `Makefile` when these are missing.
The other utilities, the library and the SoapySDR driver are built in every case.

| Utility | Needs |
|------------------------------|-----------------------------------------------------------------|
| `m2sdr_fm_tx`, `m2sdr_fm_rx` | libsndfile, libsamplerate |
| `m2sdr_check` | SDL2, OpenGL, the `cimgui` submodule |
| `m2sdr_scan` | SDL2, OpenGL, libpng, the `cimgui` submodule |

```sh
sudo apt install pkg-config libsndfile1-dev libsamplerate0-dev \
    libsdl2-dev libgl1-mesa-dev libpng-dev
git submodule update --init --recursive litex_m2sdr/software/user/cimgui
```

### m2sdr_util: board information and tests {#sw-util}

`m2sdr_util` is the first utility to run on a new setup and the main diagnostic tool. Its
commands are grouped below.

- Device: `list`, `info`, `reg-read`, `reg-write`.
- Tests: `scratch-test`, `clk-test`, `dma-test`, `vcxo-test`, `si5351-ppm-test`.
- Loopbacks: `fpga-loopback-test`, `fpga-phy-loopback-test`, `ad9361-loopback-test`,
  `ad9361-prbs-test`, `eth-rx-rate-sweep`.
- AGC counters: `agc-status`, `agc-counter`, `agc-clear`.
- PTP: `ptp-status`, `ptp-smoke`, `ptp-config`, `ptp-clock10-status`, `ptp-clock10-config`.
- LEDs: `led-status`, `led-control`, `led-pulse`, `led-release`.
- Clock generator: `si5351-init`, `si5351-dump`, `si5351-read`, `si5351-write`.
- RFIC: `ad9361-dump`, `ad9361-read`, `ad9361-write`, `ad9361-port-dump`, `ad9361-ensm-dump`.
- Flash: `flash-write`, `flash-read`, `flash-reload`.

```sh
m2sdr_util info
m2sdr_util scratch-test
m2sdr_util clk-test
m2sdr_util dma-test
```

`info` prints the identifier of the SoC, the API version of the gateware, the features built in
the bitstream, the FPGA sensors and the state of the chips. Check the feature list first when a
command reports that a function is missing: the features depend on the build options of the
bitstream.

The three loopback tests send a known pattern and check what comes back, each one closer to
the antenna: inside the FPGA, through the data port logic of the FPGA, and through the digital
loopback of the AD9361. They are used in [Ethernet streaming](#flow-ethernet) and explained in
[Debugging](#debugging).

Two options change the output of the status commands: `--watch` refreshes the command until
Ctrl-C, and `--json`, on the PTP status commands, prints one JSON object for scripts.

### m2sdr_rf: RFIC configuration {#sw-rf}

`m2sdr_rf` takes no command, only options. It initialises the clock generator and the AD9361
with the values given and exits. Options that are not given take their default. A run therefore
replaces the whole configuration: repeat the options to keep.

| Option | Default | Meaning |
|-----------------------------------|----------------|--------------------------------------------|
| `--sample-rate` | 30720000 | Sample rate, also written `30.72e6` or `20M` |
| `--bandwidth` | 56000000 | RF bandwidth in Hz |
| `--rx-freq` | 2400000000 | RX frequency in Hz |
| `--tx-freq` | 2400000000 | TX frequency in Hz |
| `--rx-gain` | | RX gain in dB, both channels, manual gain mode |
| `--rx-gain-mode` | | `manual`, `slow`, `fast` or `hybrid` |
| `--tx-att` | 20 | TX attenuation in dB |
| `--channel-layout` | `2t2r` | `1t1r` or `2t2r` |
| `--format` | `sc16` | `sc16`, `sc8` or `bfp8` |
| `--sync` | `internal` | Reference: `internal`, `external` or `fpga` |
| `--refclk-freq` | 38400000 | Reference clock of the AD9361 in Hz |
| `--refclk-ppm` | 0 | Correction of a measured reference error |
| `--loopback` | 0 | Internal loopback of the AD9361 |
| `--calibrate-delay` | | Scan the delays of the data port with a PRBS |

```sh
m2sdr_rf --sample-rate 30.72e6 --bandwidth 20e6 \
    --rx-freq 2.4e9 --tx-freq 2.4e9 --rx-gain 10 --tx-att 10
```

A sample rate above 61.44 MSPS selects the wide bandwidth configuration of the AD9361 by itself.
Rates above 61.44 MSPS are Experimental ([Status](#intro-status)); see
[Wide bandwidth](#flow-wide-bandwidth).

The per-channel forms `--rx-gain1`, `--rx-gain2`,
`--rx-gain-mode1` and `--rx-gain-mode2` set one channel only. The self tests of the AD9361 are
started with `--bist-tx-tone`, `--bist-rx-tone` and `--bist-prbs`.

`--refclk-ppm` removes the frequency error of the reference without an external reference.
Measure the offset of a known carrier, compute `ppm = -offset_hz / carrier_hz * 1e6` and pass
the result. A positive value means that the clock runs fast. The clock generator and the
reference selection are described in [Time synchronisation](#timing).

### Streaming utilities {#sw-streaming-tools}

`m2sdr_record` writes the RX stream to a file, and `m2sdr_play` sends a file to the TX stream.
Both accept `-` as file name, for standard output and standard input, so they chain with other
programs in a pipe.

```sh
m2sdr_record [options] [filename|-] [size]
m2sdr_play   [options] <filename|-> [loops]
```

The size given to `m2sdr_record` is a limit in bytes. The loop count of `m2sdr_play` is 1 by
default and 0 means forever.

| Option | Utility | Effect |
|--------------------------------|-----------|----------------------------------------------------|
| `--format` | both | `sc16`, `sc8` or `bfp8`; must match `m2sdr_rf` |
| `--enable-header` | record | Ask the gateware for the timestamp header |
| `--strip-header` | record | Remove the header from the output |
| `--sigmf` | record | Write a SigMF data file and its metadata |
| `--host-buffers` | both | Queue N buffers between the board and the file |
| `--prefill` | play | Wait for N queued buffers before starting |
| `--timed-start` | play | Start after the next full second of the board time |
| `--capture-index` | play | Select a capture of a SigMF recording |

The host queue of `--host-buffers` absorbs short stalls of a pipe or of a slow disk. It does
not help when the destination is slower than the stream all the time.

`m2sdr_gen` computes a signal and transmits it: `--signal tone`, `white`, `prbs` or `ofdm`,
with `--tone-freq` and `--amplitude`. With `--pps-freq` and `--gpio-pin` it also toggles a GPIO
pin at the given frequency, which needs a bitstream with the GPIO block. The option accepts
pins 0 to 3, and its help says which ones exist: pin 0 is TP1 and pin 1 is TP2.

```sh
m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1e6 --amplitude 0.5
```

`m2sdr_fm_tx` and `m2sdr_fm_rx` do not open the board. They are filters between audio and
I/Q samples, placed in a pipe with `m2sdr_play` or `m2sdr_record`. The recipe
[FM broadcast in a pipe](#flow-fm) gives the commands.

### Inspection utilities {#sw-inspect}

`m2sdr_check` opens a recording in a window and shows the I/Q samples in time, the
constellation, the spectrum, the histograms and a summary with DC offset, RMS level, clipping
and timestamps. It reads raw files and SigMF recordings.

```sh
m2sdr_check rx_file.bin --nchannels 2 --nbits 12 --sample-rate 30720000
m2sdr_check capture.sigmf-meta
```

`m2sdr_sigmf` prints the metadata of a SigMF recording in a terminal. With `--validate` it
checks the metadata and returns an error code when it is wrong. `--strict` also fails on
warnings, and `--ci` prints a short result without the messages, for automated tests.

`m2sdr_scan` sweeps a frequency range and draws a spectrum and a waterfall. It can also run
without a window and export the result.

```sh
m2sdr_scan --start-freq 88000000 --stop-freq 108000000 \
    --sample-rate 15360000 --fft-len 16384
m2sdr_scan --preset-load fm_band.scan --no-ui \
    --export-csv fm_band.csv --export-png fm_band.png
```

### m2sdr_sata: SATA storage {#sw-sata}

`m2sdr_sata` records the RX stream to an SSD, replays it, and moves recordings between the SSD
and host files. It works on named captures and keeps their SigMF metadata next to the samples
on the drive. Its commands are listed in [SATA recording](#flow-sata).

The named captures live in an index on the drive, called the SATA Capture Volume. It is not a
file system: each capture is a contiguous range of sectors, which is what the SATA streamers of
the gateware read and write. The gateware side is described in [Host links](#host-links).

### m2sdr_gpio: GPIO pins {#sw-gpio}

`m2sdr_gpio` controls the GPIO core of a bitstream built with `--with-gpio`. The core is 4 bits
wide; bits 0 and 1 reach the test points TP1 and TP2 of the board ([Hardware](#hardware)). Bits 2
and 3 reach no pin; the utility accepts 4-bit values and says so in its help. The pins are
driven either by the
registers (`--source csr`) or by the sample stream (`--source dma`).

```sh
m2sdr_gpio --device-num 0 --enable --source csr --output-data 0x2 --output-enable 0x3
```

### Python helpers {#sw-python}

Two Python programs complete the utilities in `litex_m2sdr/software/user/`.

- `m2sdr_fm_radio.py` is a small FM broadcast receiver with a graphical interface.
- `m2sdr_vrt_rx.py` receives and decodes the VRT packets of a bitstream built with the VRT
  streamer, without any other software of the project. The bitstream sends VRT packets only once
  its route is selected by hand; see [VRT streaming](#flow-eth-vrt).

Three scripts in `litex_m2sdr/software/` help with a PCIe board: `rescan.py` rescans the PCIe
bus after a bitstream is loaded, `flash.py` writes a bitstream to the flash over PCIe, and
`autotest.py` runs the main checks of `m2sdr_util` and `m2sdr_rf` in sequence.

## Scripts {#sw-scripts}

The `scripts/` directory at the top of the repository holds tools that need a board and are not
installed with the software. The table lists the ones used in this manual; the others are test
scripts described in [Debugging](#debugging).

| Script | Task |
|---------------------------------------------|--------------------------------------------------|
| `m2sdr_pcie_time_sync.py` | Make the board time follow the host clock over PCIe |
| `m2sdr_ptp_check.py` | Check the Ethernet PTP lock: `smoke`, `soak`, `calibrate` |
| `timed_tx_selftest.c` | Measure and calibrate the timed TX latency |
| `loopback_selftest.sh` | Regression sweep over a TX to RX cable |
| `pin_m2sdr_irq.sh` | Pin the DMA interrupt to one CPU core |
| `flash_release.py` | Download and flash a release image |

`m2sdr_sata_transport_check.py` completes the list: it checks SATA access over the selected
host link. The two C programs are built against the library of the repository, not against the
installed one.

```sh
make -C litex_m2sdr/software/user
make -C scripts
```

## Environment variables {#sw-env}

A few environment variables change the behaviour of the library without a rebuild. The defaults
suit normal use.

| Variable | Effect |
|--------------------------------------------|---------------------------------------------------|
| `M2SDR_RX_WAIT` | `mwaitx` selects the low latency RX wait |
| `M2SDR_FLASH_LEGACY` | `1` selects the slow flash access over Ethernet |
| `M2SDR_FLASH_GAP_FRAMES` | Pacing of the flash access over Ethernet |
| `M2SDR_OC_BBF_TUNE` | Corner of the baseband filters, wide bandwidth |
| `M2SDR_OC_RX_FIR_FILE` | RX equaliser taps, wide bandwidth |
| `M2SDR_OC_TX_FIR_FILE` | TX equaliser taps, wide bandwidth |
| `M2SDR_OC_RX_DELAY` | RX delay of the data port, wide bandwidth |
| `M2SDR_OC_TX_DELAY` | TX delay of the data port, wide bandwidth |
| `LITEXM2SDR_ETH_IPS` | Ethernet addresses probed by the SoapySDR driver |

The `M2SDR_OC_` variables are tuning and diagnostic controls of the wide bandwidth
configuration, which is Experimental; see [Wide bandwidth](#flow-wide-bandwidth).

## Versions and compatibility {#sw-versioning}

Three things carry a version, and they must agree for a setup to work.

| Item | Version | Where to read it |
|--------------|----------------|-----------------------------------------------------------------|
| Library API | 1.1.0 | `pkg-config --modversion m2sdr` |
| Library ABI | 1 | Shared library name `libm2sdr.so.1` |
| Gateware API | major.minor | `m2sdr_util info`, line "API Version" |
| Gateware features | bit mask | `m2sdr_util info`, section "Features" |

The library reads the feature bits of the bitstream when it opens a device and adapts to them.
A function that the bitstream does not have, such as the timed TX or the oversampling mode,
returns an error or falls back to software instead of misconfiguring the board. `info` prints
the USB3 bit on its line `USB3`. It does not list the timed TX bit; a program reads it with
`m2sdr_get_capabilities()`.

The addresses of the registers are compiled in the software. They come from the generated
header `litex_m2sdr/software/kernel/csr.h`, which a gateware build rewrites. The copy of the
repository matches the PCIe, Ethernet and USB3 builds, not the SATA and oversampling builds
([The committed register header](#soc-csr-header)).

> After building a bitstream with different options, rebuild the kernel driver, the utilities
> and the SoapySDR driver, so that the software and the bitstream use the same register map.
> `./build.py --clean` in `litex_m2sdr/software` does the three in one command.

Release images are named by date and published with their register map. The changes of the
project are summarised by quarter in `CHANGELOG.md` at the top of the repository.

## Where to go next {#sw-next}

[SDR frameworks](#frameworks) for SoapySDR, GNU Radio and GQRX; [Workflows](#workflows) for
complete task recipes; [Debugging](#debugging) when a utility reports an error.
