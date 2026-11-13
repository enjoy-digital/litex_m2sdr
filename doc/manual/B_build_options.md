# Build options {#build-options}

This appendix lists every option of the target script, as `./litex_m2sdr.py --help` prints
them, with their defaults. The chapters explain when to use them:
[Building the gateware](#building-gateware) for the commands, [The SoC](#soc) for what each
option builds and for the rules between options ([Option rules](#soc-rules)).

An option without a value is a switch, off by default. `BaseSoC` takes the same settings as
keyword arguments: `--with-eth` is `with_eth=True`, `--eth-phy=2500basex` is
`eth_phy="2500basex"`. The exceptions are noted in the tables.

## Variant and actions {#opt-actions}

| Option | Default | Effect |
|-----------------------------------|-------------|----------------------------------------------------|
| `--variant` | `m2` | Carrier: `m2` or `baseboard` ([Variants](#soc-variants)) |
| `--sys-clk-freq` | see below | System clock frequency in Hz |
| `--build` | off | Run Vivado and build the bitstream |
| `--load` | off | Load the bitstream over JTAG |
| `--flash` | off | Write the single image at offset 0 of the flash, over JTAG |
| `--flash-multiboot` | off | Write and verify the fallback and operational images, over JTAG |
| `--reset` | off | Reset the FPGA through the JTAG programmer |
| `--rescan` | off | Rescan the PCIe bus and reload the driver after the load or the flash |
| `--driver` | off | Copy the generic LitePCIe driver over the software of the repository |
| `--without-jtagbone` | off | Remove the JTAGBone host bridge |

The default system clock is 125 MHz, except 100 MHz with `--with-eth`; a `--eth-phy=2500basex`
build stays at 125 MHz ([System clock](#build-sysclk)). The action options are described in
[Loading and flashing](#build-load); they are not arguments of `BaseSoC`, and
`--without-jtagbone` is its `with_jtagbone=False`.

`BaseSoC` uses 125 MHz unless `sys_clk_freq` is given: pass `sys_clk_freq=int(100e6)` for an
Ethernet build. `--wr-status` and `--wr-firmware-target` are handled by the script, and `wr_sfp`
has no automatic value in `BaseSoC`.

## RFIC {#opt-rfic}

| Option | Default | Effect |
|-------------------------------------------------|-------------|--------------------------------------|
| `--with-rfic-oversampling` | off | Experimental: oversampling PHY and 128-bit sample path, for 2T2R at 122.88 MSPS |

The oversampling mode is Experimental ([Status](#intro-status)). It is meant for the `m2`
variant with PCIe on two or four lanes ([Oversampling mode](#rfic-oversampling)). The script
does not refuse it with Ethernet or SATA, but those paths are 64 bits wide and lose half of each
word: do not combine them. The BFP8 format does not exist on this build.

## PCIe {#opt-pcie}

| Option | Default | Effect |
|-------------------------------------------------------|-------------|--------------------------------|
| `--with-pcie` | off | PCIe host link: register access and DMA streaming |
| `--pcie-gen` | `2` | PCIe generation: `1` or `2` |
| `--pcie-lanes` | `1` | Lanes: `1`, `2` or `4`; one lane only on the baseboard |
| `--pcie-dmas` | `1` | DMA channels; channel 0 carries the samples |
| `--with-pcie-ptm` | off | PCIe PTM requester; needs `--with-pcie`, PCIe Gen2 x1 only |
| `--with-pcie-reset-workaround` | off | Toggle the PCIe reset until the link is up |

`--pcie-dmas` above 1 serves the designs that extend the gateware
([Extending the gateware](#build-extend)). The PPS input of the start synchroniser is connected
on channel 0 only: a design that uses an extra channel connects it. PCIe is off by default on
the command line, but `BaseSoC` has `with_pcie=True` as its default.

On the `baseboard` variant, `--pcie-lanes=2` or `4` stops the script on an assertion without a
message.

## Ethernet {#opt-eth}

| Option | Default | Effect |
|----------------------------------------------|-------------------------|-----------------------------|
| `--with-eth` | off | Ethernet host link: Etherbone and UDP streaming |
| `--eth-sfp` | `0` | SFP cage of the baseboard: `0` or `1` |
| `--eth-phy` | `1000basex` | PHY: `1000basex`, `2500basex` or `5000baser` |
| `--eth-local-ip` | `192.168.1.50` | IP address of the board |
| `--eth-udp-port` | `2345` | UDP port of the sample streams |
| `--with-eth-ptp` | off | PTP, with the discipline of the board time |
| `--eth-ptp-p2p` | off | PTP peer-to-peer delay mode, instead of end-to-end |
| `--eth-ptp-no-igmp` | off | No IGMP reports for the PTP multicast groups |
| `--eth-ptp-igmp-interval` | `2.0` | Interval of the IGMP reports, in seconds |
| `--with-eth-ptp-rfic-clock` | off | PTP discipline of the FPGA 10 MHz clock that feeds the clock generator |
| `--with-eth-vrt` | off | VRT streamer on the RX path; not selected after load |
| `--vrt-dst-ip` | `239.168.1.100` | Destination IP address of the VRT packets |
| `--vrt-dst-port` | `4991` | Destination UDP port of the VRT packets |

Ethernet needs the `baseboard` variant. `--eth-ptp-no-igmp` is `eth_ptp_igmp=False` in
`BaseSoC`. The PHYs and the streaming are in [Host links](#host-links), PTP in
[Time synchronisation](#timing); the `5000baser` PHY is Experimental
([Status](#intro-status)). The shipped software cannot select the VRT route
([VRT streaming](#flow-eth-vrt)).

## SATA {#opt-sata}

| Option | Default | Effect |
|---------------------------------|------------------|-------------------------------------------------|
| `--with-sata` | off | SATA storage on the baseboard |
| `--sata-gen` | `2` | SATA generation: `1`, `2` or `3` |

SATA needs the `baseboard` variant and cannot be built with both PCIe and Ethernet. Generation 3
needs a system clock of at least 150 MHz, above the defaults: `--sata-gen=3` alone is always
refused, and also needs `--sys-clk-freq` ([System clock](#build-sysclk)).

## GPIO and PPS {#opt-gpio}

| Option | Default | Effect |
|----------------------------|---------------|---------------------------------------------------------|
| `--with-gpio` | off | GPIO core on the two test points of the board |
| `--pps-input` | `m2` | PPS input pin: `m2` (M.2 connector), `tp1` or `tp2` (test points) |

A PPS input on a test point excludes `--with-gpio`, which uses the same pins
([Hardware](#hardware)). The GPIO core is 4 bits wide and only bits 0 and 1 reach a pin. The PPS
input is only monitored: it does not set the board time.

## White Rabbit {#opt-wr}

| Option | Default | Effect |
|----------------------------------------|------------------------|------------------------------------|
| `--with-white-rabbit` | off | Experimental: White Rabbit core on an SFP cage of the baseboard |
| `--wr-sfp` | automatic | SFP cage: `0` or `1`; the first free one by default |
| `--wr-dac-bits` | `16` | Width of the phase-shift control word of the clocking |
| `--wr-nic-dir` | environment | Path of the `litex_wr_nic` checkout |
| `--wr-firmware` | none | Path of the firmware image of the core |
| `--wr-firmware-target` | `acorn` | Firmware build target, used with `--build` |
| `--wr-cpu-type` | `urv` | CPU of the core: `urv` or `vexriscv` |
| `--wr-cpu-variant` | none | CPU variant, `lite`, for `vexriscv` only |
| `--wr-cpu-memory` | `private` | CPU memory: `private` or `integrated` |
| `--wr-cpu-boot` | `embedded` | Firmware source: `embedded` or `host`; `host` needs `integrated` |
| `--wr-status` | off | Print the resolved White Rabbit environment |
| `--wr-ext-clk10-port` | none | Vivado port of an external 10 MHz clock to constrain |
| `--wr-ext-clk10-period` | `100.0` | Period of that clock in ns |
| `--wr-ext-clk10-name` | `wr_ext_clk10` | Name of that clock in the constraints |

`--wr-nic-dir` defaults to the `LITEX_WR_NIC_DIR` environment variable. Without both, the script
looks for a `litex_wr_nic` directory in the repository, then next to it. `--wr-status` alone
prints the environment and exits without elaborating the SoC.

White Rabbit is Experimental ([Status](#intro-status)). It needs the `baseboard` variant and
excludes `--with-eth-ptp` ([Time synchronisation](#timing)). The script accepts it with
`--with-eth`, but the two cores would share one PLL channel and its reset: do not enable them
together.

## LiteScope probes {#opt-probes}

The probe options are mutually exclusive: a build holds one LiteScope analyzer at most.
[Probes](#soc-probes) gives the signals, the clock domain and the depth of each, and
[LiteScope](#debug-litescope) their use.

| Option | Probe |
|-------------------------------------------------------|---------------------------------------------|
| `--with-pcie-probe` | PCIe reset, reference clock and link training state |
| `--with-pcie-dma-probe` | PCIe DMA streams, PPS and header resets |
| `--with-si5351-i2c-probe` | I2C bus and sequencer of the clock generator |
| `--with-eth-phy-rx-probe` | Raw RX symbols of the Ethernet PHY |
| `--with-eth-tx-probe` | Stream entering the Ethernet TX streamer |
| `--with-ad9361-spi-probe` | SPI pins of the AD9361 |
| `--with-ad9361-data-probe` | TX and RX samples of the RFIC PHY |

## Other tools {#opt-others}

The options of the other scripts are given where the scripts are described.

| Tool | Options in |
|------------------------------------------------------|----------------------------------------------|
| `release.py` | [Release archives](#build-release): `--nobuild` |
| `scripts/github_release.py` | [Publishing a release](#build-publish) |
| `litex_m2sdr/software/flash.py` | [Flashing over PCIe](#build-flash-pcie) |
| `scripts/flash_release.py` | [Flashing a release image](#gs-flash) |
| `litex_m2sdr/software/build.py` | [Getting started](#getting-started) |
| Hardware scripts of `scripts/` | [Hardware scripts](#debug-scripts) |
