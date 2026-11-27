# SDR frameworks {#frameworks}

Read this chapter to connect an existing SDR application to the board, or to write one with the
SoapySDR API in C++ or Python. It covers the SoapySDR driver, the GNU Radio examples, GQRX,
SigDigger, srsRAN 4G, srsRAN Project with srsUE (5G SA) and the SigMF recording format.

## At a glance {#fw-glance}

| Item | Value |
|----------------|-------------------------------------------------------------------------------|
| SoapySDR driver | `driver=LiteXM2SDR`, one module for PCIe, Ethernet and USB3 |
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

The module is built on the library. The device arguments select the board and the host link.
Settings go to the RFIC and to the registers; samples go through the DMA of the kernel driver
over PCIe, through UDP over Ethernet, or through the Bulk endpoints over USB3.

![The SoapySDR module. Applications configure the board and exchange samples through the
SoapySDR API. Follow the cyan arrow for the samples: the module converts them to the format of
the application and hands them to the library, which uses the host link, such as PCIe DMA or
Ethernet UDP.](fw_soapy.svg)

The module is built and installed by `build.py` with the rest of the software; see
[Getting started](#getting-started). Its reference page, with every device argument, is
`litex_m2sdr/software/soapysdr/README.md`.

### Finding and probing a board {#fw-find}

`SoapySDRUtil` comes with SoapySDR. `--find` lists the boards the module finds; `--probe` opens
one and prints its channels, ranges, gains, sensors and settings.

```sh
SoapySDRUtil --find="driver=LiteXM2SDR"
SoapySDRUtil --probe="driver=LiteXM2SDR"
```

Discovery lists the PCIe boards, then probes the default Ethernet address, `192.168.1.50` port
1234. It does not scan the network. Other Ethernet addresses are given as a list, in the device
arguments or in an environment variable. A module built with the USB3 backend also lists the
USB3 boards, each by its bus and port path.

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
| `usb=` | The first USB3 board with the default identifiers |
| `usb=1d50:605a` | The first USB3 board with this vendor and product |
| `usb=serial=0001` | The USB3 board with this serial string |
| `usb=path=2-1.3` | The USB3 board on this bus and port path |
| `dev_id=usb:path=2-1.3` | The same, as a device identifier |
| `usb_discovery=0` | No USB3 boards in the discovery |
| `serial=4342c2c084854` | Among the boards found, the one with this serial number |

```sh
SoapySDRUtil --probe="driver=LiteXM2SDR,path=/dev/m2sdr1"
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
SoapySDRUtil --probe="driver=LiteXM2SDR,usb="
```

The value of `usb` is what follows `usb:` in a device identifier
([Device identifiers](#sw-device)). On a USB3 board the hardware key that the driver reports
ends with `-usb`.

### Device arguments {#fw-args}

These device arguments set the behaviour of the driver when the board is opened. All are
optional.

| Argument | Values | Effect |
|-----------------------------------|-------------------|-----------------------------------------|
| `bitmode` | `8`, `16` | Transport format: SC8 or SC16 |
| `clock_source` | `internal`, `external`, `fpga` | Reference of the clock generator |
| `refclk_freq` | Hz | Reference clock of the AD9361 |
| `refclk_ppm` | ppm, or `stored` | Correction of a measured reference error |
| `refclk_ppm_mode` | `si5351`, `rfic` | Where the reference error is corrected |
| `rx_agc_mode` | `slow`, `fast`, `hybrid`, `mgc` | RX gain control mode, both channels |
| `rx_agc_pin` | `on`, `off` | Drive the AGC enable pin of the AD9361 |
| `auto_bandwidth` | `on`, `off` | Follow the sample rate with the bandwidth |
| `ad9361_fir_profile` | `legacy`, `bypass`, `match`, `wide` | FIR profile used above 61.44 MSPS |
| `calibrate_delay` | `0`, `1` | Scan the data port delays at open |
| `init_samplerate` | samples per second | Sample rate and bandwidth set at open, `30.72e6` by default |
| `bypass_init` | `0`, `1` | `1`: open without initialising the clock generator and the RFIC |
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

- `bitmode`: above 61.44 MSPS, a PCIe x1 board uses the 8-bit format by default to halve the
  DMA rate, while x2 and x4 boards keep 16 bits. A USB3 board uses the 8-bit format above
  40 MSPS and the 16-bit format below, because 2T2R at 61.44 MSPS in SC16 exceeds the link. An
  explicit `bitmode`, or a `CS8` stream, overrides the default. The formats are compared in
  [The RFIC interface](#rfic).
- `init_samplerate`: the driver initialises the RFIC at open, before the application sets its
  own rate. A lower value avoids running the RFIC at 30.72 MSPS on a link or a host that cannot
  sustain it. It must be positive, and it is ignored with `bypass_init=1`.
- `bypass_init=1`: the driver attaches to a board that another program configured, such as
  `m2sdr_rf`. It reads the sample rates and the local oscillator frequencies back from the
  RFIC and starts from them; later settings reconfigure the RFIC as usual. The open fails when
  the RFIC is held in reset, and the RFIC is left running when the device is closed.
  `calibrate_delay` is ignored.
- `clock_source`: the same sources are available through the standard clock API of
  SoapySDR. Changing the source is refused while a stream is open. `external` and `fpga`
  are refused, with the clocks left as they were, when no clock reaches the reference input
  of the clock generator: without it the RFIC has no reference. The reference selection is
  described in [Time synchronisation](#timing).
- `refclk_ppm`: also a run-time setting. An application writes the setting `refclk_ppm` to trim
  the reference while streaming, in a range of 100 ppm on each side.
  `refclk_ppm=stored` takes the value of the calibration record of the board, written by
  `m2sdr_cal measure --store`; the open fails when the board has no record.
- `refclk_ppm_mode=rfic`: the correction is made in the AD9361 (LOs and sample clock) and in the
  board time, and the clock generator keeps its nominal setting. `si5351` trims the clock
  generator: all its clocks are corrected, but the trim adds a slow phase wander to the LOs
  (about 400 Hz per ppm of trim), which lowers the EVM of OFDM signals. `rfic` is the default
  when `refclk_ppm` is given at the open. Without it the default is `si5351`, and the setting
  `refclk_ppm` can be written while running; with `rfic` it cannot.
- `eth_mode=vrt`: the RX stream is meant to come in VRT packets, which carry timestamps, while
  the TX stream stays raw UDP. The driver selects the VRT route when the library is built
  against the `csr.h` of the VRT build; built against the committed `csr.h`, it refuses this mode
  as unsupported. See [VRT streaming](#flow-eth-vrt).
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

RX buffers come with the board time of their first sample. Over PCIe and USB3 the driver reads
it from the header of each buffer. Over Ethernet it needs `eth_mode=vrt`, and so software built
against the `csr.h` of the VRT build; otherwise Ethernet RX has no hardware timestamps. Without hardware
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

The Python scripts in `litex_m2sdr/software/soapysdr/` exercise the driver through the
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

`LITEX_M2SDR_DEVICE` takes any device identifier instead, for example a USB3 board:

```sh
LITEX_M2SDR_DEVICE=usb:path=2-2 LITEX_M2SDR_FREQUENCY=98200000 ./gqrx_liteeth.sh
```

While GQRX receives from an Ethernet board with SATA, `m2sdr_sata` can record the same stream
to the SSD and replay it into GQRX; see [SATA recording](#flow-sata).

## SigDigger {#fw-sigdigger}

SigDigger is a signal analyser with a spectrum, a waterfall and inspectors for digital signals.
It uses the board through SoapySDR. `suscli makeprof` creates a profile for each board it finds,
and the profile is then selected in the settings of SigDigger.

```sh
LITEXM2SDR_ETH_IPS=192.168.1.50 suscli makeprof    # Ethernet board; a PCIe board needs no variable
suscli profiles
```

- SigDigger selects the board by its serial number (`device=LiteXM2SDR,serial=...`). For an
  Ethernet board at another address than `192.168.1.50`, set `LITEXM2SDR_ETH_IPS` in the
  environment of SigDigger, to the address or to a list of addresses separated by commas.
- SigDigger starts with a manual gain of 0 dB: raise `PGA` in its source panel, or the
  spectrum only shows the noise of the converter.
- SigDigger asks every device for its external clock reference. The driver refuses it when
  no reference is connected, SigDigger logs `Failed to switch to external clock` and goes on
  with the internal reference. With a 10 MHz reference connected, it is used.
- Above a few MSPS SigDigger proposes to decimate; answer No to receive at the full rate.

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

- PCIe is the reference host link for LTE: over Ethernet the RX stream has no hardware
  timestamps yet.
- On the internal oscillator the reference can be several ppm off, which is enough to hide a
  cell from the LTE cell search at 1.8 GHz and above. Measure and store the error once with
  `m2sdr_cal measure --store`, then use `device_args = driver=LiteXM2SDR,refclk_ppm=stored`.
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

## srsRAN Project gNB and srsUE, 5G SA {#fw-srsran-5g}

A 5G standalone cell runs between two boards: the srsRAN Project gNB through its UHD backend
and SoapyUHD, and srsUE through its SoapySDR support. The setup below was measured with the two
boards cabled through attenuators, TDD with 15 kHz subcarriers, band n39 numbering moved into the
2.4 GHz ISM band by the driver's frequency offsets.

srsUE needs the fork `enjoy-digital/srsRAN_4G`, branch `srsue-nr-sa-tdd-fixes`: TDD, timing
and decoding fixes for the 5G SA terminal, which did not attach in TDD on this setup without them.

### Device arguments

gNB (`ru_sdr` section):

```yaml
device_driver: uhd
device_args: type=soapy,driver=LiteXM2SDR,refclk_ppm=stored,rx_agc_mode=mgc,tx_offset=25955,tx_freq_offset=579000000,rx_freq_offset=579000000,rf_bandwidth_scale=1.75,tx_gating=1,tx_gating_hold_ns=500000,tx_ring_buffers=24
srate: 11.52               # 23.04 at 20 MHz, with tx_ring_buffers=48
time_alignment_calibration: 0
expert_cfg:
  tx_mode: continuous
```

srsUE (`[rf]` section):

```ini
device_name = soapy
device_args = driver=LiteXM2SDR,refclk_ppm=stored,rx_agc_mode=mgc,tx_freq_offset=579000000,rx_freq_offset=579000000,tx_strict_timing=1,rf_bandwidth_scale=1.75,tx_offset=25955,tx_gating=1,tx_gating_hold_ns=50000
srate = 11.52e6            ; 23.04e6 at 20 MHz
time_adv_nsamples = -40    ; -500 at 20 MHz
```

- `refclk_ppm=stored` on both boards, after `m2sdr_cal measure --store`. The trim is then applied
  in the AD9361 (`refclk_ppm_mode=rfic`, the default with a trim): the SI5351 trim costs 2 to 3 dB
  of SNR on OFDM signals, and the terminal uplink was capped near 21 dB with it.
- `tx_freq_offset` and `rx_freq_offset` move the band n39 numbering (1899.1 MHz) to 2478.1 MHz.
  Over the air a 20 MHz carrier must stay below 2483.5 MHz: move it down by 5 MHz or more.
- `tx_strict_timing=1` on the terminal only: a terminal must hit its uplink slots, so its late
  writes are dropped and its stream caught up. On the gNB it causes underflow storms.
- `tx_gating=1` keeps the TX path off between bursts; the hold covers the gaps inside a burst
  (500 us on the gNB, which streams continuously, 50 us on the terminal).
- `tx_ring_buffers` limits how far ahead of its RX the gNB fills the TX ring. Without it the gNB
  runs up to the ring size ahead and its PRACH requests come late.
- `time_adv_nsamples` is the measured uplink timing of the terminal. At 20 MHz the default
  equivalent of `-40` at 10 MHz put the preamble too early for the gNB's PRACH detector (most
  attaches failed); `-500` gives a timing advance of 34 in the random access response.

### Cell configuration

The gNB settings that differ from srsRAN's defaults or examples:

Each setting is named by its key, with its section in parentheses where the key alone is ambiguous.

| Key | 10 MHz | 20 MHz | Why |
|----------------------------|--------|--------|----------------------------|
| `coreset0_index` (`pdcch`) | 6 | 12 | as in srsRAN's srsUE example |
| `prach_config_index` | 9 | 9 | long preamble (srsUE), in an UL slot |
| `prach_frequency_start` | default | 8 | clear of the PUCCH (PRBs 0 to 7); srsUE needs 3 or more at 20 MHz |
| `csi_rs_period` | 20 | 20 | with the offsets below, no CSI-RS in the slots of the random access response |
| CSI-RS slot offsets | 15, 15, 4 | 15, 15, 4 | measurement, zero power, tracking |
| `tdd_ul_dl_cfg` | 6 DL, 6 DL symbols, 3 UL | same | period of 10 slots |
| `min_k1` (`pucch`) | 4 | 4 | |
| `min_k2` (`pusch`) | 3 | 4 | at least the TX delay of the terminal (below) |
| PDSCH MCS, min and max | 28 | 28 | the SNR allows it |
| `max_proc_delay` | 2 | 2 | `expert_phy` |

srsUE, at 20 MHz: `[phy.nr] sa_tx_delay_ms = 4` (default 3). Decoding a full 20 MHz slot takes 1 to
2 ms on a 4-core host; with 3 ms of delay the uplink writes came too late under load, and both
directions failed. `[phy.nr] pdsch_cb_threads = 2` decodes the code blocks of a transport block in
parallel. `[phy] worker_cpu_mask` and `sync_cpu_affinity` keep the PHY workers off the core of the
receive thread.

### Measured

| | 10 MHz (11.52 MSPS) | 20 MHz (23.04 MSPS) |
|---|---|---|
| TCP down / up | 14.3 to 14.6 / 9.1 to 9.3 Mbit/s | 28.3 to 29.0 / 20.1 to 20.3 Mbit/s |
| UDP down / up | 15.0 / 9.6 Mbit/s | 29.5 to 30.1 / 20.9 to 21.0 Mbit/s |
| PUSCH SINR under load | 23 to 25 dB | 22.9 to 25 dB |
| DL / UL HARQ errors | about 4% / 0.1 to 2% | about 8 to 9% / 0.4 to 1.2% |
| Random access | first preamble in 10 of 11 attaches | 1 or 2 preambles |

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

A recording made with the timestamp headers kept in the data declares them with the SigMF field
`core:header_bytes`, set to 16; `m2sdr_play` then restores the timestamps on TX. Recordings
exported from the SATA storage carry fields prefixed by `m2sdr:` that locate the capture on the
drive. The recipe is in [Record and replay](#flow-record).

## Where to go next {#fw-next}

[Workflows](#workflows) for complete recipes; [Host software](#software) for the library under
the driver; [Streaming](#streaming) for the timestamps and the timed TX gate.
