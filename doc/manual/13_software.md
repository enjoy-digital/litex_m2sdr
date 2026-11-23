# Host software {#software}

The host software includes the kernel driver, the library, the utilities and the helper
scripts. This chapter says which layer does what and which utility to use for which task, with
the main options. The SoapySDR driver has its own chapter, [SDR frameworks](#frameworks).

Each part keeps a reference page in the repository, with every option and more examples. The
sections name these files; open them when an option is not described here.

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

The library is the single entry point to the board (`libm2sdr`). The utilities and the SoapySDR
driver are built on it, and so is any C application. A feature added to the library is therefore
available to the utilities and to SoapySDR applications at the same time.

![Host software layers, drawn for PCIe and Ethernet. Everything goes through the library, which
selects the backend when the device is opened. Follow the cyan arrows: registers and DMA through
the kernel driver over PCIe, UDP over Ethernet.](sw_stack.svg)

The library is built once with its backends. The host link is selected at run time by the
device identifier given to the utility or to the application: no rebuild is needed to move a
program from PCIe to Ethernet or to USB3.

Over PCIe, the library reaches the board through the kernel driver: registers by MMAP, samples
by DMA. Over Ethernet, no kernel driver is involved. Registers are read and written with
Etherbone and samples travel in UDP datagrams, both on ordinary sockets.

Over USB3, no kernel driver is involved either. Another helper sits next to the two of the
figure (`libliteusb`): it uses libusb, with control transfers for the registers and Bulk
transfers for the samples. The gateware side of these paths is described in
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

List the boards the host can reach, each with the identifier that opens it: the PCIe boards, the
default Ethernet address and the USB3 boards.

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

Small libraries, one per host link, sit under the main library. Applications do not call them
directly.

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

The library is the public C API of the board. It covers opening a device,
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

The target also installs the sources of the example programs, in
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

The main functions of each group, without the `m2sdr_` prefix that they all carry:

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
has no decoder yet: a program that wants plain samples decodes the blocks itself.

> BFP8 is Experimental ([Status](#intro-status)). It does not exist on an oversampling build:
> the library refuses it there with `M2SDR_ERR_UNSUPPORTED`. Raw BFP8 blocks do not combine with
> the timestamp header.

The backend exchanges fixed buffers of 8192 bytes (`M2SDR_BUFFER_BYTES`). The buffer size given
to `m2sdr_sync_config()` is in samples and must match this size. A program that wants larger
batches asks for more samples in each receive or transmit call.

The 1T1R layout selects one channel of the RFIC, but the host stream keeps the same buffer
layout. A program that needs a stable sample layout uses the 2T2R layout and separates the
channels itself.

The RX buffers come out oldest first. A program that changes the tuning or the gain and wants
the next samples to reflect it first drops the samples already received (`m2sdr_sync_rx_flush()`).
The call takes well under a millisecond. Over USB3 it also drops the samples that the gateware
held while the host was not reading.

#### Writing samples {#sw-lib-tx-size}

`m2sdr_sync_tx()` sends whole buffers of 8 KiB. When a call ends inside a buffer, the library
sends that buffer at once, with the rest filled: the transmitted signal then holds a gap. Give
each call a whole number of buffers, which `m2sdr_bytes_to_samples()` converts to samples.

Over USB3, each call ends a USB transfer. One buffer per call reaches about 300 MB/s in full
duplex; send several buffers per call above, where 4 buffers per call were tested at 368 MB/s
([USB3 host link](#perf-usb3)).

#### Lost samples {#sw-lib-loss}

`m2sdr_get_stream_stats()` reports the RX overflows and the TX underflows of a stream since its
activation. Over PCIe the library sees them on the DMA ring. Over Ethernet and USB3 the samples
are lost in the FPGA: the gateware counts them at the RFIC ([Stream loss counters](#rfic-loss))
and the library adds them to the same fields, when the bitstream has the `stream_counters`
capability bit (`M2SDR_FEATURE_STREAM_COUNTERS`).

| Field | Over Ethernet and USB3 |
|----------------------|------------------------------------------------------------------|
| `overflow_events` | Times the RX stream started losing samples |
| `overflow_buffers` | Lost RX samples, in buffers, rounded up |
| `underflow_events` | Times the RFIC ran out of TX samples; the stop of the stream is not counted |

With the TX header enabled, the end of each burst is such an interruption, so
`underflow_events` is not filled; `m2sdr_get_tx_underflow()` counts the late timed buffers.

### Timestamps and timed TX {#sw-lib-time}

Each buffer can carry a 16-byte header with a timestamp (`M2SDR_HEADER_BYTES`). On RX, the
metadata returned by `m2sdr_sync_rx()` gives the board time of the first sample of the buffer.
On TX, a timestamp in the metadata asks the gateware to hold the buffer until the board time
reaches it. The mechanism is described in [Streaming](#streaming) and used in
[Timed TX](#flow-timed-tx).

The TX header works on PCIe, USB3 and Ethernet. Over USB3 and Ethernet a program can check
the feature bits `M2SDR_FEATURE_USB3_TX_HEADER` and `M2SDR_FEATURE_ETH_TX_HEADER` of
`m2sdr_get_capabilities()`, which an older bitstream does not set: `m2sdr_set_tx_header()`
then returns `M2SDR_ERR_UNSUPPORTED`, and so does a timed transmission.

Over Ethernet, enable the headers before the streams are configured: a buffer is then one frame
of the FPGA, header included. With the RX header the library aligns the buffers on the frames,
whatever the datagram the reception starts on, and finds the frames again after lost datagrams
(`rx_frame_resyncs` in the UDP statistics, counted as RX overflows).

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

Short programs in `litex_m2sdr/doc/libm2sdr/` show the API. They are built with
`make examples` in `litex_m2sdr/software/user`. The reference page of the API is
`litex_m2sdr/doc/libm2sdr/README.md`.

| File | Shows |
|---------------------------------------|--------------------------------------------------------|
| `example_sync_rx.c` | Minimal SC16 reception |
| `example_sync_tx.c` | Minimal SC16 transmission |
| `example_tone_tx.c` | Tone transmission in SC16 or SC8 |
| `example_rx_n.c` | Reception of N buffers with their timestamps |

The utilities `m2sdr_record` and `m2sdr_play` are complete examples of file-based reception
and transmission.

## Utilities {#sw-utilities}

The utilities are small command-line programs, one per task. They share the device options of
[Device identifiers](#sw-device) and the same option names: `--sample-rate`, `--rx-freq`,
`--tx-freq`, `--rx-gain`, `--tx-att`, `--format`. Each prints its options with `--help`.

| Utility | Task |
|--------------------------|---------------------------------------------------------------------|
| `m2sdr_util` | Board information, self tests, registers, clocks, PTP status, flash |
| `m2sdr_rf` | Configure the RFIC and the reference clock |
| `m2sdr_cal` | Measure the error of the reference clock on LTE and GSM carriers, store it |
| `m2sdr_record` | Receive samples to a file or to a pipe, raw or SigMF |
| `m2sdr_play` | Transmit samples from a file or from a pipe, raw or SigMF |
| `m2sdr_gen` | Transmit a tone, noise, a PRBS, an OFDM signal or a PSK/QAM carrier |
| `m2sdr_check` | Inspect a recording in a window: time, spectrum, constellation |
| `m2sdr_analyzer` | Measure a signal, live or from a recording: spectrum, markers, channel power |
| `m2sdr_sigmf` | Print and validate SigMF metadata |
| `m2sdr_sata` | Capture to, replay from and manage the SATA storage |
| `m2sdr_gpio` | Configure and drive the GPIO pins |
| `m2sdr_fm_tx` | FM modulate audio into an I/Q stream |
| `m2sdr_fm_rx` | FM demodulate an I/Q stream into audio |
| `m2sdr_scan` | Wideband scanner with spectrum and waterfall |

The usual order is to configure the RFIC once with `m2sdr_rf`, then to stream with
`m2sdr_record`, `m2sdr_play` or `m2sdr_gen`. The RFIC keeps its configuration between
commands. `m2sdr_scan`, `m2sdr_analyzer`, `m2sdr_cal` and the capture commands of `m2sdr_sata`
configure the RFIC themselves.

TX power is set as an attenuation in dB, a positive number, in all utilities: a larger value
means less power. The reference page of the utilities is `litex_m2sdr/software/user/README.md`.

The top level of `litex_m2sdr/software/user/` holds the utilities; the rest is in
subdirectories.

| Path | Holds |
|--------------------------|---------------------------------------------------------------------|
| `m2sdr_*.c` | The utilities, one file each |
| `libm2sdr/` | The host library, and the support code of the utilities: command line, host queue, SATA capture volumes |
| `liblitepcie/`, `libliteeth/`, `libliteusb/` | Access to the board over PCIe, Ethernet and USB3 |
| `ad9361/` | Driver of the RFIC |
| `scan_ui/` | Plot and window helpers of the graphical utilities |
| `third_party/` | `kissfft` and the `cimgui` submodule |
| `python/` | Python programs |
| `tests/` | Unit tests, run by `make core-tests` |

The support code is private to the build: it is part of `libm2sdr.a`, that the utilities link,
and is neither in the shared library nor in the installed headers.

### Optional utilities {#sw-optional}

Some utilities need extra packages and are skipped by the `Makefile` when these are missing.
The other utilities, the library and the SoapySDR driver are built in every case.

| Utility | Needs |
|------------------------------|-----------------------------------------------------------------|
| `m2sdr_fm_tx`, `m2sdr_fm_rx` | libsndfile, libsamplerate |
| `m2sdr_check` | SDL2, OpenGL, the `cimgui` submodule |
| `m2sdr_scan` | SDL2, OpenGL, libpng, the `cimgui` submodule |
| The window of `m2sdr_analyzer` | SDL2, OpenGL, the `cimgui` submodule |

```sh
sudo apt install pkg-config libsndfile1-dev libsamplerate0-dev \
    libsdl2-dev libgl1-mesa-dev libpng-dev
git submodule update --init --recursive litex_m2sdr/software/user/third_party/cimgui
```

### m2sdr_util: board information and tests {#sw-util}

`m2sdr_util` is the first utility to run on a new setup and the main diagnostic tool. Its
commands, by group:

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

The loopback tests send a known pattern and check what comes back, each one closer to
the antenna: inside the FPGA, through the data port logic of the FPGA, and through the digital
loopback of the AD9361. They are used in [Ethernet streaming](#flow-ethernet) and explained in
[Debugging](#debugging).

Options change the output of the status commands: `--watch` refreshes the command until
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
the result. A positive value means that the clock runs fast. `m2sdr_cal` does this measurement
on cellular carriers, and `--refclk-ppm stored` takes the value of its record. The clock
generator and the reference selection are described in [Time synchronisation](#timing).

### m2sdr_cal: reference clock calibration {#sw-cal}

`m2sdr_cal` measures the error of the reference clock against the downlink carriers of the
cellular networks, which a macro base station keeps within 0.05 ppm. It needs an antenna on an
RX port and takes a few seconds.

```sh
./m2sdr_cal scan                                       # Find the carriers received here
./m2sdr_cal measure lte:816e6 earfcn:1300 arfcn:85     # Measure on three carriers
./m2sdr_cal measure --apply --store earfcn:1300        # Trim the reference, store the result
./m2sdr_cal show                                       # Print the stored record
./m2sdr_rf --refclk-ppm stored                         # Use it at the RF initialisation
./m2sdr_cal measure --refclk-ppm stored earfcn:1300    # Check it: what remains with the trim
```

| Signal | Meaning | Method |
|---------------|----------------------------------------------|-----------------------------------------|
| `lte:FREQ` | LTE carrier, by its centre frequency in Hz | PSS, then phase of the cyclic prefix |
| `earfcn:N` | LTE carrier, by its downlink EARFCN | The same |
| `gsm:FREQ` | GSM carrier with a BCCH, by its frequency | Frequency of the FCCH bursts |
| `arfcn:N` | GSM carrier, by its ARFCN (GSM 900, DCS 1800) | The same |

`scan` searches bands for the carriers that can be used, and prints them with a first value of
the error and the `measure` command for them. A band is given by its name (`lte20`, `lte3`,
`gsm900`, see `--help`) or as a range, `lte:791e6-821e6` or `gsm:925e6-960e6`; without a band,
the scan covers the LTE and GSM bands in use in Europe. The scan captures 5 MHz at a time at
7.68 MSPS and tests the channel raster in it, 100 kHz for LTE and 200 kHz for GSM, on the cores
of the host. A band takes a few seconds, and the default scan about half a minute.

An LTE carrier is identified on its centre only. From a raster point next to the centre the PSS
still correlates, weaker, and the error read is wrong by 5 or 10 kHz, several ppm. The scan
keeps the strongest of neighbouring detections, then keeps the carriers that agree with each
other, since they are all seen with one clock: the other detections are counted and left out.
With one band and few carriers this vote is weak, and the scan says so when no two carriers
agree. A carrier whose cyclic prefix is not clear is not listed.

`measure` configures the RFIC at 1.92 MSPS without a trim, captures each carrier a few times
(`--repeat`, `--duration`) and prints the error that each one gives. The result is their median.
Several carriers, of different operators or bands, protect against one that is off: a
repeater, or a frequency that is not the centre of a carrier. An LTE frequency must be the
centre of the carrier, where the PSS is; a signal that is not identified is reported as not
found and not used. `--max-ppm` bounds the error searched, 15 ppm by default.

`--apply` trims the reference by the result and measures again; the trim lasts until the next
RF initialisation. `--store` writes the result, with its date, the temperature of the FPGA and
the carriers used, in a file of the host: `~/.config/litex_m2sdr/cal_<FPGA DNA>.bin`.
`--store-flash` writes it in the SPI flash of the board, so that it follows the board from one
host to another. `m2sdr_rf --refclk-ppm stored` reads the flash first, then the file, and refuses
the record of another board.

`measure --refclk-ppm PPM` trims the reference by a value first, or by the one of the record
with `stored`, and reports what remains: this checks a stored calibration, close to zero while
it holds. The error of the reference is then the trim plus what remains, and it is this sum that
`--apply` and `--store` use, so that the same command refreshes a record.

The estimators and the record are functions of libm2sdr, declared in `libm2sdr/m2sdr_cal.h`:
`m2sdr_cal_lte_estimate()` and `m2sdr_cal_gsm_estimate()` take a capture at 1.92 MSPS, and
`m2sdr_cal_load()` reads the record of an open board. Another program can use them as the
utility does.

> [!WARNING]
> `--store-flash` erases and writes one block of 64 KiB of the configuration flash, at offset
> `0x00ff0000` by default: the end of the operational slot, above the bitstream. The utility asks
> before it does. A bitstream image larger than 7.9 MiB would overwrite the record.

The error of an oscillator changes with its temperature, by a few tenths of a ppm over the
range of a board: measure it warm, in the conditions of use.

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
| `--sample-rate` | play | Sample rate of the stream, to pace it over Ethernet with an older gateware |
| `--no-flow-control` | play | Ethernet: do not hold the stream back on the TX FIFO of the board |

The host queue of `--host-buffers` absorbs short stalls of a pipe or of a slow disk. It does
not help when the destination is slower than the stream all the time.

Over Ethernet nothing holds the host back by itself: UDP has no flow control, and the board
drops the datagrams that its TX FIFO cannot take, so that the signal sent is in pieces.

- **Gateware with the capability bit `eth_tx_status`:** the board gives the level of its TX
  FIFO, and libm2sdr holds `m2sdr_sync_tx()` and `m2sdr_submit_buffer()` back on it. The host
  then sends at the rate at which the board transmits, the one of its own sample clock: no
  sample rate is given, and nothing is dropped however long the transmission lasts. This serves
  `m2sdr_play`, `m2sdr_gen` and any program on these two calls.
  `m2sdr_liteeth_set_tx_flow_control()` turns it off for a program that paces its buffers
  itself, as `m2sdr_play --no-flow-control` does, and `m2sdr_liteeth_get_udp_stats()` gives
  the level, the rate found and the datagrams dropped.
- **Older gateware:** `m2sdr_play` sends the samples at their rate on the clock of the host.
  It takes the rate from a SigMF recording or from `--sample-rate`, and reads from the board
  whether the stream carries one channel or two. The TX FIFO takes the difference between the
  two clocks for minutes; over a longer run a short gap or a dropped datagram is to be expected
  from time to time. Without a rate it sends as fast as the link takes it, and says so.

```sh
m2sdr_play tx_file.bin 0                           # New gateware: paced by the board
m2sdr_play --sample-rate 7680000 tx_file.bin 0     # Older gateware: raw file at its rate
m2sdr_play capture.sigmf-meta                      # Older gateware: rate from the metadata
```

At its end, `m2sdr_play` prints the times the RFIC ran out of samples (TX underflows) and the
datagrams that the board dropped. The TX FIFO of the board holds 256 KiB, 17 ms of a 2T2R
stream at 1.92 MSPS and 4 ms at 7.68 MSPS: this is how late the host can be, with either
method. A host busy with other work underflows at the high rates.

PCIe and USB3 hold the host back by themselves.

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

| Option | Meaning |
|---|---|
| `--sample-rate` | I/Q sample rate, 1 MSPS by default |
| `--deviation` | Deviation of full-scale audio, 75 kHz by default |
| `--emphasis` | `eu` for 50 µs, `us` for 75 µs, `none` |
| `--mode` | `mono`, or `stereo` with the 19 kHz pilot and the L-R subcarrier at 38 kHz |
| `--format` | `sc16` or `sc8` I/Q samples |

Table: Main options of `m2sdr_fm_tx` and `m2sdr_fm_rx`.

The transmitter limits the audio to full scale after the pre-emphasis, so that the deviation
stays within `--deviation`, and reports the number of samples it limited. The pre-emphasis
raises the high frequencies by up to 14 dB at 15 kHz with 50 µs: a source close to full scale
needs some headroom.

In stereo, the receiver decodes the two channels once it has locked on the pilot. Without a
pilot it writes the mono signal on both channels. A small tuning offset is a constant level
after the demodulation, and is removed from the audio.

Both utilities handle the Radio Data System (RDS), the data that a station sends next to its
audio on a subcarrier at 57 kHz, three times the pilot. The transmitter adds it with
`--rds` or any of the options below, and the receiver decodes it with `--rds`.

| Option of `m2sdr_fm_tx` | Meaning |
|---|---|
| `--rds-pi` | Programme Identification, four hexadecimal digits |
| `--rds-ps` | Programme Service name, the 8 characters that a receiver displays |
| `--rds-rt` | RadioText, up to 64 characters |
| `--rds-pty` | Programme Type, 0 to 31 |
| `--rds-ct` | Clock time each minute: `now`, or a UTC time in seconds since 1970 |

Table: RDS options of `m2sdr_fm_tx`.

The transmitter sends the name and the flags in groups 0A, the text in groups 2A and the
clock time in a group 4A that ends on each minute. The subcarrier takes 4 % of the
deviation, 3 kHz, from the audio. The receiver recovers the subcarrier and the bit clock on
their own, so it also decodes a mono station. It checks each block with its checkword, drops
the wrong ones without correcting them, and prints a line on stderr each time a field
changes:

```
RDS: PI=F212 PTY=10 (Pop music) PS="M2SDR FM" RT="Hello" CT=2026-11-17 13:35 +01:00
```

The signal processing is in libm2sdr, declared in `libm2sdr/m2sdr_fm.h`. The modulator and
the demodulator work at an integer fraction of the I/Q rate, between 160 and 500 kSPS, and
the utilities resample the audio to and from this rate. Another program can use them as the
utilities do.

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

### m2sdr_analyzer: signal analyser {#sw-analyzer}

`m2sdr_analyzer` measures a signal on the RX of a board or in a recording. It only receives,
unless its test tone is asked for (`--tx-tone`); the tone is taken off the air, the TX
attenuated by 89 dB, when the tool ends. It
configures the RFIC itself and applies the calibration record of the board when it has one
([m2sdr_cal](#sw-cal)), so that the frequencies it reads are right.

```sh
m2sdr_analyzer --freq 1852.5e6 --sample-rate 7.68e6          # Window on the board RX
m2sdr_analyzer --file capture.sigmf-meta                     # Window on a recording
m2sdr_analyzer --freq 2.45e9 --report                        # One capture, figures as text
m2sdr_analyzer --file tone.bin --sample-rate 1e6 --report --json
```

| View | Shows |
|------------|---------------------------------------------------------------------------------|
| Spectrum | Live, average, max hold and min hold traces, markers, the channel, a waterfall |
| Time | I and Q, and the power, against time |
| Statistics | CCDF of the power next to the one of Gaussian noise, and Q against I |
| Phase noise | L(f) of the strongest carrier, its amplitude noise, the floor of the board |
| Modulation | Constellation, eye diagrams and error of each symbol of a PSK or QAM signal |
| LTE | Cell, resource grid, data blocks and constellations of an LTE downlink carrier |
| DVB-T | MER and its trace, constellation, channel, MER per carrier and echoes of a DVB-T multiplex |
| DAB | MER and its trace, SNR, constellation, channel, MER per carrier and echoes of a DAB/DAB+ ensemble |

The figures are next to the views, and `--report` prints the same ones for one capture:

| Figures | Content |
|----------------|-----------------------------------------------------------------------------|
| Peaks | Lines that stand above the noise, with their frequency and level |
| Tone | Level, SNR, SINAD, SFDR, THD and harmonics of the strongest line |
| Channel | Power of a channel (`--channel-bw`, `--channel-offset`), ACPR, occupied bandwidth (99 %), noise density |
| Time domain | Mean and peak power, PAPR, DC offset, I/Q gain imbalance and quadrature error, clipping |

A capture is one block of contiguous samples, `--fft-len` and `--averages` set its length and
the resolution bandwidth. The window is Hann by default; a flat-top window reads the level of a
tone within 0.01 dB, a Blackman-Harris window shows weak signals next to strong ones. The tone
figures are always taken with a Blackman-Harris window.

In the window, a left click places the selected marker, the wheel zooms around the mouse, a
right drag moves the span and a double click shows it all. A marker gives the level and the
noise density at its frequency, and its distance to the first marker. The names of the traces
are in the colour of their trace, and the top of the controls says whether the acquisition is
running or stopped. Q against I is drawn on a square, with the same scale on both axes.

> [!NOTE]
> The levels are in dBFS: the board has no absolute level calibration, and the gain of the RFIC
> changes them. `--ref-offset` adds a value measured with a known source, to read dBm at the
> connector for one gain and one frequency. The AGC is on by default; a manual gain
> (`--rx-gain`) keeps the levels comparable from one capture to the next.

When the host or the link does not follow the sample rate, samples are lost between the RFIC
and the host and a capture has a hole. `m2sdr_analyzer` reads the loss counters of the stream
around each capture, takes it again, and says so when it could not get a clean one: over 1 GbE
this is the case above about 14 MSPS, since both channels are streamed.

#### Modulation {#sw-analyzer-demod}

The Modulation view, and `--demod` with `--report`, demodulate the strongest signal of the
capture when it is a single carrier with PSK or QAM symbols under a root raised cosine, a
raised cosine or a rectangular pulse.

```sh
m2sdr_analyzer --freq 2449.9e6 --sample-rate 1.92e6 --view modulation
m2sdr_analyzer --freq 2449.9e6 --sample-rate 1.92e6 --report --demod
m2sdr_analyzer --file capture.sigmf-meta --report --demod --modulation 64qam --symbol-rate 5e6
```

The symbol rate, the roll-off and the modulation, from BPSK to 256QAM, are found from the
signal; `--symbol-rate`, `--rolloff`, `--modulation` and `--pulse` give them instead. The
sample rate is a few times the bandwidth of the signal, four samples per symbol at least.

| Figure | Meaning |
|--------------------|-------------------------------------------------------------------------|
| EVM, RMS and peak | Error vector of the symbols, in percent of the RMS of the constellation |
| MER | The same as a ratio, in dB |
| Magnitude and phase errors | The two parts of the error vector |
| Origin offset | Carrier leakage, in dB under the constellation |
| I/Q gain imbalance, quadrature error | Of the transmitter; their sign depends on the rotation found |
| Symbol rate, carrier | As measured, with the reference of the board |

- **Decisions:** the symbols are compared with the nearest points of the constellation, not
  with the symbols sent. The EVM is right while symbol errors are rare, and reads low beyond.
  The view and the report say "not locked" when the EVM is too large for the constellation,
  above a quarter of the distance between its points: the figures are then not valid.
- **Carrier loop:** the phase is followed by a loop a hundredth of the symbol rate wide, so
  slower phase noise, of the source or of the board, is not in the EVM.
- **Rotation:** the constellation is shown at one of its symmetric positions; the bits are not
  decoded.
- **Not supported:** offset QPSK, GMSK and other constant-envelope schemes, APSK, OFDM, and a
  roll-off near zero without the symbol rate given.

`m2sdr_gen --signal qam` sends random symbols of a chosen modulation, symbol rate and roll-off:
with a cable and an attenuator from TX to RX, the board is its own test source. `--no-rf-init`
then makes `m2sdr_analyzer` leave the RFIC as `m2sdr_rf` set it. The window then shows a
note in place of the controls of the board: the frequency, the rate and the gain are the ones
of `m2sdr_rf`.

```sh
m2sdr_rf --sample-rate 1920000 --rx-freq 2490000000 --tx-freq 2490400000 --tx-att 10 --rx-gain 40
m2sdr_gen --sample-rate 1920000 --signal qam --qam-modulation 64qam --qam-symbol-rate 120000 &
m2sdr_analyzer --no-rf-init --freq 2490e6 --sample-rate 1.92e6 --view modulation
```

> [!TIP]
> Keep a narrow signal away from the centre of the capture, as above with TX 400 kHz above RX.
> The RFIC removes its DC offset with a loop that also removes the lowest frequencies of a
> signal sitting on the centre: 64QAM at 30000 symbols/s read 4 to 5 % of EVM on the centre and
> 1.6 % beside it, on one board through a cable. Beside the centre, the EVM of the board, TX
> into RX, is 1.6 to 1.9 % (MER 35 dB) from 30000 to 1000000 symbols/s.

#### LTE downlink {#sw-analyzer-lte}

The LTE view, and `--lte` with `--report`, analyse one frame of an LTE downlink carrier, FDD
with a normal cyclic prefix, tuned on its centre.

```sh
m2sdr_analyzer --freq 1852.5e6 --sample-rate 7.68e6 --view lte
m2sdr_analyzer --freq 1852.5e6 --sample-rate 7.68e6 --report --lte
```

The sample rate is one of LTE, and sets how much of the carrier is analysed:

| Sample rate | Resource blocks analysed around the centre |
|-------------|-----------------------------------------------|
| 1.92 MSPS | 6 |
| 3.84 MSPS | 15 |
| 7.68 MSPS | 25 |
| 15.36 MSPS | 50 |
| 30.72 MSPS | 100 |

A carrier wider than the capture is still found and its MIB decoded, since the PSS, the SSS
and the PBCH are in its six central blocks; a narrower one is analysed whole.

| Figure | From |
|---------------------------|---------------------------------------------------------------------|
| Cell identity | PSS and SSS, kept as a cell when its MIB is read or its reference signals are found |
| Carrier offset | Cyclic prefixes, then the reference signals from one slot to the next |
| Resource blocks, antenna ports, frame number, PHICH | MIB, decoded from the PBCH with its CRC, for one or two antenna ports |
| RSRP, RSSI, PSS and SSS power | Resource grid, in dBFS |
| EVM of the reference signals | Each one against the channel given by its neighbours |
| EVM of the PBCH | Its symbols against QPSK |
| Blocks of data, their modulation and EVM | See below; one or two antenna ports |

The view draws the resource grid of the frame, the blocks of data with their modulation, and
the equalised symbols of the data, of the PBCH and of the reference signals.

- **Resource grid:** the power of each resource element, in dB over a reference signal element
  (the scale is next to it), against the time in the frame and the frequency from the centre.
- **Blocks of data:** under the grid on the same time axis, a column per slot, with the
  resource blocks numbered as in the cell: at 7.68 MSPS on a carrier of 75 blocks, blocks 25
  to 49. A carrier wider than the capture and with a different parity of blocks (75 against 6)
  has its blocks half a block off the band analysed: the ones that are whole in it are drawn.
- **Constellation:** each symbol of the data in the colour of the modulation of its block, the
  same as in the blocks of data, with the points of the modulation most used as crosses.
- **Mouse:** over the grid, the element under it and its power; over the blocks, the slot, the
  block, its modulation and its EVM.

A live cell changes from one frame to the next, and a capture can miss it. The view then
holds what it had, and says so:

- **No data read in a frame** (idle, or two layers only): the symbols of the last frame that
  had some stay in the constellation, dimmed, with the number of their frame.
- **No cell in a capture:** the last frame read stays, with its figures, under a notice in
  red, for a few captures; then the view says that no cell is found. Nothing is held across a
  change of frequency, of sample rate or of channel.

> [!IMPORTANT]
> The control channels are not decoded, so the allocation and the modulation of the data are
> not known. A block of data, a resource block in a slot, is counted as in use when it holds
> power, and is taken as the constellation that it fits best among QPSK, 16QAM, 64QAM and
> 256QAM; its EVM is against the nearest points. This is a diagnostic, not the EVM of a
> conformance test.

With two antenna ports, the usual case of a live cell, a block of data is tried as transmit
diversity and as one layer with each precoder, and is read when it fits one of them. The
report gives the blocks read in each way.

- **Two layers:** one receiver does not separate them. Such a block is counted as in use and
  not read, and is grey in the view.
- **Idle cell:** a frame can hold little more than the broadcast blocks, in QPSK. The larger
  constellations show when the cell carries traffic.
- **Four antenna ports:** the blocks of data are not analysed; the figures of the cell, of the
  reference signals and of the PBCH remain.

Not supported: TDD, the extended cyclic prefix, a PBCH on four antenna ports, the uplink.

#### DVB-T reception {#sw-analyzer-dvbt}

The DVB-T view, and `--dvbt` with `--report`, check the reception of a DVB-T multiplex
(EN 300 744), 2k or 8k, as a TV installer's meter does: the error correction and the
transport stream are not looked at. The view is live: the MER trace follows the captures, to
place an antenna. Once a capture has read the TPS, the next ones are 40 ms long and the next
capture runs while one is analysed: about twenty results a second. The TPS is read again every
15 s or so, after a change of the tuning, and when the multiplex is lost. The MER shown is the
mean of the last ten results, and the traces across the multiplex and the echoes are averaged
over about eight.

```sh
m2sdr_analyzer --freq 650e6 --view dvbt
m2sdr_analyzer --freq 650e6 --report --dvbt
m2sdr_analyzer --freq 650e6 --report --dvbt --watch
```

Without a display, `--watch` prints one line per capture until Ctrl-C: MER, margin, level,
guard interval correlation, carrier offset and strongest echo, from a board. With `--json`,
each line is a JSON object.

The sample rate is the elementary rate of the channel, 8/7 of its bandwidth: 9.142857 MSPS for
8 MHz (the default with `--dvbt`), 8 MSPS for 7 MHz, 6.857143 MSPS for 6 MHz. The view has a
button for each, and a UHF channel number (21 to 69, centred at 306 + 8 n MHz). The multiplex
may be off the centre of the capture, as with the 1/6 MHz offsets of some networks: it is found
anyway, and a button centres the capture on it.

| Figure | From |
|---------------------------|---------------------------------------------------------------------|
| Mode, guard interval | Correlation of the guard intervals, the best of 2k, 8k and 1/4 to 1/32 |
| Carrier offset | Phase of that correlation, then the continual pilots for whole carriers |
| Constellation, code rates, hierarchy, cell | TPS, from its sync word; the cell takes two frames |
| Channel, echoes | Scattered pilots of four symbols, every third carrier; the echoes from its impulse response |
| MER | Equalised data carriers against the constellation of the TPS, over all of them and per group of carriers |

The MER needed is the C/N of EN 300 744 (annex A) for the constellation and code rate of the
TPS in a Gaussian channel: 18.0 dB for 64QAM 3/4. A real receiver needs a few dB more. An
echo inside the guard interval does no harm by itself, but the notches it leaves in the
channel lower the MER of their carriers.

#### DAB reception {#sw-analyzer-dab}

The DAB view, and `--dab` with `--report` (and `--watch`), check the reception of a DAB/DAB+
ensemble (EN 300 401), transmission mode I, as a meter does: the FIC and the audio are not
looked at.

```sh
m2sdr_analyzer --freq 223.936e6 --view dab
m2sdr_analyzer --freq 223.936e6 --report --dab
m2sdr_analyzer --freq 223.936e6 --report --dab --watch
```

The sample rate is 2.048 MSPS (the default with `--dab`) and a capture is 200 ms, a whole 96 ms
frame wherever it starts. The view selects the band III block (5A to 13F) and centres the
capture on the ensemble.

| Figure | From |
|---------------------------|---------------------------------------------------------------------|
| Frame | Null symbol: the least power over its length, the power folded on the frame |
| Timing, fractional offset | Correlation of the guard intervals of the symbols of each whole frame |
| Carrier offset | Phase reference symbol against the one of the standard, carrier to carrier |
| Channel, echoes | Phase reference symbol, then all 76 symbols of the frame once their points are decided |
| MER | Data symbols (pi/4-DQPSK) equalised, their common phase removed, against the four points of their symbol |
| SNR on the carriers | Channel power against what the null symbols leave on the carriers |

The MER is below the SNR on the carriers by what the transmitter and the receiver add (phase
noise, distortion): the two read alike on a clean signal.

#### Phase noise {#sw-analyzer-pn}

The Phase noise view, and `--phase-noise` with `--report`, measure the strongest carrier of the
capture: L(f) in dBc/Hz against the offset from the carrier, the amplitude noise next to it,
the phase error integrated between two offsets (`--pn-from`, `--pn-to`), the jitter, the
residual FM and the discrete spurs.

```sh
m2sdr_analyzer --freq 2449.9e6 --sample-rate 1.92e6 --view phase-noise
m2sdr_analyzer --freq 2449.9e6 --sample-rate 1.92e6 --report --phase-noise --pn-samples 4194304
```

- **Tuning:** the capture is tuned a little off the carrier, here 100 kHz below: the centre
  holds the DC offset and the LO leakage of the receiver, and is not searched.
- **Sample rate:** the carrier must stand 10 dB above everything else in the capture, noise
  included, so a low sample rate around it is the one to use. The offsets go up to half of it.
- **Capture length:** the lowest offset is a few times the sample rate over the number of
  samples (`--pn-samples`): 2^20 samples at 1.92 MSPS start near 10 Hz.

> [!IMPORTANT]
> The reading is the phase noise of the source plus the one of the board: its synthesizers,
> its reference, and the noise of its receiver, which counts as phase for one half and as
> amplitude for the other. A source cleaner than the board reads as the board. Phase noise
> equal to the amplitude noise and flat means that the reading is the noise of the receiver:
> the signal is too weak.

To tell the two apart, the board keeps a floor: a reading taken with a source cleaner than it,
or with its own test tone through a cable from TX to RX, stored with `--pn-floor-save` or the
button of the view in `~/.config/litex_m2sdr/pn_floor_<FPGA DNA>.csv`. It is drawn under the
trace, and the points within 3 dB of it are marked: there, the reading is the board.

```sh
m2sdr_rf --sample-rate 7680000 --rx-freq 2490000000 --tx-freq 2490000000 --tx-att 10 --rx-gain 40
m2sdr_gen --sample-rate 7680000 --signal tone --tone-freq 100000 --amplitude 0.5 &
m2sdr_analyzer --no-rf-init --freq 2490e6 --sample-rate 7.68e6 --report \
    --pn-samples 4194304 --pn-floor-save
```

A floor taken this way, through a cable and an attenuator from TX to RX, holds the noise of
both synthesizers of the RFIC, TX and RX: it is an upper bound of what the board adds to an
external source, valid for the frequency it was taken at. The levels matter: the carrier is
set 10 to 20 dB under full scale, so that the noise of the receiver stays under the phase noise.

| Offset | TX and RX synthesizers together, 2490 MHz |
|---------|---------------------------------------------|
| 100 Hz | -88 dBc/Hz |
| 1 kHz | -96 dBc/Hz |
| 10 kHz | -104 dBc/Hz |
| 100 kHz | -102 dBc/Hz |
| 1 MHz | -110 dBc/Hz |

These are the readings of one board with a 30 dB attenuator, 0.5 to 0.7 degree RMS when
integrated; the amplitude noise is 6 to 20 dB lower at every offset.

`--tx-tone` sends the built-in tone of the RFIC instead, at the centre plus a thirty-second of
the sample rate, with the attenuation of `--tx-att`; with it the utility transmits by itself.
This tone is a quick check of a path, not a reference: its readings change from one run to
the next, and its own noise sets a floor near -92 dBc/Hz.

The measurements are functions of libm2sdr, declared in `libm2sdr/m2sdr_analysis.h`:
`m2sdr_psd_compute()` gives an averaged spectrum, and `m2sdr_psd_peaks()`, `m2sdr_psd_tone()`,
`m2sdr_psd_acpr()`, `m2sdr_psd_obw()` and `m2sdr_iq_stats()` the figures. `m2sdr_phase_noise()`
gives the phase noise of a carrier, `m2sdr_demod()` the symbols and the EVM of a PSK or QAM
signal, and `m2sdr_lte_analyse()` the cell and the resource grid of an LTE downlink carrier.

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

Python programs complete the utilities, in `litex_m2sdr/software/user/python/`.

- `m2sdr_fm_radio.py` is a small FM broadcast receiver with a graphical interface. It runs the
  utilities built in `litex_m2sdr/software/user/`, or else the installed ones.
- `m2sdr_vrt_rx.py` receives and decodes the VRT packets of a bitstream built with the VRT
  streamer, without any other software of the project. The bitstream sends VRT packets only once
  its route is selected by hand; see [VRT streaming](#flow-eth-vrt).

Scripts in `litex_m2sdr/software/` help with a PCIe board: `rescan.py` rescans the PCIe
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

Several things carry a version, and they must agree for a setup to work.

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
