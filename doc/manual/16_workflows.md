# Workflows {#workflows}

This chapter is a set of task recipes: the commands to run, in order, and what to look for.
Each recipe states the bitstream and the host link it needs, then links to the chapter that
explains the mechanism. Start with [Getting started](#getting-started) if the software is not
installed or the board has never been checked.

## At a glance {#flow-glance}

| Recipe | Host link | Bitstream needs |
|----------------------------------------------------|-------------|-----------------------------|
| [First loopback](#flow-first) | any | nothing specific |
| [Record and replay](#flow-record) | any | nothing specific |
| [FM broadcast in a pipe](#flow-fm) | any | nothing specific |
| [Ethernet streaming](#flow-ethernet) | Ethernet | `--with-eth` |
| [SATA recording](#flow-sata) | PCIe, Ethernet | `--with-sata` |
| [Wide bandwidth](#flow-wide-bandwidth) | PCIe | Experimental; see the recipe |
| [Timed TX](#flow-timed-tx) | PCIe | nothing specific |
| [Low latency](#flow-low-latency) | PCIe | nothing specific |
| [Several boards in one host](#flow-multi-board) | PCIe, Ethernet | nothing specific |
| [Time synchronisation](#flow-time-sync) | PCIe, Ethernet | depends on the method |

The status of each feature is in the [status table](#intro-status). The build options are
listed in [Build options](#build-options), and building or flashing a bitstream is described in
[Building the gateware](#building-gateware).

The commands assume that the utilities are installed and in the path. From a source tree
without installation, run them from `litex_m2sdr/software/user` with a `./` prefix. Commands
that start with `./litex_m2sdr.py` or `scripts/` run from the top of the repository.

A recipe written for one host link works on the other by changing the device option: `-c 0`
for a PCIe board, `-i 192.168.1.50` for an Ethernet board. See
[Device identifiers](#sw-device).

## First loopback {#flow-first}

This recipe sends a tone, receives it through the internal loopback of the RFIC and displays
it. It needs no antenna and no cable, and it exercises the whole chain: TX stream, RFIC, RX
stream, host.

Configure the RFIC with its internal loopback enabled.

```sh
m2sdr_rf --sample-rate 30720000 --bandwidth 40000000 \
    --tx-freq 2400000000 --rx-freq 2400000000 \
    --tx-att 10 --rx-gain 10 --loopback 1
```

Start the tone generator in a first terminal. It runs until Ctrl-C.

```sh
m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1000000 --amplitude 0.8
```

Record the RX stream in a second terminal, then open the file.

```sh
m2sdr_record rx_file.bin 2000000
m2sdr_check rx_file.bin --nchannels 2 --nbits 12 --sample-rate 30720000
```

`m2sdr_check` must show a sine wave in the time view, a circle in the constellation and one
line at 1 MHz in the spectrum. Run `m2sdr_rf` again without `--loopback` to return to the
antennas.

## Record and replay {#flow-record}

A recording is either a raw file of interleaved I/Q samples or a SigMF recording, which adds a
metadata file. SigMF is the better choice for anything kept: the sample rate, the frequency and
the format travel with the samples. See [SigMF](#fw-sigmf).

### Record {#flow-record-rx}

Configure the RFIC, then record. The last argument is the size of the recording in bytes; one
SC16 sample of one channel takes 4 bytes.

```sh
m2sdr_rf --sample-rate 30720000 --rx-freq 2400000000 --rx-gain 20
m2sdr_record --sigmf --sample-rate 30720000 --center-freq 2400000000 capture 200000000
```

This writes `capture.sigmf-data` and `capture.sigmf-meta`. The options `--sample-rate` and
`--center-freq` of `m2sdr_record` only fill the metadata: they must repeat the values given to
`m2sdr_rf`.

To keep the timestamps, ask for the header and let the utility strip it from the data. With
`--annotate-ts-jumps`, a jump of the timestamps, which marks lost samples, becomes a SigMF
annotation.

```sh
m2sdr_record --sigmf --sample-rate 30720000 --center-freq 2400000000 \
    --enable-header --strip-header --annotate-ts-jumps capture 200000000
```

An annotation can also be added by hand, to mark a part of the recording.

```sh
m2sdr_record --sigmf --sample-rate 30720000 \
    --annotation-label burst --annotation-comment "loopback capture" \
    --annotation-start 0 --annotation-count 8192 capture 2000000
```

### Inspect {#flow-record-check}

```sh
m2sdr_sigmf capture.sigmf-meta
m2sdr_sigmf --validate --strict capture.sigmf-meta
m2sdr_check capture.sigmf-meta
```

The first command prints the metadata, the second checks it, the third opens the recording in
a window. Look at the clipping count and the RMS level of the summary to judge the RX gain.

### Replay {#flow-record-tx}

`m2sdr_play` reads the format from the metadata. The second argument is the number of loops;
0 loops forever. Set the TX frequency and attenuation with `m2sdr_rf` first.

```sh
m2sdr_rf --sample-rate 30720000 --tx-freq 2400000000 --tx-att 20
m2sdr_play capture.sigmf-meta 10
m2sdr_play --capture-index 1 capture.sigmf-meta 1
```

Raw files work the same way, with the format given on the command line.

```sh
m2sdr_record --format sc16 rx_file.bin 200000000
m2sdr_play --format sc16 rx_file.bin 10
```

When the source or the destination can stall for a moment, such as a pipe or a shared disk,
add a host queue. `--prefill` delays the start of the playback until the queue holds enough
buffers.

```sh
m2sdr_record --host-buffers 64 rx_file.bin 200000000
m2sdr_play --host-buffers 64 --prefill 16 tx_file.bin 1
```

> Recordings grow fast. At 30.72 MSPS, a 2T2R recording in SC16 takes 245.76 MB per second.
> Record to a disk that sustains the rate, or lower the sample rate, use SC8 or use one channel.

## FM broadcast in a pipe {#flow-fm}

The FM utilities are filters between audio and I/Q samples. Chained with `m2sdr_record` and
`m2sdr_play` in a pipe, they make an FM receiver and an FM transmitter. `ffmpeg` converts and
plays the audio.

### Receive {#flow-fm-rx}

```sh
m2sdr_rf --channel-layout 1t1r --sample-rate 1000000 --bandwidth 1500000 \
    --rx-freq 100000000 --rx-gain 40
m2sdr_record - \
| m2sdr_fm_rx --sample-rate 1000000 --deviation 75000 --bits 12 \
    --emphasis eu --mode stereo - - \
| ffmpeg -f s16le -ac 2 -ar 44100 -i - -f alsa default
```

Set `--rx-freq` to a local station. The de-emphasis is `eu` or `us` depending on the region.

### Transmit {#flow-fm-tx}

```sh
m2sdr_rf --channel-layout 1t1r --sample-rate 1000000 --bandwidth 1500000 \
    --tx-freq 100000000 --tx-att 20
ffmpeg -re -stream_loop -1 -i music.mp3 -f s16le -ac 1 -ar 44100 - \
| m2sdr_fm_tx --sample-rate 1000000 --deviation 75000 --bits 12 \
    --emphasis eu --mode mono --input-channels 1 --input-sample-rate 44100 - - \
| m2sdr_play --format sc16 -
```

> Transmit in the FM broadcast band only into a cable with attenuators or a shielded setup.

## Ethernet streaming {#flow-ethernet}

Over Ethernet, the board is controlled and streams samples through an SFP module of the LiteX
Acorn Baseboard Mini. The sample rate is limited by the rate of the link. The Ethernet PHYs and
the limits they set are described in [Host links](#host-links).

### Build and reach the board {#flow-eth-build}

Build and load a bitstream for the baseboard. The PHY is 1000BASE-X by default; `--eth-phy`
selects 2500BASE-X or the experimental 5GBASE-R.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build --load
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --eth-phy=2500basex \
    --build --load
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --eth-phy=5000baser \
    --build --load
```

The board answers at `192.168.1.50`; the build option `--eth-local-ip` changes the address.
Give the host interface an address in the same network, then check the link.

```sh
ping 192.168.1.50
m2sdr_util -i 192.168.1.50 info
```

`info` must print the identifier of the SoC and, in its feature list, the line `Ethernet` with
the value `Yes`. No kernel driver is needed.

Notes on the SFP modules:

- The link and the traffic do not need the EEPROM of the SFP module. A module whose EEPROM does
  not answer is not a link failure.
- 2500BASE-X was validated in the SFP 0 cage with a LianGuo LG 2.5GE copper SFP module and the
  jumpers JP1 and JP4 of the baseboard fitted.
- 5GBASE-R needs a module whose host side runs 5GBASE-R at 5.15625 Gbaud. Many multi-rate
  copper modules default to a fixed 10 Gbps host side. Such a module must be switched to the
  5GBASE-R host mode through its management interface before use; it keeps the setting while
  powered. The module tested on the baseboard is the H!Fiber ASF-10G-T80; see
  [Host links](#host-links).

### Check the stream path {#flow-eth-check}

Before starting an application, run the loopback tests of `m2sdr_util`. They send a pattern
from the host and check what returns, first through the data port logic of the FPGA, then
through the digital loopback of the AD9361.

```sh
m2sdr_util -i 192.168.1.50 --duration 4 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-phy-loopback-test
m2sdr_util -i 192.168.1.50 --duration 8 --pace=rx --sample-rate 1920000 --window 32 \
    ad9361-loopback-test
```

Each test must end with a line starting with `PASS` and report 0 errors. The loopback inside
the FPGA, without the RFIC, is a stress test of the link at a chosen rate.

```sh
m2sdr_util -i 192.168.1.50 --duration 3 --pace=rate --sample-rate 30720000 --window 32 \
    fpga-loopback-test
```

The tests reset the stream state of the FPGA when they start and when they end, so they can
run after a SoapySDR or GQRX session. Add `--verbose` for the RF setup log and the UDP
counters. [Debugging](#debugging) explains the counters.

### Stream {#flow-eth-stream}

All the utilities take the address of the board with `-i`.

```sh
m2sdr_rf -i 192.168.1.50 --channel-layout 1t1r --sample-rate 4000000 \
    --rx-freq 100000000 --rx-gain 20
m2sdr_record -i 192.168.1.50 rx_file.bin 16000000
m2sdr_play -i 192.168.1.50 --format sc16 tx_file.bin 1
```

With SoapySDR, add the address to the device string.

```sh
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50"
```

When the driver warns that the UDP receive buffer is capped, raise the limit of the host.

```sh
sudo sysctl -w net.core.rmem_max=8388608
```

### VRT streaming {#flow-eth-vrt}

A bitstream built with `--with-eth-vrt` can send the RX stream as VRT packets, with timestamps,
to the address and port set by `--vrt-dst-ip` and `--vrt-dst-port`. The default destination is
the multicast group `239.168.1.100`, port 4991. The mechanism is in
[VRT streamer](#links-eth-vrt).

> After load the bitstream sends raw UDP, not VRT, and the shipped software cannot select the VRT
> route: the library has no definition of the register that selects it. The SoapySDR argument
> `eth_mode=vrt` is refused as unsupported for the same reason.

Until the software selects it, write the route by hand: the value 2 in the register
`main_eth_rx_mode`, through the Etherbone bridge and with the register file of the build.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-eth-vrt --build --load
litex_server --udp --udp-ip=192.168.1.50 --udp-port=1234
litex_cli --csr-csv=scripts/csr.csv --write main_eth_rx_mode 2
python3 litex_m2sdr/software/user/m2sdr_vrt_rx.py --group 239.168.1.100 \
    --port 4991 --count 20
```

`litex_server` keeps running: start it in a second terminal. The receiver prints the header
fields of each packet. VRT is Simulation-tested ([Status](#intro-status)).

### Combinations {#flow-eth-combine}

| With | Option | Notes |
|------------|------------------------------------------|------------------------------------------|
| SATA storage | `--with-sata` | See [SATA recording](#flow-sata) |
| Ethernet PTP | `--with-eth-ptp` | See [Time synchronisation](#flow-time-sync) |
| PTP RFIC reference | `--with-eth-ptp-rfic-clock` | SI5351C version of the board |
| VRT | `--with-eth-vrt` | RX only, selected by hand |

PCIe, Ethernet and SATA cannot all be in one bitstream, and Ethernet PTP excludes White Rabbit.
The reason is in [Host links](#host-links).

## SATA recording {#flow-sata}

With SATA, the board records the RX stream to an SSD and replays it without the host in the
sample path. The host only starts and stops the transfers, so the recording rate does not
depend on the host link. The SATA connector is on the baseboard. The gateware side is described
in [Host links](#host-links).

The figure below shows the paths used by the commands of `m2sdr_sata`.

![Paths of the SATA commands. `capture` records the RX stream of the RFIC and `play` replays a
capture to its TX stream. `serve` replays a capture into the RX path of the host, where an
application sees it as live samples. `import` and `export` copy between the SSD and host
files.](flow_sata.svg)

### Commands {#flow-sata-commands}

| Command | Action |
|-----------------------------|------------------------------------------------------------------|
| `info` | Host link, SATA link, drive, volume state |
| `init` | Create the SATA Capture Volume |
| `list`, `show` | List the captures; show one capture and its metadata |
| `check` | Check the names and the sector ranges of the volume |
| `capture` | Configure the RFIC and record the RX stream |
| `capture-start` | The same, returning at once |
| `capture-current` | Record the stream an application is receiving, without retuning |
| `play` | Replay a capture to the TX stream |
| `serve` | Replay a capture into the RX path of the host |
| `import` | Copy a raw file or a SigMF recording to the SSD |
| `export` | Copy a capture to a SigMF recording, or to a raw file with `--raw` |
| `delete` | Remove a capture from the volume |
| `stop` | Reset the RX streamer, the TX streamer or both |
| `diag` | Raw sector, routing, pattern and throughput diagnostics |

`--dry-run` in front of a command prints what it would do without starting it.

### Bring-up {#flow-sata-bringup}

Build a bitstream with SATA, over Ethernet or over PCIe, then rebuild the software so that it
matches the bitstream.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-sata --build --load
./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-sata --build --load
```

Always start with `info`. It reports the host link in use, the state of the SATA link, the
identity of the drive and the state of the volume. Then create the volume once.

```sh
m2sdr_sata -i 192.168.1.50 info
m2sdr_sata -i 192.168.1.50 init
```

`init` creates the volume when there is none. It refuses to reset a volume that holds captures
unless `--force` is given. Resetting the volume or deleting a capture removes index entries
only: the sample sectors are not erased.

Over PCIe, rescan the bus after loading the bitstream and use `-c 0` in place of the address.

```sh
sudo litex_m2sdr/software/rescan.py
m2sdr_sata -c 0 info
```

### Capture from the RFIC {#flow-sata-capture}

`capture` configures the RFIC, records for the given duration and adds a named capture with its
metadata. Check the plan first with `--dry-run`.

```sh
m2sdr_sata -i 192.168.1.50 --dry-run capture fm_test --seconds 2 \
    --sample-rate 4M --format sc16 --channel-layout 1t1r \
    --rx-freq 100M --rx-gain 20 --bandwidth 5M
m2sdr_sata -i 192.168.1.50 capture fm_test --seconds 2 \
    --sample-rate 4M --format sc16 --channel-layout 1t1r \
    --rx-freq 100M --rx-gain 20 --bandwidth 5M
```

`--size` gives the length in bytes in place of `--seconds`. Then list and inspect.

```sh
m2sdr_sata -i 192.168.1.50 list
m2sdr_sata -i 192.168.1.50 show fm_test
m2sdr_sata -i 192.168.1.50 check
```

### Capture what GQRX receives {#flow-sata-gqrx}

`capture-current` records the RX stream that an application is receiving, while the application
keeps running. It reads the sample rate, the format, the channel layout, the frequency and the
bandwidth that the SoapySDR driver published, and neither retunes the RFIC nor disconnects the
application.

Start GQRX on the Ethernet board with the device string
`driver=LiteXM2SDR,eth_ip=192.168.1.50`, tune it and start the DSP. Then record and replay.

```sh
m2sdr_sata -i 192.168.1.50 capture-current my_capture --seconds 10
m2sdr_sata -i 192.168.1.50 serve my_capture
```

During `serve`, GQRX stays open and displays the samples read from the SSD. When the replay
ends, the live stream returns. The utility and GQRX share the Etherbone control of the board:
the board answers each of them on its own UDP port.

> Install the SoapySDR module built from the same tree as the bitstream, with the `/usr` prefix
> (`-DCMAKE_INSTALL_PREFIX=/usr`). A module installed under `/usr/local` can be hidden by an
> older one under `/usr`.

### Export to the host {#flow-sata-export}

```sh
m2sdr_sata -i 192.168.1.50 export fm_test /tmp/fm_test.sigmf-meta
m2sdr_sata -i 192.168.1.50 export fm_test /tmp/fm_test.sc16 --raw
```

The first command writes a SigMF recording, metadata and data; the second writes the samples
only. The exported recording opens in `m2sdr_check` and plays with `m2sdr_play`.

### Import and replay {#flow-sata-replay}

A raw file needs its metadata on the command line; a SigMF recording brings its own. The input
can be the metadata file, the data file or the base name.

```sh
m2sdr_sata -i 192.168.1.50 import tx_test /tmp/tx.sc16 \
    --sample-rate 4M --format sc16 --channel-layout 1t1r --tx-freq 2400M --tx-att 20
m2sdr_sata -i 192.168.1.50 import tx_sigmf /tmp/tx.sigmf-meta
```

`play` replays a capture to the antenna, with the RF parameters stored in its metadata. `serve`
replays it into the RX path of the host. It selects Ethernet or PCIe from the device option;
`--dst pcie` or `--dst eth` overrides the choice.

```sh
m2sdr_sata -i 192.168.1.50 play tx_sigmf
m2sdr_sata -i 192.168.1.50 serve fm_test
m2sdr_sata -c 0 serve fm_test
```

### Write rate {#flow-sata-rate}

A capture is in real time only when the drive sustains the rate of the stream.

```text
bytes per second = sample rate x bytes per sample x channel count
```

A sample takes 4 bytes in SC16 and 2 bytes in SC8. A 1T1R SC16 stream at 4 MSPS needs 16 MB/s
(15.26 MiB/s);
a 2T2R SC16 stream at 30.72 MSPS needs 245.76 MB/s.

The sustained write rate is a property of the drive and of its state. A short write can be much
faster than a long one, because the cache of the drive absorbs it. Measured drive rates are in
[Performance](#performance).

When the drive is too slow, the RX stream is held back and the capture takes longer than
requested: the utility reports the difference. Treat such a capture as incomplete in time and
lower the sample rate, the sample size or the channel count.

### Stop and recover {#flow-sata-stop}

```sh
m2sdr_sata -i 192.168.1.50 stop both
```

Ctrl-C during `capture`, `play` or `serve` stops the transfer at the end of the SATA command in
progress, and the utility exits with the SATA link usable. A capture stopped this way is not
added to the volume. `stop` does the same to a busy streamer before resetting it. On a
bitstream without the stop registers of the streamers, Ctrl-C lets the whole programmed transfer
finish.

A second Ctrl-C abandons the transfer. After that, or after a timeout, run `info`. If it
reports `Drive: Not present` while the SATA link is up, reload the bitstream before the next
SATA command.

### Diagnostics {#flow-sata-diag}

The `diag` commands work on raw sectors, below the volume. They check the drive and the host
access without the RFIC.

```sh
m2sdr_sata -i 192.168.1.50 diag etherbone-bench
m2sdr_sata -c 0 diag pcie-dma-bench 0x200000 32768
m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-write 0x260000 8192
m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-check 0x260000 8192
m2sdr_sata -i 192.168.1.50 diag read 0x260000 8192 /tmp/raw.bin
m2sdr_sata -i 192.168.1.50 diag write /tmp/raw.bin 0x280000
```

The arguments are a first sector and a number of sectors. The index of the volume is at sector
`0x800`, and the automatic allocation of the captures starts at sector `0x100000`. Keep raw
writes away from the sectors of the captures. The script
`scripts/m2sdr_sata_transport_check.py` runs a sequence of these checks over the selected host
link.

If `diag pcie-dma-bench` reports `unsupported`, the loaded kernel driver lacks the SATA DMA
request: the driver does not match the software, and the result says nothing about SATA.

## Wide bandwidth {#flow-wide-bandwidth}

> Experimental ([Status](#intro-status)). Sample rates above 61.44 MSPS run the RFIC outside its
> specification. Use this recipe for evaluation, not as the normal operating mode.

The RFIC is specified for a bandwidth of 56 MHz and a sample rate of 61.44 MSPS. This recipe
runs it at 122.88 MSPS with a wider analog passband. How this is done, and what it costs, is
explained in [Oversampling mode](#rfic-oversampling); the measured results are in
[Performance](#performance).

| Mode | Bitstream | Host link |
|-------------------|---------------------------------------------------------|-------------------|
| 1T1R at 122.88 MSPS | Any PCIe bitstream | PCIe |
| 2T2R at 122.88 MSPS | Built with `--with-rfic-oversampling` | PCIe Gen2 x2 or x4 |

### 1T1R {#flow-wide-1t1r}

A sample rate above 61.44 MSPS selects the wide bandwidth configuration of the RFIC by itself.

```sh
m2sdr_rf --channel-layout 1t1r --sample-rate 122.88e6 --rx-freq 3.6e9 --rx-gain 30
```

The configuration takes a little longer than usual: the software checks the data port with a
PRBS and programs the clocks again until the port is aligned. Then record or stream as usual.
Over a PCIe x1 link, use the SC8 format to halve the rate of the stream.

```sh
m2sdr_rf --channel-layout 1t1r --sample-rate 122.88e6 --format sc8 --rx-freq 3.6e9
m2sdr_record --format sc8 rx_file.bin 500000000
```

### 2T2R full duplex {#flow-wide-2t2r}

Full duplex 2T2R at 122.88 MSPS needs the oversampling build.

1. Build the bitstream.

   ```sh
   ./litex_m2sdr.py --variant=m2 --with-pcie --pcie-gen=2 --pcie-lanes=4 \
       --with-rfic-oversampling --build
   ```

2. Flash it and reload the FPGA; see [Building the gateware](#building-gateware).
3. Rebuild the software against the register map of this build; see
   [Versions and compatibility](#sw-versioning).
4. Check that the bitstream is the oversampling one.

   ```sh
   m2sdr_util info
   ```

   The line `Oversampling` of the feature list must start with `Yes`.
5. Configure the RFIC.

   ```sh
   m2sdr_rf --channel-layout 2t2r --sample-rate 122.88e6 \
       --rx-freq 3.6e9 --tx-freq 3.6e9 --rx-gain 30 --tx-att 20
   ```

The configuration checks the framing of the TX data port and starts again when the RFIC locked
on the wrong phase, without any action from the application. An application only asks for
122.88 MSPS with the 2T2R layout.

Three limits apply to the oversampling build.

- It is for PCIe alone. The script accepts the option with Ethernet or SATA, but these paths
  lose half of each word: do not combine them. The BFP8 format does not exist on this build.

- TX works only in 2T2R at 122.88 MSPS. RX works at all rates. Use a standard bitstream to
  transmit at lower rates.
- On a standard bitstream, the software refuses 2T2R above 61.44 MSPS with an error instead of
  misconfiguring the RFIC.

### Tuning {#flow-wide-tuning}

The environment variables below are tuning and diagnostic controls of the wide bandwidth
configuration.
The defaults are the values the mode was measured with.

| Variable | Controls |
|---------------------------------------------|--------------------------------------------------|
| `M2SDR_OC_BBF_TUNE` | Corner target of the baseband filters |
| `M2SDR_OC_BBF_FORCE` | Older method that forces the filter registers |
| `M2SDR_OC_RX_FIR_FILE` | RX equaliser taps, 16 at most |
| `M2SDR_OC_TX_FIR_FILE` | TX equaliser taps, 16 at most, designed for 245.76 MHz |
| `M2SDR_QEC_KEXP` | Gain of the RX quadrature tracking loop |
| `M2SDR_RFPLL_CP_PERCENT` | Charge pump scale of the RX RF PLL |
| `M2SDR_OC_RX_DELAY` | RX delay of the data port |
| `M2SDR_OC_TX_DELAY` | TX delay of the data port |

With SoapySDR, the device argument `ad9361_fir_profile` selects the FIR profile of this mode;
see [Device arguments](#fw-args). The best signal quality is obtained on the internal
reference.

## Timed TX {#flow-timed-tx}

With timed TX, a buffer is transmitted when the board time reaches its timestamp. The gateware
holds the buffer in a gate until then, so the timestamp is the time on air, on a 10 ns step.
The gate is described in [Streaming](#streaming).

The gate is part of every bitstream, but timed TX and the TX header work over PCIe only. A
program checks the presence of the gate with the feature bit `M2SDR_FEATURE_TIMED_TX` of
`m2sdr_get_capabilities()`; `m2sdr_util info` does not print it.

Three rules follow from the mechanism.

- Only the first buffer of a burst needs a timestamp. The rest of the burst follows it without
  a gap.
- A buffer without a timestamp is transmitted at once. Timed and untimed buffers can be mixed.
- A buffer that reaches the gate after its time is dropped whole. The RFIC transmits zeros
  for its duration and the TX underflow counter increases. Write far enough ahead of time.

### Calibrate the offset {#flow-timed-cal}

Between the gate and the antenna, the samples cross a fixed pipeline. The TX offset compensates
it, so that a buffer stamped for time T is on air at T. The offset depends on the sample rate
and on the channel layout. It is about a microsecond at 30.72 MSPS and smaller at higher rates.

Measure it once for each configuration, with a cable and attenuators from a TX port to the RX
port of the same channel. The test uses channel 2, the 2T2R layout, 122.88 MSPS and a lead of
3000 µs by default, so `--rate` is required for another rate.

```sh
make -C litex_m2sdr/software/user
make -C scripts
sudo ./scripts/timed_tx_selftest --chan 2 --rate 30.72e6 --lead-us 8000
```

With attenuators in the cable, raise the RX gain and lower the TX attenuation with `--rx-gain`
and `--tx-att` until the test finds its markers.

The test transmits marker buffers at chosen times, finds them in the RX stream and prints the
latency, then a line starting with `CALIBRATION` that gives the TX offset to use. It also sends
a buffer stamped in the past and checks that it is dropped. Run it again with the value to
check the result.

```sh
sudo ./scripts/timed_tx_selftest --chan 2 --rate 30.72e6 --lead-us 8000 --set-offset 1212
```

The value 1212 is an example: use the one the first run printed. The test runs in the 2T2R
layout by default; add `--layout 1t1r` to calibrate the 1T1R layout. The loopback measures the sum of the TX and RX pipelines, so this
calibration aligns the TX times with the RX timestamps. This is what an application that
answers a received signal needs.

### With the library {#flow-timed-lib}

```c
m2sdr_set_tx_header(dev, true);
m2sdr_set_tx_offset(dev, 1212);

struct m2sdr_metadata meta = {
    .timestamp = air_time_ns,
    .flags     = M2SDR_META_FLAG_HAS_TIME,
};
m2sdr_sync_tx(dev, buf, n, &meta, timeout_ms);

uint32_t underflows;
m2sdr_get_tx_underflow(dev, &underflows);
```

Enabling the TX header is required: without it, a timed write returns `M2SDR_ERR_STATE`. The
underflow counter gives the number of buffers that missed their time.

RX and TX timestamps share the board time. "Received at T, transmit at T plus D" is therefore
plain arithmetic on one clock. `m2sdr_set_rx_offset()` is the mirror of the TX offset for the
RX timestamps. It is needed only when the timestamps must refer to the antenna in absolute
time, for instance to align several boards.

### With SoapySDR {#flow-timed-soapy}

No code change is needed in a SoapySDR application that already uses timed writes. Pass the
calibrated offset in the device arguments, or omit it to let the driver derive a value.

```text
driver=LiteXM2SDR,tx_offset=1212
```

The driver uses the gate when the bitstream has it and its software timeline otherwise; see
[Timed TX](#fw-timed-tx) in the SoapySDR driver.

## Low latency {#flow-low-latency}

The defaults favour throughput: large DMA rings that absorb the pauses of the host. A real-time
loop that transmits in answer to what it receives wants the opposite: a short path, even if the
host must then never stall. Three settings shorten the path. All are optional and leave the
defaults unchanged for other uses.

### Shallow DMA ring {#flow-ll-ring}

The TX latency floor is the time to drain the ring.

```text
drain time = dma_buffer_count x 8192 / (sample rate x bytes per sample)
```

At 30.72 MSPS in 2T2R, where a sample of both channels takes 8 bytes, 256 buffers hold about
8.5 ms and 8 buffers about 0.27 ms. In 1T1R, with 4 bytes, the times double. Load the driver
with a short ring, then pin its interrupt to the core of the radio thread.

```sh
sudo rmmod m2sdr
sudo insmod litex_m2sdr/software/kernel/m2sdr.ko dma_buffer_count=8 dma_buffer_per_irq=2
sudo scripts/pin_m2sdr_irq.sh 2
```

The argument of the script is the CPU core, 2 by default. The interrupt affinity is lost each
time the module is loaded, so run the script again after each load. The parameters are
described in [Kernel driver](#sw-kernel).

### Fast RX wake-up {#flow-ll-rx}

By default the library waits for RX buffers with `poll()`. On a CPU with the MONITORX and
MWAITX instructions, found on AMD processors, it can instead sleep on the memory of the next
buffer and wake as soon as the board writes it.

```sh
M2SDR_RX_WAIT=mwaitx ./my_radio_app
```

On a CPU without these instructions the variable has no effect.

### TX fill lead {#flow-ll-tx}

With the zero-copy API, the library fills the whole TX ring by default. A lead keeps the host
only a few buffers ahead of the board, so the TX latency becomes about the lead times the
duration of a buffer.

```c
m2sdr_set_tx_lead_buffers(dev, 3);
```

The lead must be greater than `dma_buffer_per_irq`: 3 or more with the short ring above. With
the default of 8 buffers per interrupt a small lead is not usable; keep 0, which fills the
ring.

### Host setup {#flow-ll-host}

A short ring absorbs less host jitter. Run the radio thread with a real-time priority
(`SCHED_FIFO`), with its memory locked (`mlockall`), on a core isolated from the scheduler
(`isolcpus=`), and keep the DMA interrupt on that same core. Under load, check that the stream statistics report 0 overflow and 0 underflow. The
mechanism is described in [Streaming](#streaming).

## Several boards in one host {#flow-multi-board}

Several boards can work in one host: for instance four boards on a bifurcated M.2 carrier card
give eight RX and eight TX channels, each board streaming over its own PCIe link. The carrier
cards are described in [Carriers and hosts](#carriers).

### Select a board {#flow-multi-select}

Each PCIe board gets a device node: `/dev/m2sdr0`, `/dev/m2sdr1` and so on, with the default
single DMA channel. A build with several DMA channels gets one node per channel. List them, then
address each one.

```sh
m2sdr_util list
m2sdr_util -c 0 info
m2sdr_util -c 1 info
```

The second board is selected in the same way in every layer of the software.

- Utilities: `-c 1`.
- Library: `pcie:/dev/m2sdr1`.
- SoapySDR: `driver=LiteXM2SDR,path=/dev/m2sdr1`.

To tell the boards apart, read the line `FPGA DNA` that `m2sdr_util info` prints: this
identifier is unique to each FPGA.

Each board is configured and streams on its own: run one `m2sdr_rf` per board, then one
streaming process per board.

```sh
m2sdr_rf -c 0 --sample-rate 30720000 --rx-freq 2400000000
m2sdr_rf -c 1 --sample-rate 30720000 --rx-freq 2400000000
m2sdr_record -c 0 rx0.bin 200000000 &
m2sdr_record -c 1 rx1.bin 200000000 &
wait
```

### Ethernet boards {#flow-multi-eth}

Ethernet boards on the same network need different addresses. The address is a build option, so
each board gets its own bitstream.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-local-ip=192.168.1.51 --build
```

```sh
m2sdr_util -i 192.168.1.50 info
m2sdr_util -i 192.168.1.51 info
SoapySDRUtil --find="driver=LiteXM2SDR,eth_ips=192.168.1.50;192.168.1.51"
```

### Common time and frequency {#flow-multi-sync}

Boards that run from their own oscillator drift apart in frequency and in time. For coherent
reception or transmission on several boards, give them a common reference with one of the
methods of [Time synchronisation](#flow-time-sync), and compare the samples by their
timestamps.

- Common frequency: feed the same 10 MHz reference to every board and select it with
  `--sync external`.
- Common time over PCIe: each board has its own PTP hardware clock. Run one instance of the
  time script per board, with `--phc /dev/ptpN`.
- Common time over Ethernet: every board follows the same PTP master.

The reference inputs of the board are described in [Time synchronisation](#timing). The PPS
input is only monitored: it does not set the board time, and the board has no PPS output.

## Time synchronisation {#flow-time-sync}

This section gives the commands of each synchronisation method. The mechanisms, the accuracy to
expect and the tuning of the loops are described in [Time synchronisation](#timing).

| Goal | Method | Needs |
|---------------------|----------------|----------------------------------------------------------|
| Frequency from a lab reference | External 10 MHz | SI5351C version of the board |
| Board time set once | Host write | nothing specific |
| Board time follows the host | PCIe PTM | `--with-pcie-ptm`, PCIe Gen2 x1 |
| Board time follows the network | Ethernet PTP | `--with-eth-ptp` |
| Frequency from the network | PTP RFIC reference | `--with-eth-ptp-rfic-clock` |
| Time from White Rabbit | White Rabbit | `--with-white-rabbit`; Experimental |

### External 10 MHz reference {#flow-sync-10mhz}

Connect the 10 MHz reference to the reference input of the board, then select it when the RFIC
is configured.

```sh
m2sdr_rf --sync external --refclk-freq 38400000
m2sdr_util clk-test
```

`clk-test` measures the clocks of the board. With SoapySDR, use `clock_source=external` and
read the sensor `ref_locked`.

```sh
SoapySDRUtil --probe="driver=LiteXM2SDR,clock_source=external"
```

Without an external reference, the error of the internal oscillator can be measured against a
known carrier and removed with `--refclk-ppm`; see
[m2sdr_rf: RFIC configuration](#sw-rf).

### Set the board time from the host {#flow-sync-set}

The script below sets the board time to the time of the host, once, then reads it back for a
few seconds. The board time then runs from the reference clock of the board.

```sh
python3 litex_m2sdr/software/soapysdr/test_time.py --set now --interval 0.2 --duration 5
```

The write is refused when Ethernet PTP disciplines the board time.

### PCIe PTM {#flow-sync-ptm}

With PTM, the board time follows the clock of the host over PCIe. If the host clock is itself
synchronised, by NTP or PTP, the board follows that time.

```sh
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=1 --with-pcie-ptm --build --load
```

The kernel driver exposes the board time as a PTP hardware clock. The script finds this clock
by its name, `m2sdr`, and runs `phc2sys` of linuxptp with the host clock as the source and the
board as the sink. Print the command first, then run it.

```sh
scripts/m2sdr_pcie_time_sync.py --dry-run
sudo scripts/m2sdr_pcie_time_sync.py --stdout
```

With `--stdout`, `phc2sys` prints the offset between the two clocks at each update: it must
settle to a small, stable value. With several boards, select the clock of each one.

```sh
sudo scripts/m2sdr_pcie_time_sync.py --phc /dev/ptp1 --stdout
```

To start the synchronisation at boot, install a systemd service that runs the script.

```ini
[Unit]
Description=Synchronize the LiteX-M2SDR board time to host time
After=multi-user.target

[Service]
ExecStart=/path/to/litex_m2sdr/scripts/m2sdr_pcie_time_sync.py --stdout
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

### Ethernet PTP {#flow-sync-ptp}

With Ethernet PTP, the board is a PTP slave on its Ethernet link and disciplines its board time
to the master. The recipe below uses the host as the master, on a direct cable.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-ptp --eth-sfp=0 --build --load
```

Check what the network interface of the host supports. In the commands below, replace `eth0`
by the name of the interface connected to the board.

```sh
ethtool -T eth0
```

When the output lists a PTP hardware clock and hardware timestamping in both directions, use
the hardware configuration. When it lists only software timestamping, use the software one. The
board locks in both cases; with software timestamps the host dominates the jitter.

```sh
sudo ptp4l -i eth0 -f doc/ptp/ptp4l-m2sdr-hardware.cfg -m
sudo ptp4l -i eth0 -f doc/ptp/ptp4l-m2sdr-software.cfg -m
```

Then look at the board.

```sh
m2sdr_util -i 192.168.1.50 info
m2sdr_util -i 192.168.1.50 --watch ptp-status
```

`ptp-status` shows whether the PTP core is locked, whether the board time is locked to it or in
holdover, the identity of the master and the lock and loss counters. `ptp-config` shows and
changes the parameters of the discipline.

A script checks the lock and the PTP messages on the wire. `smoke` is a short test; `soak`
samples the status for a long time and writes a report.

```sh
scripts/m2sdr_ptp_check.py smoke --ip 192.168.1.50 --iface eth0 --duration 10
scripts/m2sdr_ptp_check.py soak --ip 192.168.1.50 --duration 1800 --interval 5 \
    --max-error-ns 1000000 --json-out build/ptp/soak.json --csv-out build/ptp/soak.csv
```

Both end with a pass or fail result. Keep the default error limit with software timestamps, and
lower it with hardware timestamps once the link has been measured.

### PTP-referenced RFIC clock {#flow-sync-ptp-clk}

On the SI5351C version of the board, a second loop can discipline a 10 MHz clock of the FPGA to
PTP and feed it to the clock generator. The sample clock and the frequencies of the RFIC then
follow the network. The loop is off after the bitstream is loaded.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-ptp \
    --with-eth-ptp-rfic-clock --eth-sfp=0 --build --load
```

Wait for a stable PTP lock, then enable the loop and watch it until it reports a lock.

```sh
m2sdr_util -i 192.168.1.50 ptp-clock10-status
m2sdr_util -i 192.168.1.50 ptp-clock10-config enable on
m2sdr_util -i 192.168.1.50 --watch ptp-clock10-status
```

If the loop was started before the PTP lock, or after a change of reference, ask for a new
alignment. It is applied on the next edge of the PTP reference.

```sh
m2sdr_util -i 192.168.1.50 ptp-clock10-config align
```

Then select the FPGA clock as the reference when the RFIC is configured.

```sh
m2sdr_rf -i 192.168.1.50 --sync fpga --refclk-freq 38400000
SoapySDRUtil --probe="driver=LiteXM2SDR,eth_ip=192.168.1.50,clock_source=fpga"
```

The check script covers this loop with two more options.

```sh
scripts/m2sdr_ptp_check.py smoke --ip 192.168.1.50 --iface eth0 --duration 10 \
    --with-clock10 --require-clock10-lock
```

If the phase error grows after the loop is enabled, flip the sign of the correction with the
field `invert` of `ptp-clock10-config`. The fields of the loop are described in
[Time synchronisation](#timing).

### White Rabbit {#flow-sync-wr}

> Experimental ([Status](#intro-status)). The lock, the jitter and the PPS accuracy of the
> White Rabbit build are not measured.

White Rabbit needs the baseboard, an SFP port and the LiteX-WR-NIC repository next to this one.
The build, the console of the White Rabbit core and its limits are described in
[Time synchronisation](#timing). Do not enable Ethernet in the same bitstream: the script
accepts the pair, but the two cores would share one PLL channel and its reset.

The first command below checks that the dependency is found; the second builds a bitstream with
PCIe and White Rabbit.

```sh
./litex_m2sdr.py --variant=baseboard --wr-status
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit --build
```

## Where to go next {#flow-next}

[Debugging](#debugging) when a recipe does not give the expected result;
[Performance](#performance) for measured rates; [Host software](#software) and
[SDR frameworks](#frameworks) for the options of the utilities and of the SoapySDR driver.
