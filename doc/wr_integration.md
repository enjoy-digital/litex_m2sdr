# White Rabbit on M2SDR

WR requires the Acorn Baseboard Mini and an SFP port. M2SDR uses the reusable
`add_white_rabbit` integration helper and its automatic HDL source registration.
The default is uRV with 128 KiB of private RAM and embedded firmware.

## Dependency

This integration requires the core, memory/boot, management and clock APIs from
[LiteX-WR-NIC #72–#77](https://github.com/enjoy-digital/litex_wr_nic/pull/77).
CI pins commit `98034d6ea2545bc3d9b513030221e6ef577e3479` while that series is
pending merge. Select a checkout containing that commit or the merged APIs.
The dependency's MMCM backend still needs physical clock/WR servo qualification.
It preserves the previous nominal MMCM tuning rate and polarity; this update
does not retune the Acorn firmware PI coefficients. The dependency also keeps
the original `PSGen` implementation for older integrations. M2SDR explicitly
uses `WRMMCMBackend` with both completion signals connected.

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

`read-time` snapshots WR's native time. The existing SDR `time_gen` is a separate
timebase; this update does not connect WR timecode to SDR sample timestamps.
CPU/console operation and valid snapshots do not establish servo lock, jitter
or calibrated PPS accuracy.
