# SATA Hardware Validation

This file records known-good SATA host I/O validation runs. Speeds are measured
from the host utility wall time unless the command reports its own timing.

## 2026-05-24, SigMF SATA round-trip

Tested on the LiteX-M2SDR baseboard with the SATA SSD connected. Software was
at `64b22db` (`m2sdr_sata: keep imports on one device session`) after the
SigMF SATA workflow changes.

### PCIe + SATA

Gateware:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-sata --load
sudo ./rescan.py
```

Validity:

- `./m2sdr_sata -c 0 status`: SATA PHY, TX, RX, and CTRL all ready.
- `./m2sdr_sata -c 0 pcie-dma-bench 0x200000 32768`: 16 MiB write/read/verify ok.
- `./m2sdr_sata -c 0 --pattern counter write-pattern 0x240000 8192`
  followed by `verify-pattern`: 4 MiB counter pattern verified.
- 8 MiB SigMF `import-sigmf` + `export-sigmf`: exported data matched the input
  with `cmp`; exported metadata passed `m2sdr_sigmf --validate`.

Measured throughput:

| Test | Size | Write/import | Read/export |
| ---- | ---- | ------------ | ----------- |
| `pcie-dma-bench` | 16 MiB | 93.202 MiB/s | 63.391 MiB/s |
| SigMF round-trip | 8 MiB | 0.112 s, about 71.4 MiB/s | 0.133 s, about 60.2 MiB/s |

### Ethernet + SATA

Gateware:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-sata --load
```

Reachability:

- `ping -c 2 192.168.1.50`: 2/2 replies, about 0.084 ms average.
- `./m2sdr_sata -i 192.168.1.50 status`: SATA PHY, TX, RX, and CTRL all ready.

Validity:

- `./m2sdr_sata -i 192.168.1.50 etherbone-bench --iterations 3`: all burst
  sizes from 1 to 128 words passed.
- `./m2sdr_sata -i 192.168.1.50 --pattern counter write-pattern 0x260000 8192`
  followed by `verify-pattern`: 4 MiB counter pattern verified.
- 8 MiB SigMF `import-sigmf` + `export-sigmf`: exported data matched the input
  with `cmp`; exported metadata passed `m2sdr_sigmf --validate`.
- Exported SigMF metadata kept the stored M2SDR extension fields, including
  `m2sdr:transport` set to `ethernet`.

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

Measured SigMF round-trip throughput:

| Test | Size | Write/import | Read/export |
| ---- | ---- | ------------ | ----------- |
| SigMF round-trip over Etherbone | 8 MiB | 0.877 s, about 9.1 MiB/s | 1.099 s, about 7.3 MiB/s |
| SigMF round-trip over Etherbone, pipelined reads | 8 MiB | 0.882 s, about 9.1 MiB/s | 0.303-0.308 s, about 26.0-26.4 MiB/s |

The pipelined read run kept the 128-word Etherbone bulk burst size and used an
8-request software read window. Three repeated exports matched the original
data byte-for-byte and completed in 0.308 s, 0.303 s, and 0.303 s.

Multi-record Etherbone was reviewed but not implemented here. LiteEth's
Etherbone frontend currently documents one record per frame and no address
space/flag support (`rca`, `bca`, `wca`, `wff`), so supporting several records
per UDP packet would require gateware changes. With a standard Ethernet MTU,
128-word records would only pack about two records per frame, so after
pipelining the expected incremental gain is mainly fewer packets/syscalls
rather than a fundamental transfer-rate change.

Additional software-only optimizations were prototyped after the pipelined read
change and measured on a 16 MiB `read-file 0x128000 32768` transfer over
Ethernet+SATA:

| Variant | Read throughput |
| ------- | --------------- |
| Current pipelined Etherbone reads | 29.8, 32.4, 32.6 MiB/s |
| Direct little-endian output buffer, larger Etherbone UDP socket buffers, Linux `sendmmsg`/`recvmmsg` batching | 30.6, 32.0, 31.5 MiB/s |
| Direct little-endian output buffer and larger Etherbone UDP socket buffers only | 31.9, 32.4, 31.7 MiB/s |

The exported data matched byte-for-byte in these runs, but the changes were
within measurement noise and sometimes slower, so they were not kept.

### Host file transfer path

These measurements use the user-visible host file/SigMF import/export commands,
not just the lower-level microbenchmarks. Ethernet/Etherbone used `write-file`
and `read-file` on a 16 MiB host file at sector `0x150000`; every readback
matched the input file with `cmp`. PCIe uses the SATA PCIe DMA path exercised by
the SigMF import/export flow, which calls the same sector copy helpers used by
plain file transfers.

| Transport | Host-to-SATA write | SATA-to-host read |
| --------- | ------------------ | ----------------- |
| Ethernet/Etherbone, `write-file`/`read-file`, 16 MiB | 1.709 s, 1.740 s, 1.719 s; 9.4, 9.2, 9.3 MiB/s | 0.539 s, 0.520 s, 0.492 s; 29.7, 30.8, 32.5 MiB/s |
| PCIe DMA-backed file path, SigMF import/export, 8 MiB | 0.112 s; about 71.4 MiB/s | 0.133 s; about 60.2 MiB/s |
| PCIe DMA microbenchmark, `pcie-dma-bench`, 16 MiB | 93.202 MiB/s | 63.391 MiB/s |

A later PCIe check with a stale `m2sdr.ko` reported `pcie-dma-bench` as
`unsupported` and fell back to the CSR host-buffer path. That fallback measured
only about 7.0 MiB/s write and 2.2 MiB/s read, so it is a diagnostic symptom of
the DMA ioctl not being available, not the expected PCIe performance.

### Notes

- The catalog entries created for this run were deleted after validation. Data
  sectors were not erased.
- Reloading between Ethernet+SATA and PCIe+SATA requires a PCIe rescan before
  `/dev/m2sdr0` can be used again.
