# SATA Workflows

This page describes the user-facing SATA flow. Use `m2sdr_sata info` first:
it reports the selected transport, SATA link state, drive identify data,
catalog state, and whether host file I/O will use PCIe DMA or Etherbone.

## Bring-Up

Ethernet:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-sata --build --load
cd litex_m2sdr/software/user
make m2sdr_sata
./m2sdr_sata -i 192.168.1.50 info
./m2sdr_sata -i 192.168.1.50 init
```

`init` creates the catalog when it is missing. If the catalog already contains
entries, `init` refuses to reset it unless `--force` is provided. Resetting the
catalog never erases sample data sectors.

PCIe:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-sata --build --load
sudo ./rescan.py
cd litex_m2sdr/software/user
make m2sdr_sata
./m2sdr_sata -c 0 info
./m2sdr_sata -c 0 init
```

## Capture And Retrieve

### Record and replay what GQRX is receiving

Build and load the Ethernet + SATA image, then rebuild the user software and
SoapySDR module so gateware and software use the same CSR map:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-sata --build --flash
make -C litex_m2sdr/software/user clean
make -C litex_m2sdr/software/user
cmake -S litex_m2sdr/software/soapysdr -B /tmp/litex_m2sdr_soapy \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build /tmp/litex_m2sdr_soapy
sudo cmake --install /tmp/litex_m2sdr_soapy
```

The clean user build is important after changing gateware because generated CSR
offsets are compiled into `libm2sdr` and the SoapySDR module.  Installing with
the `/usr` prefix replaces the module in Debian/Ubuntu's multiarch SoapySDR
directory; a default `/usr/local` install can otherwise be shadowed by an older
module under `/usr`.

Start GQRX normally with the Ethernet device
`driver=LiteXM2SDR,eth_ip=192.168.1.50`, tune it, and enable DSP. The SATA
utility can then record the exact RX stream that continues to feed GQRX:

```sh
cd litex_m2sdr/software/user
./m2sdr_sata -i 192.168.1.50 capture-current my_capture --seconds 10
```

`capture-current` reads the active sample rate, format, channel layout,
frequency, and bandwidth published by the SoapySDR driver. It does not retune
the RFIC or disconnect GQRX. An explicit `--size` can be used instead of
`--seconds`.

The required sustained write rate is:

```text
sample rate x bytes per complex sample x channel count
```

For example, 1T1R SC16 needs 15.26 MiB/s at 4 MS/s and 38.15 MiB/s at
10 MS/s. The sustained rate is a drive property, not an Ethernet or
FIFO-capacity limit, and varies enormously between drives and with drive
state. A degraded DRAM-less WDC WDS120G1G0A sustained only 10--25 MiB/s on
long LiteSATA writes (4 MS/s SC16 was real-time, 10 MS/s backpressured the
shared RX path and took about twice the requested duration), while a Samsung
850 EVO 250GB on the same gateware sustained 30.72 MS/s SC16 2T2R
(233 MiB/s) in real time. Short writes can be much faster than a drive's
sustained rate because they are absorbed by its SLC/DRAM cache. Use a rate
below the measured sustained write speed when GQRX must remain smooth; see
`sata-bandwidth-investigation.md` for the measurement recipe. If the utility
reports that elapsed capture time differs from the requested time, treat that
capture as backpressured and lower the rate, bit depth, or channel count.

Replay the named capture into the same Ethernet RX path:

```sh
./m2sdr_sata -i 192.168.1.50 serve my_capture
```

GQRX remains open and DSP-active: during `serve` it displays the samples read
from SATA, and when replay finishes the normal live RF stream is restored.
Etherbone control is shared by routing replies to each process's UDP source
port; no second control port or dashboard is required.

To reset an idle or cleanly stopped streamer frontend:

```sh
./m2sdr_sata -i 192.168.1.50 stop both
```

The frontend reset cannot abort an ATA command that was already accepted by
the SATA core. Interrupting a capture, play, or serve with Ctrl-C therefore
stops at the current command boundary (at most one 32 MiB command finishes,
typically well under a second) and the utility exits with the SATA core
usable; a stopped capture is not registered in the catalog. The `stop`
command applies the same boundary stop to a busy streamer before resetting
it. On gateware without the streamer stop CSRs the interrupt falls back to
finishing the whole programmed transfer (a replay is unpaced so it drains as
fast as the destination accepts). A second Ctrl-C abandons the transfer:
after that, or after a timeout, run `info`. If it reports
`Drive: Not present` while the PHY is still ready, reload the FPGA bitstream
before issuing another SATA command.

### Standalone RF capture

Record RF samples directly to SATA:

```sh
./m2sdr_sata -i 192.168.1.50 --dry-run capture fm_test \
    --seconds 2 \
    --sample-rate 4M \
    --format sc16 \
    --channel-layout 1t1r \
    --rx-freq 100M \
    --rx-gain 20 \
    --bandwidth 5M

./m2sdr_sata -i 192.168.1.50 capture fm_test \
    --seconds 2 \
    --sample-rate 4M \
    --format sc16 \
    --channel-layout 1t1r \
    --rx-freq 100M \
    --rx-gain 20 \
    --bandwidth 5M
```

List and inspect the catalog:

```sh
./m2sdr_sata -i 192.168.1.50 info
./m2sdr_sata -i 192.168.1.50 list
./m2sdr_sata -i 192.168.1.50 show fm_test
./m2sdr_sata -i 192.168.1.50 check
```

Export a capture as SigMF, or raw payload only:

```sh
./m2sdr_sata -i 192.168.1.50 --dry-run export fm_test /tmp/fm_test.sigmf-meta
./m2sdr_sata -i 192.168.1.50 export fm_test /tmp/fm_test.sigmf-meta
./m2sdr_sata -i 192.168.1.50 export fm_test /tmp/fm_test.sc16 --raw
```

## Import And Replay

Import a raw file with metadata:

```sh
./m2sdr_sata -i 192.168.1.50 --dry-run import tx_test /tmp/tx.sc16 \
    --sample-rate 4M \
    --format sc16 \
    --channel-layout 1t1r \
    --tx-freq 2400M \
    --tx-att 20

./m2sdr_sata -i 192.168.1.50 import tx_test /tmp/tx.sc16 \
    --sample-rate 4M \
    --format sc16 \
    --channel-layout 1t1r \
    --tx-freq 2400M \
    --tx-att 20
```

Import a SigMF dataset. The input can be the `.sigmf-meta`, `.sigmf-data`, or
basename:

```sh
./m2sdr_sata -i 192.168.1.50 import tx_sigmf /tmp/tx.sigmf-meta
```

Replay a named entry to RF:

```sh
./m2sdr_sata -i 192.168.1.50 play tx_sigmf
```

Replay a named entry into the normal host RX path used by SoapySDR/GQRX:

```sh
./m2sdr_sata -i 192.168.1.50 serve fm_test
./m2sdr_sata -c 0 serve fm_test
```

`serve` infers `eth` or `pcie` from the selected device. Use
`--dst pcie|eth` only when overriding that default.

## Diagnostics

Raw sector and routing tools are under `diag`:

```sh
./m2sdr_sata -i 192.168.1.50 diag etherbone-bench
./m2sdr_sata -c 0 diag pcie-dma-bench 0x200000 32768
./m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-write 0x260000 8192
./m2sdr_sata -i 192.168.1.50 --pattern counter diag pattern-check 0x260000 8192
./m2sdr_sata -i 192.168.1.50 diag read 0x260000 8192 /tmp/raw.bin
./m2sdr_sata -i 192.168.1.50 diag write /tmp/raw.bin 0x280000
```

The catalog is stored at sector `0x800`. Automatic data allocation starts at
sector `0x100000`, and each named entry reserves a small SigMF metadata region
next to its sample data.
