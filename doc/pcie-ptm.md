# PCIe PTM Host Time Sync

With PCIe Precision Time Measurement (PTM), the board time (exposed by the kernel driver as a PTP
hardware clock, PHC) can follow the host clock over PCIe. If the host clock is itself locked by
NTP/PTP, the board follows that disciplined host time.

PTM uses LitePCIe directly and does not require LiteX-WR-NIC. It relies on LitePCIe's
`S7PCIEPHY.create_ptm_sniffer()` ([litepcie#187](https://github.com/enjoy-digital/litepcie/pull/187),
merged in `56a97c9`) and is currently supported with PCIe Gen2 x1 only.

For Ethernet-based time distribution, see [Ethernet PTP Bring-Up](ptp/README.md).

## Build

```bash
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=1 --with-pcie-ptm --build --load
```

## Run The Host Sync Helper

Start the helper after the kernel driver has created the M2SDR PHC:

```bash
scripts/m2sdr_pcie_time_sync.py --dry-run
sudo scripts/m2sdr_pcie_time_sync.py --stdout
```

The helper auto-detects `/sys/class/ptp/ptp*/clock_name == m2sdr` and runs
`phc2sys -s CLOCK_REALTIME -c /dev/ptpN`, so the board is the sink and the host is the source. For
multi-board systems, pass `--phc /dev/ptpN`.

## Start At Boot

Install a systemd service similar to:

```ini
[Unit]
Description=Synchronize M2SDR PCIe board time to host time
After=multi-user.target

[Service]
ExecStart=/path/to/litex_m2sdr/scripts/m2sdr_pcie_time_sync.py --stdout
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```
