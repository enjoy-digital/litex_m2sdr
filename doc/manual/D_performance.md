# Performance and resources {#performance}

This appendix gathers the measured results of the project. They were measured by the
maintainers, and each one is given with the conditions of its measurement; nothing here was
measured for the manual. The status of each feature is in the [status table](#intro-status).

## Summary {#perf-summary}

The table gives the headline figures and where each is detailed. The values are given in the
units of their measurement.

| Subject | Figure | Detailed in |
|----------------------|-------------------------|-----------------------------------------------------|
| Sample rate | 2T2R up to 61.44 MSPS, 12 bits | [The RFIC interface](#rfic) |
| Timed TX offset | About a microsecond, per rate and channel layout | [Streaming](#streaming-timed-tx-plane) |
| TX latency floor | Drain time of the DMA ring | [Streaming](#streaming-latency-source) |
| RF to SATA recording | 232.94 MiB/s sustained, in real time | [SATA storage](#perf-sata) |

The sample rate is the statement of `README.md`, and the TX offset is recorded in
`litex_m2sdr/software/soapysdr/README.md`. The latency floor and the SATA figures were measured
by the maintainers; the SATA runs are dated in [SATA storage](#perf-sata).

One lane of PCIe Gen2 is enough for the standard 61.44 MSPS 2T2R rate.

> The margin is small: 2T2R at 61.44 MSPS is 491.5 MB/s of samples in SC16, against 500 MB/s of
> raw rate for one Gen2 lane.

### Oversampling mode {#perf-oversampling}

Experimental ([Status](#intro-status)). The figures below are those of the oversampling mode,
measured by the maintainers on one board; the conditions are in
[Measured performance](#rfic-oversampling-performance).

| Subject | Figure | Detailed in |
|----------------------|-------------------------|-----------------------------------------------------|
| Sample rate | 2T2R at 122.88 MSPS | [Oversampling mode](#rfic-oversampling) |
| Analog bandwidth | About 100 MHz usable | [Oversampling mode](#rfic-oversampling) |
| PCIe bus load | 7.9 Gbps for 2T2R at 122.88 MSPS | [Host links](#host-links) |

The oversampling mode needs two or four PCIe lanes. The bus load is the statement of
`README.md`.

## SATA storage {#perf-sata}

This section is the record of the SATA validation on the LiteX Acorn Baseboard Mini with an SSD
connected, measured by the maintainers on the dates given with each run. The dated note
`doc/notes/sata-bandwidth-investigation.md` completes it. The speeds are wall-clock measurements
of the host utility, unless a command reports its own throughput. The unit is MiB/s.

The figure below shows the three measured paths. The RF path records the RX stream straight to
the sectors of the SSD. The two host paths copy between the host and the SSD: over PCIe by DMA
to the host memory, over Ethernet through a staging buffer of the FPGA that Etherbone reads and
writes.

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

No utilisation figure is published. The public statement is that the base design uses
a fraction of the XC7A200T and leaves room for user processing (`README.md`). The utilisation of
a build is in the Vivado reports of its build directory
([The build flow](#build-flow)).
