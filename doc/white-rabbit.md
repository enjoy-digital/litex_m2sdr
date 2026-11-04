# White Rabbit on M2SDR

WR requires the Acorn Baseboard Mini and an SFP port. M2SDR uses the reusable
`add_white_rabbit` integration helper and its automatic HDL source registration.
The default is uRV with 128 KiB of private RAM and embedded firmware.

## Dependency

This integration requires the host management, CPU memory/boot and application
time APIs of [LiteX-WR-NIC](https://github.com/enjoy-digital/litex_wr_nic). CI pins
commit `31cfa02e80814f93084de652cfe42a975b14c164`; use that commit or a later `main`.
M2SDR WR servo lock, jitter and PPS accuracy still need measurement.

The `litex_wr_nic` checkout is looked up in this order: `--wr-nic-dir`,
`LITEX_WR_NIC_DIR`, then `./litex_wr_nic` and `../litex_wr_nic` next to this
repository. `--wr-firmware` overrides the firmware image picked from it.

Run the following commands from the M2SDR repository, with the dependency path
pointing to the **repository root**:

```sh
export LITEX_WR_NIC_DIR=/path/to/litex_wr_nic
export PYTHONPATH="$LITEX_WR_NIC_DIR${PYTHONPATH:+:$PYTHONPATH}"
./litex_m2sdr.py --variant=baseboard --wr-status
```

## Build

Default uRV/private RAM, with PCIe and JTAGBone access:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit --build
```

VexRiscv `lite` with embedded firmware in integrated RAM:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit \
    --wr-cpu-type=vexriscv --wr-cpu-memory=integrated --build
```

VexRiscv with integrated RAM held for host loading:

```sh
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit \
    --wr-cpu-type=vexriscv --wr-cpu-memory=integrated --wr-cpu-boot=host --build
```

uRV can also use integrated RAM with embedded or host boot. Private RAM requires
uRV and embedded boot. This target has no WR HyperRAM/DDR or SPI-boot option.
`--wr-sfp=0` or `--wr-sfp=1` chooses a port; omission selects the first available.

The build helper selects CPU-specific firmware and uses the `acorn` firmware
target. A custom `--wr-firmware image.bram` for integrated embedded boot needs
the corresponding `image.bin` alongside it. Omitting `--build` performs only
elaboration, building firmware first if it is missing. Use `--load` with the
same configuration to load the generated image.

Build directory names include nondefault CPU, variant, memory and boot choices.
Save the generated `scripts/csr.csv` with each bitstream: it is overwritten by
the next configuration. Rebuild host software against the matching generated
headers when changing configuration.

## Console and host loading

Start a LiteX server using the board's PCIe address, for example:

```sh
sudo litex_server --pcie --pcie-bar=04:00.0
```

The [debugging guide](debugging-guide.md) covers other available transports.
In another terminal with the dependency on `PYTHONPATH`:

```sh
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv status
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv console --command ver
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv console --command uptime
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv read-time
```

With host boot, load firmware before using the console:

```sh
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv load-firmware \
    "$LITEX_WR_NIC_DIR/litex_wr_nic/firmware/spec_a7_wrc_vexriscv.boot"
```

The common host tool verifies the full reserved RAM before releasing the CPU.
The shared UART provides a 4096-byte RX FIFO, occupancy and overflow reporting.
The WR host window remains at `0x00040000`, size `0x00040000`; generated UART
and management CSRs require the new bitstream's CSV.

Both MMCM backends accept coherent tuning commands from `wr_sys` and wait for
their own `PSDONE` in `clk200`. Their status/counters report busy, completion
timeout, completed shifts and superseded commands. A completion timeout requires
resetting the backend and MMCM, for example by reloading the FPGA.

Both WR MMCMs explicitly use `fractional=False`: fine phase shifting requires
integer output division. Their nominal outputs remain 125 MHz and 62.5 MHz.
The selected VCO is 1.5 GHz, giving approximately 11.905 ps per shift. The
previous fractional DMTD configuration used a 1.59375 GHz VCO and an output
divider of 25.5, which is incompatible with fine phase shifting. Correcting it
changes the DMTD phase-step size by 6.25%; firmware PI coefficients are unchanged.

The dependency's `test/test_wr_clock.py` covers the backends in simulation. These
digital simulations do not measure physical clock jitter or closed-loop WR
performance.

`read-time` snapshots WR's native time. The existing SDR `time_gen` is a separate
timebase; this update does not connect WR timecode to SDR sample timestamps.
CPU/console operation and valid snapshots do not establish servo lock, jitter
or calibrated PPS accuracy.
