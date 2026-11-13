# SDR frameworks {#frameworks}

This chapter describes how the board is used from SDR applications: the SoapySDR driver, the
GNU Radio examples, GQRX, srsRAN and the SigMF recording format. Read it to connect an existing
application to the board, or to write one with the SoapySDR API in C++ or Python.

## At a glance {#fw-glance}

| Item | Value |
|----------------|-------------------------------------------------------------------------------|
| SoapySDR driver | `driver=LiteXM2SDR`, one module for PCIe and Ethernet |
| Source | `litex_m2sdr/software/soapysdr/` |
| Channels | 2 RX, 2 TX |
| Stream formats | `CF32`, `CS16`, `CS8` |
| Sample rate | 0.55 to 61.44 MSPS |
| Frequency | RX 70 MHz to 6 GHz, TX 47 MHz to 6 GHz |
| Bandwidth | 0.2 to 56 MHz |
| Gains | RX `PGA` 0 to 73 dB; TX `ATT` 0 to 89 dB of attenuation |
| Antennas | RX `A_BALANCED`, TX `A` |
| Clock sources | `internal`, `external`, `fpga` |
| Time sources | `internal`, and `ptp` on a bitstream with Ethernet PTP |
| Recording format | SigMF 1.2.6, written and read by the utilities |

The range that the driver reports extends to 122.88 MSPS. Rates above 61.44 MSPS are
Experimental ([Status](#intro-status)); see [Wide bandwidth](#flow-wide-bandwidth).

## The SoapySDR driver {#fw-soapy}

SoapySDR is a vendor-neutral SDR library: an application written for its API works with any
radio that has a driver module. The project provides such a module. With it, GQRX, GNU Radio,
srsRAN and any other SoapySDR application use the board without specific code.

The module is built on the library, as the figure below shows. The device arguments select the
board and the host link. Settings go to the RFIC and to the registers; samples go through the
DMA of the kernel driver over PCIe, or through UDP over Ethernet.

![The SoapySDR module. Applications configure the board and exchange samples through the
SoapySDR API. Follow the cyan arrow for the samples: the module converts them to the format of
the application and hands them to the library, which uses PCIe DMA or Ethernet UDP.](fw_soapy.svg)

The module is built and installed by `build.py` with the rest of the software; see
[Getting started](#getting-started). Its reference page, with every device argument, is
`litex_m2sdr/software/soapysdr/README.md`.

### Finding and probing a board {#fw-find}

`SoapySDRUtil` comes with SoapySDR. The first command below lists the boards the module finds;
the second opens one and prints its channels, ranges, gains, sensors and settings.

```sh
SoapySDRUtil --find="driver=LiteXM2SDR"
SoapySDRUtil --probe="driver=LiteXM2SDR"
```

Discovery lists the PCIe boards, then probes the default Ethernet address, `192.168.1.50` port
1234. It does not scan the network. Other Ethernet addresses are given as a list, in the device
arguments or in an environment variable.

```sh
SoapySDRUtil --find="driver=LiteXM2SDR,eth_ips=192.168.1.50;192.168.1.51:1234"
LITEXM2SDR_ETH_IPS=192.168.1.50,192.168.1.51 SoapySDRUtil --find="driver=LiteXM2SDR"
```

### Selecting a board {#fw-select}

| Device argument | Selects |
|------------------------------------------------------|-----------------------------------------|
| `path=/dev/m2sdr1` | A PCIe board by its device node |
| `eth_ip=192.168.1.50` | An Ethernet board; `eth_port` sets the port |
| `dev_id=pcie:/dev/m2sdr0` | A board by its device identifier |
| `dev_id=eth:192.168.1.50:1234` | The same, for an Ethernet board |
| `eth_discovery=0` | No Ethernet probe during discovery |

```sh
SoapySDRUtil --probe="driver=LiteXM2SDR,path=/dev/m2sdr1"
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

### Device arguments {#fw-args}

The device arguments below set the behaviour of the driver when the board is opened. All are
optional.

| Argument | Values | Effect |
|-----------------------------------|-------------------|-----------------------------------------|
| `bitmode` | `8`, `16` | Transport format: SC8 or SC16 |
| `clock_source` | `internal`, `external`, `fpga` | Reference of the clock generator |
| `refclk_freq` | Hz | Reference clock of the AD9361 |
| `refclk_ppm` | ppm | Correction of a measured reference error |
| `rx_agc_mode` | `slow`, `fast`, `hybrid`, `mgc` | RX gain control mode, both channels |
| `rx_agc_pin` | `on`, `off` | Drive the AGC enable pin of the AD9361 |
| `auto_bandwidth` | `on`, `off` | Follow the sample rate with the bandwidth |
| `ad9361_fir_profile` | `legacy`, `bypass`, `match`, `wide` | FIR profile used above 61.44 MSPS |
| `calibrate_delay` | `0`, `1` | Scan the data port delays at open |
| `eth_mode` | `udp`, `vrt` | Ethernet RX stream: raw UDP, or VRT (refused, see below) |
| `vrt_port` | port | UDP port of the VRT stream, 4991 by default |
| `tx_pacing` | `rate`, `none` | Ethernet TX pacing by the sample rate |
| `rx_dma_header` | `1`, `0` | Use the RX timestamp headers over PCIe |
| `timed_tx` | `auto`, `hardware`, `software`, `off` | Timed TX mode |
| `tx_offset` | ns | Timed TX latency compensation |
| `rx_offset` | ns | RX timestamp compensation |

The defaults are the 16-bit format, the internal reference, manual RX gain, no automatic
bandwidth, raw UDP over Ethernet, paced Ethernet TX, RX timestamp headers on, and the `auto`
timed TX mode.

Some arguments deserve a comment.

- `bitmode`: above 61.44 MSPS, a PCIe x1 board uses the 8-bit format by default to halve the
  DMA rate, while x2 and x4 boards keep 16 bits. An explicit `bitmode`, or a `CS8` stream,
  overrides the default. The formats are compared in
  [The RFIC interface](#rfic).
- `clock_source`: the same three sources are available through the standard clock API of
  SoapySDR. Changing the source is refused while a stream is open. The reference selection is
  described in [Time synchronisation](#timing).
- `refclk_ppm`: also a run-time setting. An application writes the setting `refclk_ppm` to trim
  the reference while streaming, in a range of 100 ppm on each side.
- `eth_mode=vrt`: the RX stream is meant to come in VRT packets, which carry timestamps, while
  the TX stream stays raw UDP. With the shipped software the driver refuses this mode as
  unsupported: the library has no definition of the register that selects the VRT route. See
  [VRT streaming](#flow-eth-vrt).
- Antennas: the driver exposes only the ports that the board connects. Passing another antenna
  name makes the driver fail at open.

### Sensors {#fw-sensors}

The driver publishes the state of the board as SoapySDR sensors, which `SoapySDRUtil --probe`
lists.

| Sensor | Content |
|-------------------------------------|----------------------------------------------------------|
| `fpga_temp`, `fpga_vccint` | FPGA temperature and core supply |
| `fpga_vccaux`, `fpga_vccbram` | FPGA auxiliary and block RAM supplies |
| `ad9361_temp` | Temperature of the RFIC |
| `ref_locked` | The selected reference is present and locked |
| `lo_locked` | Per channel: the local oscillator of the RFIC is locked |
| `ptp_locked`, `ptp_state` | Ethernet PTP state, on a bitstream that has it |
| `ptp_last_error_ns` | Last time error measured by the PTP discipline |

A PTP bitstream adds the identity of the PTP master: `ptp_master_ip`, `ptp_master_clock_id`,
`ptp_master_port`. An Ethernet board adds counters of the UDP stream, whose names start with
`liteeth_`.

### Time and timestamps {#fw-time}

The board time is the hardware time of the SoapySDR device, in nanoseconds. An application
reads it with `getHardwareTime()` and sets it with `setHardwareTime()`. The time sources are
`internal` and, on a bitstream with Ethernet PTP, `ptp`.

RX buffers come with the board time of their first sample. Over PCIe the driver reads it from
the header of each DMA buffer. Over Ethernet it needs `eth_mode=vrt`, which the shipped software
does not select, so Ethernet RX has no hardware timestamps. Without hardware
timestamps the driver counts samples from the time read when the stream starts.

### Timed TX {#fw-timed-tx}

A timed transmission follows the standard SoapySDR contract: the application calls
`writeStream()` with the flag `SOAPY_SDR_HAS_TIME` and a time in nanoseconds. The driver has
two ways to honour the time, selected by `timed_tx`.

| Mode | Behaviour |
|-----------------|------------------------------------------------------------------------------|
| `auto` | `hardware` when the bitstream has the timed TX gate, else `software` |
| `hardware` | The gateware holds the buffer until the board time reaches the timestamp |
| `software` | The driver fills the gap with zeros on a host timeline |
| `off` | Timestamps are ignored |

In the hardware mode the timestamp is the time on air, on the 10 ns step of the board time. A
buffer that arrives after its time is dropped whole and reported as `SOAPY_SDR_UNDERFLOW` by
`readStreamStatus()`. Writes without a time are sent at once, so continuous streaming is
unchanged. The hardware mode is available over PCIe.

In the software mode, a write that is later than the margin `tx_late_margin_ns` returns
`SOAPY_SDR_TIME_ERROR`. Two more arguments tune this mode: `tx_lead_buffers` schedules the
timeline ahead of the board time, and `tx_latency_ns` trims the latency of the pipeline.

`tx_offset` compensates the fixed delay between the gate and the antenna in the hardware mode.
Without it, the driver derives a value from the sample rate and the channel layout. The gate is
described in [Streaming](#streaming); the calibration is the recipe [Timed TX](#flow-timed-tx).

### Test scripts {#fw-tests}

Three Python scripts in `litex_m2sdr/software/soapysdr/` exercise the driver through the
SoapySDR Python API. They are short and make good starting points for an application.

```sh
./test_time.py --set now --interval 0.2 --duration 5
./test_record.py --samplerate 4e6 --freq 2.4e9 --gain 20 --secs 5 --check-ts output.bin
./test_play.py --samplerate 4e6 --freq 2.4e9 --att 20 --tone-freq 1e6 --ampl 0.8 --secs 5
```

`test_time.py` sets and reads the board time. `test_record.py` records `CF32` samples to a file
and, with `--check-ts`, prints the timestamps and their differences. `test_play.py` transmits a
tone or a file of `CF32` samples.

## GQRX {#fw-gqrx}

GQRX is a receiver with a spectrum and a waterfall. It uses the board through SoapySDR: in its
device dialog, select the LiteX-M2SDR entry or type the device string.

```text
driver=LiteXM2SDR
driver=LiteXM2SDR,eth_ip=192.168.1.50
```

The first string opens the first PCIe board, the second an Ethernet board. Choose a sample
rate that the host link carries; the limits of each link are given in [Host links](#host-links).

For an Ethernet board, the script `gqrx_liteeth.sh` in `litex_m2sdr/software/soapysdr/` builds
the module in a temporary directory, writes a GQRX configuration for the board and starts GQRX
with it. Environment variables set the address, the sample rate and the frequency.

```sh
LITEX_M2SDR_ETH_IP=192.168.1.50 LITEX_M2SDR_SAMPLE_RATE=2000000 \
    LITEX_M2SDR_FREQUENCY=100000000 ./gqrx_liteeth.sh
```

While GQRX receives from an Ethernet board with SATA, `m2sdr_sata` can record the same stream
to the SSD and replay it into GQRX; see [SATA recording](#flow-sata).

## GNU Radio {#fw-gnuradio}

GNU Radio uses the board with its Soapy source and sink blocks, with `driver=LiteXM2SDR` in the
device arguments. The directory `litex_m2sdr/software/gnuradio/` holds example flowgraphs for
GNU Radio Companion.

| Flowgraph | Content |
|-----------------------------------------|------------------------------------------------------|
| `test_rx.grc` | Two-channel receiver: display, and one file per channel |
| `test_tx.grc` | Two-channel transmitter: carrier, 1 MHz sine wave or file |
| `test_tx_rx.grc` | One-channel TX to RX test with an external loopback |
| `test_fm_rx.grc` | FM broadcast receiver at 8 MSPS with audio output |
| `test_fm_tx.grc` | FM transmitter of an audio tone |
| `test_video_5ghz_rx.grc` | Analog FM video receiver at 5825 MHz; needs an out-of-tree NTSC module |

```sh
gnuradio-companion litex_m2sdr/software/gnuradio/test_rx.grc
```

The flowgraphs have sliders for the frequency, the RX gain and the TX attenuation. Their files
hold interleaved complex floats, the `CF32` format. The reference page, with a screenshot of
each of the first four flowgraphs, is `litex_m2sdr/software/gnuradio/README.md`.

## srsRAN 4G {#fw-srsran}

The driver works with srsRAN 4G, base station and terminal, through the SoapySDR support of
srsRAN. srsRAN streams `CF32` samples, derives its TX time from the RX timestamps and schedules
each subframe a few milliseconds ahead with a timed write. The driver serves both with the RX
hardware timestamps and the timed TX.

```ini
[rf]
device_name = soapy
device_args = driver=LiteXM2SDR
tx_gain = 60
rx_gain = 40
```

- PCIe is the host link for LTE: over Ethernet the RX stream has no hardware timestamps.
- All the LTE sample rates, from 1.92 to 23.04 MSPS, are in the supported range.
- Do not pass antenna arguments such as `rxant=` or `txant=`: unknown antenna names make the
  driver fail at open.
- Keep `tx_lead_buffers=0`, the default. srsRAN already writes ahead of time, and a lead would
  shift the transmission before the requested time.
- On the srsRAN console, `L` or `T` marks a late timed write, `U` a TX underflow and `O` an RX
  overflow. They should be absent or rare; raise `time_adv_nsamples` or check the CPU load
  otherwise.

srsRAN reaches the clock sources and the sensors `ref_locked` and `lo_locked` through the
standard SoapySDR API, so its reference selection works with the board.

## SigMF {#fw-sigmf}

SigMF is an open format for recordings of radio signals. A recording is a pair of files with the
same base name: the samples in a `.sigmf-data` file and a JSON description in a `.sigmf-meta`
file. The description holds the sample format, the sample rate, the frequency and optional
annotations.

The utilities read and write SigMF, so a recording keeps its parameters and opens in other
SigMF tools.

| Utility | SigMF support |
|------------------------|-----------------------------------------------------------------------|
| `m2sdr_record` | Writes a recording with `--sigmf`, with optional annotations |
| `m2sdr_play` | Plays a recording; `--capture-index` selects a capture |
| `m2sdr_check` | Displays a recording with its capture markers |
| `m2sdr_sigmf` | Prints and validates the metadata |
| `m2sdr_sata` | Stores the metadata of each capture; imports and exports recordings |

The recordings use the SigMF data types `ci16_le` for SC16 and `ci8` for SC8. A file name given
to a utility can be the data file, the metadata file or the base name.

Two points are specific to the board. A recording made with the timestamp headers kept in the
data declares them with the SigMF field `core:header_bytes`, set to 16; `m2sdr_play` then
restores the timestamps on TX. Recordings exported from the SATA storage carry fields prefixed
by `m2sdr:` that locate the capture on the drive. The recipe is in
[Record and replay](#flow-record).

## Where to go next {#fw-next}

[Workflows](#workflows) for complete recipes; [Host software](#software) for the library under
the driver; [Streaming](#streaming) for the timestamps and the timed TX gate.
