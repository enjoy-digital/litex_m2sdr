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
