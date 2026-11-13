```{=latex}
\part{The gateware}
```

```{=html}
<h1 class="part">Part III. The gateware</h1>
```

# The SoC {#soc}

This chapter describes the FPGA design as a whole: the target script `litex_m2sdr.py`, its two
variants, its clock domains and the blocks it assembles. Read it before changing the gateware or
when a register of the board has to be traced back to the block that owns it.

The sample path is detailed in [The RFIC interface](#rfic) and [Streaming](#streaming), the host
links in [Host links](#host-links) and the clock discipline in [Timing](#timing).

## At a glance {#soc-glance}

| Item               | Value                                                                  |
|---------------------------------|---------------------------------------------------------------------------------------------------------------------|
| Target script      | `litex_m2sdr.py`, class `BaseSoC`                                      |
| Platform           | `litex_m2sdr_platform.py`, XC7A200T in SBG484 package, speed grade -3  |
| SoC type           | `SoCMini`: no CPU, registers driven by the host bridges                |
| Variants           | `m2` (M.2 slot) and `baseboard` (LiteX Acorn Baseboard Mini)           |
| System clock       | 125 MHz, or 100 MHz with 1000BASE-X or 5GBASE-R Ethernet               |
| Register bus       | CSR bus, 32-bit, block locations set by one map                        |
| Sample datapath    | 64-bit streams; 128-bit in the oversampling build                      |
| Toolchain          | Vivado                                                                 |

## Overview {#soc-overview}

The gateware is a LiteX SoC without a CPU. The host is the processor: it reads and writes the
registers of the FPGA through a host bridge and exchanges samples through a host link. The
[LiteX primer](#litex-primer) introduces these two notions.

The figure below shows the blocks. The top row is the sample path between the host and the
AD9361. The bottom row holds the blocks that only have registers: the board time, the control of
the SI5351 clock generator and the board utilities.

![Blocks of the SoC. The top row carries the samples in both directions; the bottom row is
reached through the CSR bus only, from a host link or from JTAG. The board time feeds the
timestamps of the header.](soc_blocks.svg)

One script builds every configuration. The options select the host links, the timing features
and the debug probes, and the script refuses most of the combinations that the hardware cannot
provide. [Option rules](#soc-rules) also lists the ones it accepts although they do not work.
The complete list of options is in [Build options](#build-options); the commands that build and
load a bitstream are in [Building the gateware](#building-gateware).

## Variants {#soc-variants}

A variant tells the gateware what the M.2 connector is plugged into. It is selected with
`--variant` and changes the use of the four transceiver lanes of the connector.

| Variant     | Carrier                    | PCIe lanes            | Other transceiver uses     |
|-----------------------------------|------------------------------------|-------------------------------|------------------------------------------------|
| `m2`        | M.2 slot                   | 1, 2 or 4 (lanes 0-3) | None                       |
| `baseboard` | LiteX Acorn Baseboard Mini | 1 (lane 3)            | 2 SFP (lanes 2 and 1), SATA (lane 0) |

The `baseboard` variant adds the SFP and SATA pins to the platform (`_io_baseboard`). Ethernet,
SATA and White Rabbit therefore need this variant: the script stops with an error when one of
them is requested with `m2`.

On the `m2` variant with 2 or 4 lanes, the script places each transceiver channel on the pin of
its M.2 lane (`M2_PCIE_GT_LOCS`). Lanes 1 and 2 are swapped on the board compared with the
default order of the PCIe core, and a link built without these constraints trains with one lane
only.

The carriers and their connectors are described in [Carriers](#carriers).

## Option rules {#soc-rules}

`BaseSoC` checks the options before it builds anything. The rules below are the ones the code
enforces: the script stops with an error.

| Option                       | Rule                                                              |
|----------------------------------------------------------------------------|--------------------------------------------------------------------------|
| `--with-eth`, `--with-sata`  | Need the `baseboard` variant                                      |
| `--with-pcie`                | PCIe, SATA and one of Ethernet or White Rabbit cannot all be enabled |
| `--with-white-rabbit`        | Needs the `baseboard` variant and an SFP. Excludes `--with-eth-ptp` |
| `--with-pcie-ptm`            | Needs `--with-pcie` and one lane                                  |
| `--with-eth-ptp`             | Needs `--with-eth`                                                |
| `--with-eth-ptp-rfic-clock`  | Needs `--with-eth-ptp`. Excludes White Rabbit                     |
| `--pps-input`                | The values `tp1` and `tp2` exclude `--with-gpio`                  |
| `--pcie-lanes`               | 2 and 4 need the `m2` variant                                     |
| `--sata-gen`                 | The value 3 needs a system clock of 150 MHz or more               |

The second rule comes from the transceiver PLL. The four lanes share one QPLL that has two
channels, so PCIe, Ethernet or White Rabbit, and SATA cannot all be enabled in one bitstream.
[Host links](#host-links) explains which pairs can coexist.

Two of these rules stop the script differently. The lane rule is a bare assertion, without a
message. The SATA rule is checked when the SATA core is added, against the system clock of the
build, which `--sys-clk-freq` can raise.

The script does not check every combination. No rule rejects the ones below, and they do not
give a working bitstream. They are limitations of the design: do not build them.

| Combination                                   | Result                                                  |
|-----------------------------------------------------------------|-----------------------------------------------------------------------|
| Oversampling with `--with-eth` or `--with-sata` | Half of the samples are lost, see [Limits of the build](#rfic-oversampling-limits) |
| `--with-eth` with `--with-white-rabbit`       | The two cores use the same QPLL channel, see [Host links](#host-links) |

The command line and the class do not have the same default for PCIe. `BaseSoC` enables PCIe
when it is instantiated from Python without arguments; the script enables it only with
`--with-pcie`.

## Clock domains {#soc-clocks}

The design has few clock domains. The table lists the domains created by
the clock and reset generator (`CRG`) and by the cores of the sample path. The transceiver clocks
of the host links are in [Host links](#host-links).

| Domain        | Source                                | Frequency              | Use                                   |
|------------------------------------|------------------------------------------|---------------------------------|---------------------------------------|
| `sys`         | 100 MHz oscillator, PLL               | 125 or 100 MHz         | Registers, streams, host link cores   |
| `idelay`, `clk200` | Same PLL                         | 200 MHz                | Delay calibration, MMCM phase shift   |
| `clk100`      | 100 MHz oscillator, direct            | 100 MHz                | Input of the White Rabbit MMCMs       |
| `clk10`       | 100 MHz oscillator, dedicated MMCM    | 10 MHz                 | 10 MHz reference for the SI5351C      |
| `time`        | SI5351 output `si5351_clk1`           | 100 MHz                | Board time counter                    |
| `rfic`        | `DATA_CLK` of the AD9361              | Up to 245.76 MHz, 491.52 MHz in oversampling mode | RFIC PHY and its FIFOs |
| `refclk_eth`  | PLL from `sys`, or `sys` itself       | 125 or 103.125 MHz     | Ethernet transceiver reference        |
| `refclk_sata` | PLL from `sys`                        | 150 MHz                | SATA transceiver reference            |
| `refclk_pcie` | Reference clock of the M.2 connector  | 100 MHz                | PCIe transceiver reference            |

The system clock is 125 MHz by default. The script lowers it to 100 MHz when Ethernet is enabled
with the 1000BASE-X or 5GBASE-R PHY, and keeps 125 MHz with 2500BASE-X, where the Ethernet
reference is the system clock itself. `--sys-clk-freq` overrides the choice.

The 10 MHz clock has its own MMCM so that its phase can be shifted in fine steps while the design
runs. This is how the gateware steers the reference of the SI5351C version of the board; the
mechanism is described in [Timing](#timing). With White Rabbit, the 10 MHz clock is derived from
the recovered White Rabbit clock by another MMCM.

The 200 MHz clock always exists because the MMCM phase shift interfaces use it. The delay
calibration block (`IDELAYCTRL`) is only instantiated in the oversampling mode, the one build
that uses input delays.

The `time` and `rfic` clocks come from outside the FPGA and are asynchronous to `sys`. Every
signal that crosses to or from them goes through a clock domain crossing block, and the timing
constraints declare the pairs asynchronous. [Clock domain crossings](#streaming-cdc) lists the
blocks.

## Blocks {#soc-blocks}

The table gives every block that `BaseSoC` instantiates, in the order of the script, with the
option that enables it and the chapter that describes it. The blocks marked SoC are in the
sections that follow.

| Attribute            | Class                     | Enabled by             | Chapter                |
|--------------------------------------------|--------------------------------------------|-------------------|------------------------------------------|
| `crg`                | `CRG`                     | Always                 | [SoC](#soc)     |
| `qpll`               | `SharedQPLL`              | Always                 | [Links](#host-links)   |
| `capability`         | `Capability`              | Always                 | [SoC](#soc) |
| `si5351`             | `SI5351`                  | Always                 | [Timing](#timing)      |
| `clk10_discipline`   | MMCM phase discipline     | No White Rabbit        | [Timing](#timing)      |
| `time_gen`           | `TimeGenerator`           | Always                 | [Stream](#streaming) |
| `pps_in`, `pps_gen`  | `PPSInput`, `PPSGenerator` | Always                | [Timing](#timing)      |
| `jtagbone`           | LiteX JTAGBone            | Default                | [Links](#host-links) |
| `icap`, `flash`      | `ICAP`, `S7SPIFlash`      | Always                 | [SoC](#soc)      |
| `xadc`, `dna`        | `S7SystemMonitor`, `S7DNA` | Always                | [SoC](#soc)       |
| `pcie_phy`, `pcie_dma0` | LitePCIe               | PCIe option          | [Links](#host-links)   |
| `eth_phy`, streamers | LiteEth                   | Ethernet option           | [Links](#host-links)   |
| `sata_phy`, streamers | LiteSATA                 | SATA option          | [Links](#host-links)   |
| `ad9361`             | `AD9361RFIC`              | Always                 | [RFIC](#rfic)          |
| `header`             | `TXRXHeader`              | Always                 | [Stream](#streaming) |
| `txrx_loopback`      | `TXRXLoopback`            | Always                 | [Stream](#streaming) |
| `crossbar`           | LiteX stream crossbar     | Always                 | [Stream](#streaming) |
| `leds`               | `StatusLed`               | Always                 | [SoC](#soc)        |
| `gpio`               | `GPIO`                    | GPIO option          | [SoC](#soc)       |
| `clk_measurement`    | Clock measurement         | Always                 | [RFIC](#rfic) |
| `analyzer`           | LiteScope analyzer        | One probe option       | [SoC](#soc)     |

JTAGBone is present unless `--without-jtagbone` is given.

The SoC is created with an identifier string that contains the variant and the build date:
"LiteX-M2SDR SoC / m2 variant / built on" followed by the date. The utilities print it, which is
the quickest way to check which bitstream a board runs.

## Register map {#soc-csr}

Most blocks with registers have a fixed location on the CSR bus. The locations are declared in
one dictionary at the top of `BaseSoC` (`SoCCore.csr_map`), and a location is a slot of `0x800`
bytes: the block at location 13 starts at `0x6800`. The table gives the dictionary; location 22 is
not in it.

| Location | Block              | Location | Block               |
|--------------------|-----------------------------------------------|--------------------|--------------------------------------------------------------|
| 0        | `ctrl`             | 21       | `gpio`              |
| 1        | `uart`             | 23       | `header`            |
| 2        | `icap`             | 24       | `ad9361`            |
| 3        | `flash_cs_n`       | 25       | `crossbar`          |
| 4        | `xadc`             | 26       | `sata_identify`     |
| 5        | `dna`              | 27       | `sata_mem2sector`   |
| 6        | `flash`            | 28       | `sata_sector2mem`   |
| 7        | `leds`             | 29       | `sata_rx_streamer`  |
| 8        | `identifier_mem`   | 30       | `clk_measurement`   |
| 9        | `timer0`           | 31       | `analyzer`          |
| 10       | `pcie_phy`         | 32       | `sata_tx_streamer`  |
| 11       | `pcie_msi`         | 33       | `txrx_loopback`     |
| 12       | `pcie_dma0`        | 34       | `ptm_requester`     |
| 13       | `capability`       | 35       | `eth_rx_mode`       |
| 14       | `eth_phy`          | 36       | `vrt_streamer`      |
| 15       | `eth_rx_streamer`  | 37       | `eth_ptp`           |
| 16       | `eth_tx_streamer`  | 38       | `ptp_discipline`    |
| 17       | `time_gen`         | 39       | `ptp_identity`      |
| 18       | `sata_phy`         | 40       | `clk10_discipline`  |
| 19       | `sata_core`        | 41       | `ptp_clk10_discipline` |
| 20       | `si5351`           |          |                     |

The map is a superset. A block of the table keeps its location whether the option that enables it
is set or not, so it starts at the same address in every bitstream that contains it. The software
reads the [capability registers](#soc-capability) to learn which optional blocks are present.

### Locations that move {#soc-csr-moving}

Three kinds of blocks are not in the dictionary. LiteX gives them the first free location, which
changes with the options, so their registers do not have the same address in every bitstream.

| Block                                | PCIe or Ethernet build | With SATA or `--pcie-dmas 2` |
|-----------------------------------------------------|--------------------------------------|--------------------------------------------|
| `pps_in`                             | Location 22, `0xb000`  | Location 42, `0x15000`       |
| `main`, the SATA streamer control    | Absent                 | Location 22 with SATA        |
| `pcie_dma1` and the following        | Absent                 | Location 22 and the next free ones |

These addresses were read on elaborations of the script with PCIe alone, Ethernet alone, PCIe
with two DMA channels and SATA. In a White Rabbit build the `pps_in` registers are absent.

The offsets inside a block can move too. The rule for a new register is to append it after the
existing ones, so that the older registers keep their offsets: the timed TX registers of the
header follow it. The `ad9361` block breaks it in two places.

- The three `ad9361_active_*` registers are declared before the SPI registers. `ad9361_spi_control`
  is at `0xc01c` in every elaboration, and at `0xc010` in the `csr.h` committed in
  `litex_m2sdr/software/kernel/`, which does not contain them.
- The oversampling PHY has more registers than the standard PHY, so the registers that follow it in
  the block are shifted in the `--with-rfic-oversampling` build.

> The software must be built against the `csr.h` of the bitstream it drives. The target script
> writes this file at each run, so a bitstream and a software built from the same run agree.
> Software built from a clean checkout does not match a bitstream built from the same sources: it
> writes the SPI of the AD9361 at the wrong addresses.

The register bus is 15 bits wide in words, which gives room for 64 locations. The registers of
every block are listed in [Registers](#registers).

## Capability registers {#soc-capability}

The software cannot guess what a bitstream contains, so the gateware tells it. A small block of
read-only registers holds the build configuration as constants (`Capability`).

| Register                 | Content                                                            |
|------------------------------------------------------------------------|------------------------------------------------------------------------------|
| `capability_api_version` | Version of the register interface, major in bits 31:16, minor in bits 15:0. The value is `0x00010000`, version 1.0 |
| `capability_features`    | One bit per optional feature, see the next table                   |
| `capability_pcie_config` | PCIe speed (bits 1:0), lanes (bits 3:2), PTM (bit 4)               |
| `capability_eth_config`  | Ethernet speed (bits 1:0), PTP (bit 2), PTP RFIC clock (bit 3)     |
| `capability_sata_config` | SATA generation (bits 1:0), mode (bits 3:2)                        |
| `capability_board_info`  | Variant (bits 1:0), JTAGBone (bit 2), Ethernet SFP (bit 3), White Rabbit SFP (bit 4) |

The feature bits are the following.

| Bit | Field                | Set when                                              |
|-----------|--------------------------------------------------------|-----------------------------------------------------------------------------------|
| 0   | `pcie`               | `--with-pcie`                                         |
| 1   | `eth`                | `--with-eth`                                          |
| 2   | `sata`               | `--with-sata`                                         |
| 3   | `gpio`               | `--with-gpio`                                         |
| 4   | `wr`                 | `--with-white-rabbit`                                 |
| 5   | `jtagbone`           | JTAGBone present                                      |
| 6   | `eth_ptp`            | `--with-eth-ptp`                                      |
| 7   | `eth_ptp_rfic_clock` | `--with-eth-ptp-rfic-clock`                           |
| 8   | `timed_tx`           | Always, although timed TX only works on PCIe          |
| 9   | `rfic_oversampling`  | `--with-rfic-oversampling`                            |

The configuration registers are meaningful only when the matching feature bit is set. The PCIe
speed field is 0 for Gen1 and 1 for Gen2; the lanes field is 0, 1 or 2 for x1, x2 or x4.

The Ethernet speed field is 0, 1 or 2 for 1 Gbps, 2.5 Gbps or 5 Gbps. The SATA mode is always 2,
read and write.

The library reads these registers when it opens a board, and `m2sdr_util info` prints them. A
feature that is absent is reported as unsupported instead of failing on a missing register.

The `timed_tx` bit says that the registers of the gate exist, not that the gate can be used: see
[Contract with the host](#streaming-timed-tx-contract).

## Flash and multiboot {#soc-flash}

The configuration flash holds two bitstreams so that a failed update does not leave the board
without a working design. The platform is created with `build_multiboot=True` and every build
produces three images.

| File                        | Flash offset | Role                                                |
|-----------------------------------------------------------------------|-------------------------------|-----------------------------------------------|
| `<build>.bin`               | `0x000000`   | Single image, written by `--flash`                  |
| `<build>_fallback.bin`      | `0x000000`   | First image loaded; jumps to the operational image  |
| `<build>_operational.bin`   | `0x800000`   | Image used in normal operation                      |

The fallback image contains the address of the operational image and the FPGA loads the
operational image from it. The two multiboot images are built with the configuration fallback
enabled and a configuration watchdog; the single image is not. When the operational image is
missing or corrupt, the FPGA returns to the fallback image, which holds the same design.

The image size is 8 MiB, `0x800000`, for the XC7A200T (`Platform.image_size`).

`--flash-multiboot` writes the two images over JTAG and verifies them. The host can also update
the operational image through the host link, without a JTAG cable, because the flash is
reachable from the registers:

- The SPI flash core runs the flash at 25 MHz (`flash`), with a separate register for the chip
  select (`flash_cs_n`).
- The configuration access port can restart the FPGA configuration from a register write
  (`icap`, with its reload command), so the new image is loaded without a power cycle.

The image size is also exported to the software as the constant `FLASH_IMAGE_SIZE`. The update
procedure for a release image is in [Getting started](#getting-started).

All bitstreams are compressed, use the four-wire SPI mode at a configuration rate of 33 MHz,
except the fallback image which uses the single-wire mode, and pull the unused pins down.

## Monitoring and identity {#soc-xadc}

Two small blocks of the FPGA are exposed as they are.

The system monitor reads the die temperature and the supply voltages of the FPGA (`xadc`). The
registers hold raw 12-bit values that the utilities convert.

| Register            | Quantity                       |
|------------------------------------------------------------------------------------|------------------------------------------------------------------|
| `xadc_temperature`  | Die temperature                |
| `xadc_vccint`       | Core supply                    |
| `xadc_vccaux`       | Auxiliary supply               |
| `xadc_vccbram`      | Block RAM supply               |

The bitstream also enables the over-temperature shutdown of the FPGA, so a board without enough
cooling powers its FPGA down instead of overheating. [Hardware](#hardware) gives the thermal
recommendations.

The device identifier of the FPGA is read through `dna_id`. It is unique to each FPGA: the
utilities use it as a serial number, and the PTP core derives its clock identity from it.

## Status LED {#soc-led}

The board has one user LED and the gateware uses its brightness to show three things at once:
whether the design is ready, the PPS, and the traffic (`StatusLed`).

| Layer      | Condition                                        | Effect                                      |
|----------------------------|----------------------------------------------------------------------|-----------------------------------------------------|
| Not ready  | Board time stopped or zero, or no host link up   | Double pulse every second                   |
| Ready      | Board time running, one host link up             | Slow, dim breathing                         |
| PPS        | Pulse of the internal PPS generator              | Bright flash of 80 ms                       |
| Activity   | Samples moving on the TX or RX stream            | Bright flashes of 10 ms, every 40 ms        |

The brightest layer wins at each instant. The host link condition only applies to the links the
bitstream contains, PCIe and Ethernet: a build without any of them is ready as soon as the board
time runs.

The LED can also be driven by hand for tests, with the three registers below. The script
`scripts/test_led.py` uses them.

| Register        | Role                                                       |
|--------------------------------|------------------------------------------------------------------|
| `leds_control`  | Manual mode: every input is replaced by a register bit     |
| `leds_pulse`    | Injects single events                                      |
| `leds_status`   | Brightness level                                           |

## GPIO {#soc-gpio}

The GPIO block gives the host digital inputs and outputs that are sampled with the RFIC clock,
so they line up with the samples. It is built with `--with-gpio` and uses the two test points
TP1 and TP2 of the board, which is why the option excludes a PPS input on these test points.

The register `gpio_control` has three bits.

| Bit | Field      | Role                                                                    |
|-----------|-----------------------|----------------------------------------------------------------------------------|
| 0   | `enable`   | Inserts the GPIO bits in the RX samples and extracts them from the TX samples |
| 1   | `source`   | Selects what drives the pins, see the next table                        |
| 2   | `loopback` | Returns the outputs to the inputs inside the FPGA, for tests            |

The block has two sources.

| `source` | Mode             | Outputs come from                 | Inputs go to                      |
|------------------------|----------------------------|------------------------------------------------------|---------------------------------------------|
| 0        | Stream           | Spare bits of the TX samples      | Spare bits of the RX samples      |
| 1        | Register         | `gpio__o` and `gpio_oe`           | `gpio__i`                         |

The stream mode uses the fact that a sample of the AD9361 has 12 bits and travels in a 16-bit
field. In the TX stream, bits 15:12 of the I field carry four output values and bits 15:12 of the
Q field carry the four output enables.

In the RX stream, bits 15:12 of the I field carry the four input values and bits 15:12 of the Q
field are zero.

Each channel slot of a sample word has its own four bits. The first slot is driven on the first
edge of the RFIC clock and the second slot on the second edge, with double data rate input/output
cells.

The host must mask bits 15:12 of the RX samples whenever `enable` is set, in both sources, since
they no longer extend the sign. In register mode the RX samples carry the GPIO inputs too, and
the pins are driven even when `enable` is 0.

The logic handles four bits; the platform connects two pins, so bits 0 and 1 reach TP1 and TP2.
The stream mode needs the SC16 format: the SC8 format has no spare bits.

The GPIO stages work on one group of samples, 64 bits, in the `rfic` domain, between the PHY and
the converters of the oversampling build. The stream mode therefore applies to each group in both
builds.

## Probes {#soc-probes}

A LiteScope logic analyzer can be added to a build to capture internal signals. The probe
options are mutually exclusive: one bitstream has at most one analyzer, at location 31 of the
register map.

| Option                     | Signals                                              | Clock domain  | Depth |
|------------------------------------------------------------------|--------------------------------------|--------------------------------|--------------|
| `--with-pcie-probe`        | PCIe reset, reference clock, link rate, width, LTSSM | `sys`         | 4096  |
| `--with-pcie-dma-probe`    | PPS, DMA sink and source, synchronizer, header resets | `sys`        | 1024  |
| `--with-si5351-i2c-probe`  | I2C pins, master streams, sequencer                  | `sys`         | 4096  |
| `--with-eth-phy-rx-probe`  | Two raw 10-bit RX symbols of the Ethernet PHY        | `eth_rx_half` | 4096  |
| `--with-eth-tx-probe`      | Stream entering the Ethernet TX streamer             | `sys`         | 1024  |
| `--with-ad9361-spi-probe`  | SPI pins of the AD9361                               | `sys`         | 4096  |
| `--with-ad9361-data-probe` | PHY TX and RX samples, PRBS synchronisation          | `rfic`        | 4096  |

The signal list of a build is written to `test/analyzer.csv`. `BaseSoC` also has a method for a
probe on the SATA DMA bus, `add_pcie_slave_probe()`, which has no command line option. The use
of the probes is described in [Debugging](#debugging).

## Build name and timing {#soc-build}

The build directory is named after the options, so that several configurations can live side by
side under `build/`. The name starts with `litex_m2sdr_<variant>` and receives one suffix per
option, for example `_pcie_x1`, `_eth`, `_eth_2500basex`, `_ptp`, `_sata`, `_white_rabbit` or
`_rfic_oversampling`.

The 125 MHz system clock is the critical domain of the design. The script asks Vivado for the
timing-oriented placement and routing directives and for the physical optimisation passes after
placement and after routing. Synthesis directives are left at their defaults.

> A bitstream that does not meet timing must not be loaded. A marginal design can sample the
> AD9361 interface wrongly, and the symptom is an RFIC that looks dead.

The constraints follow one pattern. The board clocks get a period in the platform
(`Platform.do_finalize()`), the RFIC clock gets the period that matches the build, and each pair
of unrelated clocks is declared asynchronous with a guarded command that does nothing when one of
the clocks does not exist in the build.

Slow pins such as SPI, I2C and the control pins of the AD9361 are excluded from timing.

The period of the RFIC clock depends on the configuration (`get_rfic_clk_freq()`).

| Build                          | RFIC clock constraint | Sizing case                    |
|----------------------------------------------------------|--------------------------------------------|------------------------------------------------|
| PCIe or SATA, standard         | 245.76 MHz            | 2T2R at 61.44 MSPS             |
| PCIe, oversampling             | 491.52 MHz            | 2T2R at 122.88 MSPS            |
| Ethernet 1000BASE-X            | 122.88 MHz            | 2T2R at 30.72 MSPS, SC8        |
| Ethernet 2500BASE-X, 5GBASE-R  | 245.76 MHz            | 2T2R at 61.44 MSPS, SC8        |

The Ethernet builds are limited by the rate of the link, not by the RFIC, so their constraint is
relaxed to the highest sample rate the link can carry. The function returns the Ethernet value
before it looks at the oversampling option, one reason why the two
[must not be combined](#soc-rules).

## In the code {#soc-code}

| File                                  | Content                                                     |
|-------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| `litex_m2sdr.py`                      | `CRG`, `BaseSoC`, probes, `main()` with the options         |
| `litex_m2sdr_platform.py`             | Pins, M.2 connector, bitstream settings, programmer         |
| `litex_m2sdr/gateware/capability.py`  | `Capability`                                                |
| `litex_m2sdr/gateware/led.py`         | `StatusLed` and its effect helpers                          |
| `litex_m2sdr/gateware/gpio.py`        | `GPIO`, `GPIORXPacker`, `GPIOTXUnpacker`                    |
| `test/test_cli_args.py`               | Option rules, clock policy, lane placement                  |
| `test/test_capability.py`, `test_led.py`, `test_gpio.py` | Unit tests of the blocks                 |

## Where to go next {#soc-next}

[The RFIC interface](#rfic) follows the samples to the pins of the AD9361, and
[Streaming](#streaming) follows them to the host.
