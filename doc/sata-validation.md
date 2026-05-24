# SATA Hardware Validation

This page records known-good SATA validation on the LiteX-M2SDR baseboard with
the SATA SSD connected. Speeds are host utility wall-clock measurements unless a
command reports its own throughput.

SATA host access is through `m2sdr_sata`: PCIe uses the `LITEPCIE_IOCTL_SATA_DMA`
userspace DMA path, while Ethernet uses Etherbone access to the SATA host
staging buffer.

## Architecture Notes

The SATA integration is split between the board target and reusable gateware
helpers:

- `BaseSoC.add_sata()` wires the board-specific PHY, LiteSATA core, identify
  CSR, DMA masters, host staging buffer, RF streamers, IRQs, and timing
  constraints.
- `litex_m2sdr/gateware/sata.py` contains the M2SDR-specific LiteSATA wrappers
  and the host-buffer/router helpers.
- Existing SATA CSR names and bit meanings are part of the userspace ABI.
  Prefer comments, helper functions, and tests over CSR renaming or repacking.

Direction names are used consistently in gateware and userspace:

| Name | Direction |
| ---- | --------- |
| `Sector2Mem` | SATA sectors to host memory/staging buffer |
| `Mem2Sector` | host memory/staging buffer to SATA sectors |
| `Stream2Sectors` | RF receive stream to SATA sectors |
| `Sectors2Stream` | SATA sectors to RF transmit or host replay stream |

The default host staging buffer is 128 KiB. It provides 256-sector SATA chunks
for host-buffer workflows and avoids the RAMB cascade DRC issue observed with a
256 KiB buffer on the current Artix-7 target. Ethernet host-buffer transfers use
128-word Etherbone bursts by default.

## Current Hardware Numbers

These are the latest validated numbers from the 2026-05-24 hardware run.

| Transport | User-visible path | Host to SATA | SATA to host | Validation |
| --------- | ----------------- | ------------ | ------------ | ---------- |
| PCIe | SigMF import/export, 8 MiB | 0.112 s, about 71.4 MiB/s | 0.133 s, about 60.2 MiB/s | exported data matched with `cmp`; metadata validated |
| PCIe | raw DMA benchmark, 16 MiB | 93.202 MiB/s | 63.391 MiB/s | `pcie-dma-bench` write/read/verify passed |
| Ethernet/Etherbone | SigMF import/export, 8 MiB small-file case | 0.167 s, about 47.8 MiB/s | 0.314 s, about 25.5 MiB/s | exported data matched with `cmp`; metadata validated |
| Ethernet/Etherbone | SigMF import/export, 64 MiB sustained case | 1.289 s, about 49.7 MiB/s | 1.882 s, about 34.0 MiB/s | exported data matched with `cmp`; metadata validated |
| Ethernet/Etherbone | raw `write-file`/`read-file`, 16 MiB | 46.0, 53.5, 53.5 MiB/s | 31.5, 34.0, 34.0 MiB/s | all readbacks matched with `cmp` |

The Ethernet SigMF and raw file rows are not identical measurements. Both use
the same Etherbone host-buffer transfer and SATA sector-copy helpers, but
`write-file`/`read-file` moves only a raw sector payload to/from a fixed sector
range and is the clearest host-to-SATA/SATA-to-host payload throughput number.
`import-sigmf`/`export-sigmf` measures the end-to-end named-capture workflow:
catalog lookup/update, SigMF metadata parsing/formatting, metadata sector I/O,
and host `.sigmf-meta`/`.sigmf-data` file handling are included in the wall
time. It also used an 8 MiB dataset instead of the 16 MiB raw file test, so
fixed command overhead has more weight.

For sustained Ethernet SigMF throughput, use the 64 MiB row. It amortizes the
fixed catalog/metadata overhead and lands close to the raw payload transfer
numbers: about 49.7 MiB/s host-to-SATA and 34.0 MiB/s SATA-to-host.

## Validated Commands

PCIe + SATA gateware:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-sata --load
sudo ./rescan.py
cd litex_m2sdr/software/user
./m2sdr_sata -c 0 status
./m2sdr_sata -c 0 pcie-dma-bench 0x200000 32768
./m2sdr_sata -c 0 --pattern counter write-pattern 0x240000 8192
./m2sdr_sata -c 0 --pattern counter verify-pattern 0x240000 8192
```

Ethernet + SATA gateware:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-sata --load
cd litex_m2sdr/software/user
./m2sdr_sata -i 192.168.1.50 status
./m2sdr_sata -i 192.168.1.50 etherbone-bench --iterations 3
./m2sdr_sata -i 192.168.1.50 --pattern counter write-pattern 0x260000 8192
./m2sdr_sata -i 192.168.1.50 --pattern counter verify-pattern 0x260000 8192
```

In both cases, SATA PHY, TX, RX, and CTRL were ready. Pattern tests verified
4 MiB of counter data. SigMF round-trips used `import-sigmf` and `export-sigmf`;
the exported `.sigmf-data` matched the source data byte-for-byte and the
exported metadata passed `m2sdr_sigmf --validate`.

## Ethernet Optimization History

The current Ethernet numbers use a 128 KiB SATA host staging buffer and the
default 128-word Etherbone burst size. Earlier measurements are kept here to
show what changed.

| Ethernet path | Size | Host to SATA | SATA to host |
| ------------- | ---- | ------------ | ------------ |
| Original Etherbone path | 8 MiB SigMF | 0.877 s, about 9.1 MiB/s | 1.099 s, about 7.3 MiB/s |
| Pipelined Etherbone reads | 8 MiB SigMF | 0.882 s, about 9.1 MiB/s | 0.303-0.308 s, about 26.0-26.4 MiB/s |
| 128 KiB SATA host buffer | 8 MiB SigMF | 0.167 s, about 47.8 MiB/s | 0.314 s, about 25.5 MiB/s |
| 128 KiB SATA host buffer | 64 MiB SigMF | 1.289 s, about 49.7 MiB/s | 1.882 s, about 34.0 MiB/s |
| 128 KiB SATA host buffer | 16 MiB file | 46.0, 53.5, 53.5 MiB/s | 31.5, 34.0, 34.0 MiB/s |

The 128 KiB host buffer reduces the number of SATA `MEM2SECTOR` commands needed
for Ethernet imports/writes. A 256 KiB buffer was tested first but rejected
because Vivado DRC failed on cascaded RAMB36 address pins.

Etherbone host-buffer burst benchmark:

| Words | Bytes | Write MiB/s | Read MiB/s |
| ----: | ----: | ----------: | ---------: |
| 1 | 12 | 0.880 | 0.107 |
| 2 | 24 | 1.272 | 0.214 |
| 4 | 48 | 3.270 | 0.428 |
| 8 | 96 | 6.539 | 0.864 |
| 16 | 192 | 14.085 | 1.695 |
| 32 | 384 | 26.158 | 2.977 |
| 64 | 768 | 56.340 | 5.051 |
| 128 | 1536 | 104.632 | 7.710 |

Multi-record Etherbone was reviewed but not implemented. LiteEth's Etherbone
frontend currently documents one record per frame and no address space/flag
support (`rca`, `bca`, `wca`, `wff`). With a standard Ethernet MTU, 128-word
records would only pack about two records per frame, so after read pipelining
the expected incremental gain is mostly fewer packets/syscalls rather than a
fundamental transfer-rate change.

Additional software-only read optimizations were tested on a 16 MiB
`read-file 0x128000 32768` transfer over Ethernet + SATA:

| Variant | Read throughput |
| ------- | --------------- |
| Current pipelined Etherbone reads | 29.8, 32.4, 32.6 MiB/s |
| Direct little-endian output buffer, larger UDP socket buffers, Linux `sendmmsg`/`recvmmsg` batching | 30.6, 32.0, 31.5 MiB/s |
| Direct little-endian output buffer and larger UDP socket buffers only | 31.9, 32.4, 31.7 MiB/s |

The exported data matched byte-for-byte in these runs, but the changes were
within measurement noise and sometimes slower, so they were not kept.

## Notes

- The catalog entries created for validation were deleted after each run. Data
  sectors were not erased.
- Reloading between Ethernet + SATA and PCIe + SATA requires a PCIe rescan
  before `/dev/m2sdr0` can be used again.
- If `pcie-dma-bench` reports `unsupported`, the loaded `m2sdr.ko` does not
  expose the SATA userspace DMA ioctl. That is a driver mismatch, not expected
  PCIe SATA performance.
