# Performance and resources {#performance}

This appendix gathers the measured results of the project. They were measured by the
maintainers, and each one is given with the conditions of its measurement; nothing here was
measured for the manual. The status of each feature is in the [status table](#intro-status).

## Summary {#perf-summary}

The values are given in the units of their measurement.

| Subject | Figure | Detailed in |
|----------------------|-------------------------|-----------------------------------------------------|
| Sample rate | 2T2R up to 61.44 MSPS, 12 bits | [The RFIC interface](#rfic) |
| Timed TX offset | About a microsecond, per rate and channel layout | [Streaming](#streaming-timed-tx-plane) |
| TX latency floor | Drain time of the DMA ring | [Streaming](#streaming-latency-source) |
| RF to SATA recording | 232.94 MiB/s sustained, in real time | [SATA storage](#perf-sata) |
| USB3 sample stream | 368.6 MB/s in full duplex, 440 MB/s for RX alone | [USB3 host link](#perf-usb3) |
| Ethernet sample stream, 1000BASE-X | 116 MB/s in full duplex | [Ethernet host link](#perf-eth) |

The sample rate is the statement of `README.md`, and the TX offset is recorded in
`litex_m2sdr/software/soapysdr/README.md`. The latency floor and the SATA figures were measured
by the maintainers; the SATA runs are dated in [SATA storage](#perf-sata).

One lane of PCIe Gen2 is enough for the standard 61.44 MSPS 2T2R rate.

> The margin is small: 2T2R at 61.44 MSPS is 491.5 MB/s of samples in SC16, against 500 MB/s of
> raw rate for one Gen2 lane.

### Oversampling mode {#perf-oversampling}

Experimental ([Status](#intro-status)). Measured by the maintainers on one board; the conditions
are in [Measured performance](#rfic-oversampling-performance).

| Subject | Figure | Detailed in |
|----------------------|-------------------------|-----------------------------------------------------|
| Sample rate | 2T2R at 122.88 MSPS | [Oversampling mode](#rfic-oversampling) |
| Analog bandwidth | About 100 MHz usable | [Oversampling mode](#rfic-oversampling) |
| PCIe bus load | 7.9 Gbps for 2T2R at 122.88 MSPS | [Host links](#host-links) |

The oversampling mode needs two or four PCIe lanes. The bus load is the statement of
`README.md`.

## USB3 host link {#perf-usb3}

Measured by the maintainers, 2026-11-16, on a board in the LiteX Acorn Baseboard Mini with a
Xillybus SFP2USB adapter in the SFP 0 cage, a USB3-only bitstream and a PC with Linux as host.
[Sample rates per host link](#links-rates) compares the result with the other links.

### Method {#perf-usb3-method}

The RFIC runs at the real sample rate with its digital loopback: what the host transmits comes
back on the RX stream. The host sends a ramp over the sample values and checks every received
value, so a sample lost or repeated in either direction is counted. The received samples are
also counted against the board time of the RX frames. The error counters of the USB3 link are
read after each run, through JTAG.

A run passes with no ramp error, no sample missing against the board time and no stream error.
The host sends 16 buffers per call (128 KiB). The RX-only runs have no ramp: they are checked
against the board time alone.

### Full duplex {#perf-usb3-fdx}

Each run lasts 20 s. The stream rate is per direction.

| Channels | Format | Sample rate | Stream | Result |
|----------|--------|--------------------|-------------------|------------------------------|
| 2T2R | SC16 | 10, 20 MSPS | 80, 160 MB/s | Pass |
| 2T2R | SC16 | 30.72 MSPS | 245.8 MB/s | Pass |
| 2T2R | SC16 | 35, 40, 43 MSPS | 280 to 344 MB/s | Pass |
| 2T2R | SC16 | 46.08 MSPS | 368.6 MB/s | Pass |
| 2T2R | SC16 | 50 MSPS | 400 MB/s | Fail: 366 MB/s received, 379 MB/s sent |
| 2T2R | SC8 | 30.72, 46.08, 61.44 MSPS | 122.9 to 245.8 MB/s | Pass |
| 1T1R | SC16 | 30.72, 46.08, 61.44 MSPS | 122.9 to 245.8 MB/s | Pass |
| 1T1R | SC8 | 61.44 MSPS | 122.9 MB/s | Pass |

Three runs of 120 s at 245.8 MB/s also pass, with 29.5 GB checked in each direction per run:
2T2R at 30.72 MSPS in SC16, 2T2R at 61.44 MSPS in SC8 and 1T1R at 61.44 MSPS in SC16.

### 1T1R above 61.44 MSPS {#perf-usb3-oversampling}

The rates of the [oversampling mode](#rfic-oversampling), in 1T1R on the standard USB3
bitstream, 20 s per run in full duplex unless noted.

| Format | Sample rate | Stream | Result |
|--------|--------------------|-------------------|------------------------------------------|
| SC8 | 76.8, 92.16 MSPS | 153.6, 184.3 MB/s | Pass |
| SC8 | 122.88 MSPS | 245.8 MB/s | Pass, also over 120 s |
| SC16 | 64, 76.8, 92.16 MSPS | 256 to 368.6 MB/s | Pass |
| SC16 | 107.52 MSPS, RX alone | 430.1 MB/s | Pass |
| SC16 | 122.88 MSPS, RX alone | 491.5 MB/s | Fail: 453 MB/s received |

The full-duplex runs are checked on every sample, on the 12 bits in SC16. They use the TX delay
of the library for this mode, see [Principle](#rfic-oversampling-principle); with the earlier
delay the lowest bit of each TX sample was read as 1.

### RX alone {#perf-usb3-rx}

| Channels | Format | Sample rate | Stream | Result |
|----------|--------|--------------------|-------------------|------------------------------|
| 2T2R | SC16 | 30.72, 40, 46.08 MSPS | 245.8 to 368.6 MB/s | Pass |
| 2T2R | SC16 | 50, 52, 55 MSPS | 400 to 440 MB/s | Pass |
| 2T2R | SC16 | 61.44 MSPS | 491.5 MB/s | Fail: 453 MB/s received |

### Notes on the runs {#perf-usb3-notes}

- The link counted no error and no recovery during the runs of the tables. The FPGA was between
  66 and 73 °C.
- The limit in full duplex, about 370 MB/s per direction, is lower than for RX alone.
- The size of the writes changes the TX rate that a program reaches, see
  [Writing samples](#sw-lib-tx-size). With one 8 KiB buffer per call and a reader running, the
  library reached 279 MB/s, little above the 245.8 MB/s of these configurations, and the stream
  underflowed about 200 times per run of 30 s, mostly in its first seconds. The library no
  longer processes the USB events at each buffer, and one buffer per call reaches 308 MB/s:
  8 runs of 30 s at 245.8 MB/s gave 2 underflow events, in one run where the RX stream
  overflowed too.
- The figures are those of one board, one adapter and one host.

### Loss counters {#perf-usb3-counters}

The gateware counts the samples lost at the RFIC ([Stream loss counters](#rfic-loss)), and the
stream statistics of the library report them. Measured on the same setup:

| Run | RX overflow | TX underflow |
|-----------------------------------------------------|---------------------------|--------------|
| 2T2R at 30.72 MSPS in SC16, full duplex | 0 events | 0 events |
| Reader stopped 5 times for 60 ms | 5 events, 6425 buffers | 0 events |
| Writer stopped 5 times for 30 ms | 0 events | 5 events |
| 2T2R at 61.44 MSPS in SC16, RX alone, above the link | 62 million words lost in 14 s | |

The word count of the last run matches the samples missing against the board time. Stopping
the TX stream is not counted as an underflow.

### Timed TX {#perf-usb3-timed-tx}

Measured by the maintainers, 2026-11-16, on the same setup, with a bitstream that carries the
TX header over USB3 and a TX offset of 0.

With the digital loopback of the RFIC, a timed burst comes back on the RX stream at a constant
delay after its transmit time: the spread over 5 to 10 bursts is below 1.5 sample periods at 5,
10, 30.72 and 46.08 MSPS in 2T2R, for bursts of 1 to 64 buffers. A burst sent with a transmit
time in the past loses its first buffer, and `header_tx_underflow` counts it.

The delay to the antenna was measured over the air, through the coupling between the TX and RX
paths of the board: a burst of tone, located in the RX samples by correlation.

| Configuration | Sample rate | Delay, mean | Standard deviation | In sample periods |
|---------------|-------------|-------------|--------------------|-------------------|
| 2T2R | 10 MSPS | 3684 ns | 60 ns | 36.8 |
| 2T2R | 20 MSPS | 1898 ns | 37 ns | 38.0 |
| 2T2R | 30.72 MSPS | 1287 ns | 17 ns | 39.5 |
| 2T2R | 46.08 MSPS | 840 ns | 20 ns | 38.7 |

These delays are close to those of PCIe ([One reference plane](#streaming-timed-tx-plane)), which
measured 1215 ns at 30.72 MSPS over a cable. The SoapySDR driver uses the same TX offset on
both links. The method locates the edge of a burst to about one sample period.

## Ethernet host link {#perf-eth}

Measured by the maintainers, 2026-11-16, on a board in the LiteX Acorn Baseboard Mini with a
1000BASE-X SFP module in the SFP 0 cage, an Ethernet-only bitstream and a PC with Linux on the
same network. The method is the one of the [USB3 host link](#perf-usb3-method): digital
loopback of the RFIC, ramp checked on every sample, samples counted against the board time of
the RX frames, loss counters of the gateware read after each run.

Two settings of the host matter:

- The socket buffers. `net.core.rmem_max` and `net.core.wmem_max` were raised to 8 MiB. With the
  default of 208 KiB the kernel dropped datagrams at each short stall of the reader.
- The pacing of the writer. UDP has no flow control, and the TX FIFO of the gateware holds
  2.5 ms of samples at 100 MB/s ([Sample streams](#links-eth-streams)). The runs pace the
  writer on the clock of the host with a busy wait. With a sleep between the writes, the
  wake-up was now and then late enough to empty the FIFO: one TX underflow per 15 to 30 s at
  100 MB/s.

### Full duplex {#perf-eth-fdx}

Each run lasts 20 s. The stream rate is per direction.

| Channels | Format | Sample rate | Stream | Result |
|------------|--------|--------------------------|-------------------|------------------------------|
| 2T2R | SC16 | 5, 10, 12.5, 13.5, 14 MSPS | 40 to 112 MB/s | Pass |
| 2T2R | SC16 | 14.5 MSPS | 116 MB/s | Pass |
| 2T2R | SC8 | 15.36, 20, 25, 27, 28 MSPS | 61.4 to 112 MB/s | Pass |
| 2T2R | SC8 | 29 MSPS | 116 MB/s | Pass |
| 1T1R | SC16 | 15.36, 25, 27, 28, 29 MSPS | 61.4 to 116 MB/s | Pass |
| 1T1R | SC8 | 30.72, 50, 56, 58 MSPS | 61.4 to 116 MB/s | Pass |
| 1T1R | SC8 | 61.44 MSPS | 122.9 MB/s | Fail: above the link |

Three runs of 120 s at 100 MB/s: 2T2R at 12.5 MSPS in SC16 and 2T2R at 25 MSPS in SC8 pass;
1T1R at 25 MSPS in SC16 had one TX underflow, a stall of the host.

### RX alone {#perf-eth-rx}

| Channels | Format | Sample rate | Stream | Result |
|------------|--------|--------------------|-------------------|------------------------------|
| 2T2R | SC16 | 14, 14.5 MSPS | 112, 116 MB/s | Pass |
| 2T2R | SC16 | 15 MSPS | 120 MB/s | Fail: above the link |
| 2T2R | SC8 | 28, 28.5, 29 MSPS | 112 to 116 MB/s | Pass |

When the link is saturated the samples are lost in the FPGA, and the loss counters report
it: 8 million words in 20 s at 15 MSPS, with no frame broken on the host side.

### Timed TX {#perf-eth-timed-tx}

With the digital loopback of the RFIC and a TX offset of 0, a timed burst comes back on the RX
stream at a constant delay after its transmit time.

| Sample rate (2T2R) | Buffers per burst | Delay | Spread over the bursts |
|--------------------|-------------------|--------------------|--------------------------|
| 1 MSPS | 2 | 7.9 to 8.6 µs | 0.7 sample period |
| 2 MSPS | 4 | 4.1 to 4.5 µs | 0.8 sample period |
| 5 MSPS | 1, 4 | 1.9 to 2.0 µs | 0.6 to 0.8 sample period |
| 10 MSPS | 1, 4, 32 | 1.1 to 1.3 µs | 0.6 to 1.1 sample period |
| 12.5 MSPS | 8, 16 | 1.0 to 1.1 µs | 0.5 to 0.8 sample period |

Every burst is received at full length. A burst sent with a transmit time in the past loses its
first buffer, and `header_tx_underflow` counts it. The delay to the antenna was not measured on
this link.

### Notes on the runs {#perf-eth-notes}

- The network card of the host counted no error and no drop. The FPGA was between 66 and 73 °C.
- `m2sdr_util ad9361-loopback-test` passes at 5 and 10 MSPS (80 MB/s) on this host and fails
  above: its reader, in one thread with the check, does not follow 100 MB/s of datagrams, and
  its writer is paced on that reader.
- The figures are those of one board, one module and one host.

## SATA storage {#perf-sata}

This section is the record of the SATA validation on the LiteX Acorn Baseboard Mini with an SSD
connected, measured by the maintainers on the dates given with each run. The dated note
`doc/notes/sata-bandwidth-investigation.md` completes it. The speeds are wall-clock measurements
of the host utility, unless a command reports its own throughput. The unit is MiB/s.

Three paths were measured. The RF path records the RX stream straight to the sectors of the
SSD. The two host paths copy between the host and the SSD: over PCIe by DMA to the host memory,
over Ethernet through a staging buffer of the FPGA that Etherbone reads and writes.

![SATA measurement paths: the RX stream to the SSD, and the host to the SSD over PCIe DMA or over Etherbone and the staging buffer.](perf_sata_paths.svg)

The gateware blocks are named after the direction of the data. [Host links](#host-links)
describes them and [Workflows](#workflows) gives the commands of `m2sdr_sata`.

| Name | Direction |
|------------------------------------|----------------------------------------------------------------|
| `Sector2Mem` | SATA sectors to the host memory or the staging buffer |
| `Mem2Sector` | Host memory or staging buffer to SATA sectors |
| `Stream2Sectors` | RX stream to SATA sectors |
| `Sectors2Stream` | SATA sectors to the TX stream or to a replay stream to the host |

The staging buffer is 128 KiB, which gives SATA commands of 256 sectors. Ethernet transfers to
and from it use Etherbone bursts of 128 words by default.

### Required write rate {#perf-sata-required}

A recording is in real time when the SSD sustains the rate of the samples: sample rate, times
the bytes per complex sample, times the number of channels. In 1T1R with SC16, 4 MSPS needs
15.26 MiB/s and 10 MSPS needs 38.15 MiB/s.

The sustained write rate is a property of the SSD and of its state, not of the Ethernet link or
of a FIFO.

Short writes can be much faster than the sustained rate, because the cache of the SSD
absorbs them. When the utility reports an elapsed time that differs from the requested one, the
capture was slowed by backpressure: lower the rate, the sample width or the channel count.

### RF to SATA and host paths, Ethernet build {#perf-sata-rf}

Measured by the maintainers, 2026-07-13: Samsung 850 EVO 250GB, Ethernet and SATA build on the baseboard,
SATA generation 2, write cache enabled. All the captures completed in real time through
`Stream2Sectors`.

| Path | Test | Result | Validation |
|------------|-------------------------------|---------------------|------------------------------------|
| RF to SATA | 10 MSPS SC16 1T1R, 10 s (381.5 MiB) | 38.13 MiB/s, elapsed 10.005 s | Real time, no backpressure |
| RF to SATA | 30.72 MSPS SC16 1T1R, 4 s (468.8 MiB) | 117.15 MiB/s, elapsed 4.001 s | Real time, no backpressure |
| RF to SATA | 30.72 MSPS SC16 2T2R, 15 s (3.43 GiB) | 232.94 MiB/s, elapsed 15.092 s | Real time, past the fast write cache of the SSD |
| Host to SATA | 1 GiB `diag write` | 43.9 MiB/s | Limit of the Etherbone staging path, not of the SSD |
| SATA to host | 64 MiB `diag read`, idle board | 26 to 30 MiB/s | Readback matched with `cmp` |

The same gateware with a worn WDC WDS120G1G0A, an SSD without DRAM that had never been trimmed,
sustained only 10 to 25 MiB/s on long writes. With it, 4 MSPS SC16 was in real time, and
10 MSPS took about twice the requested duration. The dated note
`doc/notes/sata-bandwidth-investigation.md` holds that investigation and the recipe to
benchmark an SSD.

### Host paths, PCIe and Ethernet {#perf-sata-host}

Measured by the maintainers, 2026-05-24.

| Host link | Path | Host to SATA | SATA to host | Validation |
|-------------|-----------------------|------------|------------|----------------------------------------|
| PCIe | SigMF import and export, 8 MiB | 0.112 s, about 71.4 MiB/s | 0.133 s, about 60.2 MiB/s | Data matched with `cmp`, metadata validated |
| PCIe | Raw DMA benchmark, 16 MiB | 93.202 MiB/s | 63.391 MiB/s | `diag pcie-dma-bench` write, read and verify passed |
| Ethernet | SigMF import and export, 8 MiB | 0.167 s, about 47.8 MiB/s | 0.314 s, about 25.5 MiB/s | Data matched with `cmp`, metadata validated |
| Ethernet | SigMF import and export, 64 MiB | 1.289 s, about 49.7 MiB/s | 1.882 s, about 34.0 MiB/s | Data matched with `cmp`, metadata validated |
| Ethernet | Raw `diag write` and `diag read`, 16 MiB | 46.0, 53.5, 53.5 MiB/s | 31.5, 34.0, 34.0 MiB/s | All readbacks matched with `cmp` |

The SigMF rows and the raw rows of Ethernet are not the same measurement. Both use the same
Etherbone transfer and the same sector copy, but `diag write` and `diag read` move only a raw
payload to or from a fixed range of sectors. They are the clearest figure of the payload
throughput.

`import` and `export` measure the whole workflow of a named capture. The wall time includes the
lookup and the update of the volume, the parsing and the formatting of the SigMF metadata, the
metadata sectors and the files of the host. The 8 MiB case also gives more weight to the fixed
overhead of a command.

For the sustained SigMF throughput over Ethernet, use the 64 MiB row. It amortises the fixed
overhead and lands close to the raw figures: about 49.7 MiB/s to the SSD and 34.0 MiB/s to the
host.

### Validated commands {#perf-sata-commands}

PCIe and SATA build:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-sata --load
sudo ./rescan.py
cd litex_m2sdr/software/user
./m2sdr_sata -c 0 info
./m2sdr_sata -c 0 diag pcie-dma-bench 0x200000 32768
./m2sdr_sata -c 0 --pattern counter diag pattern-write 0x240000 8192
./m2sdr_sata -c 0 --pattern counter diag pattern-check 0x240000 8192
```

Ethernet and SATA build:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-sata --load
cd litex_m2sdr/software/user
./m2sdr_sata -i 192.168.1.50 info
./m2sdr_sata -i 192.168.1.50 diag etherbone-bench --iterations 3
./m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-write 0x260000 8192
./m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-check 0x260000 8192
```

`rescan.py` is in `litex_m2sdr/software`. In both cases the SATA PHY, its TX, its RX and its
control were ready. The pattern tests verified 4 MiB of counter data. The SigMF round trips used
`import` and `export`: the exported data matched the source byte for byte and the exported
metadata passed `m2sdr_sigmf --validate`.

### Ethernet optimisation history {#perf-sata-history}

The Ethernet figures above use the 128 KiB staging buffer and the default burst of 128 words.
The earlier measurements are kept to show what each change brought.

| Ethernet path | Size | Host to SATA | SATA to host |
|----------------------------|---------------|-----------------------|----------------------------------|
| Original Etherbone path | 8 MiB SigMF | 0.877 s, about 9.1 MiB/s | 1.099 s, about 7.3 MiB/s |
| Pipelined Etherbone reads | 8 MiB SigMF | 0.882 s, about 9.1 MiB/s | 0.303 to 0.308 s, about 26.0 to 26.4 MiB/s |
| 128 KiB staging buffer | 8 MiB SigMF | 0.167 s, about 47.8 MiB/s | 0.314 s, about 25.5 MiB/s |
| 128 KiB staging buffer | 64 MiB SigMF | 1.289 s, about 49.7 MiB/s | 1.882 s, about 34.0 MiB/s |
| 128 KiB staging buffer | 16 MiB file | 46.0, 53.5, 53.5 MiB/s | 31.5, 34.0, 34.0 MiB/s |

The 128 KiB buffer reduces the number of `Mem2Sector` commands of an import or of a write. A
256 KiB buffer was tried first and rejected: the design rule check of Vivado failed on the
address pins of the cascaded block RAMs.

Benchmark of the Etherbone burst size on the staging buffer:

| Words | Bytes | Write MiB/s | Read MiB/s |
|------------------:|------------------:|--------------------------------:|----------------------------:|
| 1 | 12 | 0.880 | 0.107 |
| 2 | 24 | 1.272 | 0.214 |
| 4 | 48 | 3.270 | 0.428 |
| 8 | 96 | 6.539 | 0.864 |
| 16 | 192 | 14.085 | 1.695 |
| 32 | 384 | 26.158 | 2.977 |
| 64 | 768 | 56.340 | 5.051 |
| 128 | 1536 | 104.632 | 7.710 |

Several records per Etherbone frame were considered and not implemented. The Etherbone
front-end of LiteEth handles one record per frame, without address space or flag support
(`rca`, `bca`, `wca`, `wff`). With a standard Ethernet frame size, records
of 128 words would pack about two per frame, so the expected gain after the read pipelining is
fewer packets and system calls, not a different transfer rate.

Three software variants of the read path were compared on a 16 MiB transfer
(`diag read 0x128000 32768`) over Ethernet:

| Variant | Read throughput |
|--------------------------------------------------------------------|--------------------------------|
| Pipelined Etherbone reads | 29.8, 32.4, 32.6 MiB/s |
| Direct output buffer, larger socket buffers, batched socket calls | 30.6, 32.0, 31.5 MiB/s |
| Direct output buffer and larger socket buffers only | 31.9, 32.4, 31.7 MiB/s |

The exported data matched byte for byte in these runs. The differences were within the
measurement noise, sometimes slower, so the two variants were not kept.

### Notes on the runs {#perf-sata-notes}

- The captures created for the validation were deleted from the volume after each run. The data
  sectors were not erased.
- Reloading between the Ethernet and SATA build and the PCIe and SATA build needs a PCIe rescan
  before `/dev/m2sdr0` can be used again.
- When `diag pcie-dma-bench` reports `unsupported`, the loaded kernel driver does not have the
  SATA DMA request (`LITEPCIE_IOCTL_SATA_DMA`). It is a driver that does not match the software, not a performance result.
- The names of the registers and the meaning of their bits are used by the software: they are
  kept as they are ([Register map](#soc-csr)).

## Resources {#perf-resources}

No utilisation figure is published yet. The public statement is that the base design uses
a fraction of the XC7A200T and leaves room for user processing (`README.md`). The utilisation of
a build is in the Vivado reports of its build directory
([The build flow](#build-flow)).
