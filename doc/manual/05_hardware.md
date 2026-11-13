```{=latex}
\part{The board}
```

```{=html}
<h1 class="part">Part II. The board</h1>
```

# Hardware {#hardware}

This chapter describes the board as an FPGA designer and an integrator see it: the main
components, the M.2 connector and its pins, the I/O voltages, the clocks, the LED, the JTAG access
and the thermal constraints. Read it before wiring a PPS signal, designing a carrier or changing a
pin of the gateware.

The gateware and the software of the project are open source. The hardware design of the board is
not published: this chapter uses the facts of the platform file `litex_m2sdr_platform.py`, of the
repository and of the vendor documents kept in `doc/ad9361/` and `doc/si5351/`. Pin and resistor
references are those of the revision R02 of the board.

## At a glance {#hw-glance}

| Item | Value |
|-----------------------|--------------------------------------------------------------------|
| Form factor | M.2 2280 Key M |
| FPGA | AMD Artix-7 XC7A200T, SBG484 package, speed grade -3 |
| RFIC | Analog Devices AD9361, 2T2R, 70 MHz to 6 GHz, 12-bit converters |
| Clock generator | Skyworks SI5351C, or SI5351B on earlier boards |
| Oscillator | 100 MHz, to the FPGA |
| Transceivers | 4 GTP lanes on the M.2 connector, with the M.2 reference clock |
| Configuration | SPI flash, two multiboot images of 8 MiB |
| RF connectors | TX1, TX2, RX1, RX2 |
| Timing inputs | One uFL connector for a 10 MHz reference, PPS input on the M.2 connector |
| Debug | JTAG connector, 2 test points, 1 status LED |

The figure below is the annotated view of the board published with the project. It locates the
components named in this chapter.

![The LiteX-M2SDR board, revision R02, with its main components and connectors
annotated.](m2sdr_annotated.png){width=95%}

## Block diagram {#hw-block}

The figure below shows how the components connect. The FPGA sits between the M.2 connector and the
RFIC: every sample crosses it, and every host link is built in it from the four transceiver lanes
of the connector.

![Block diagram of the board. The cyan path is the M.2 connector and its four transceiver
lanes, which the gateware turns into PCIe, Ethernet or SATA.](hw_block.svg)

The RFIC exchanges samples with the FPGA over an LVDS data port and is configured over SPI.
[SDR in a nutshell](#sdr-primer) explains its signal chain, and
[The RFIC interface](#rfic) the gateware that drives it.

The clock generator produces the reference clock of the RFIC and two clocks for the FPGA. The
100 MHz oscillator clocks the FPGA logic. The SPI flash holds the bitstreams. Each of these blocks
has a section below.

## FPGA {#hw-fpga}

The FPGA is an XC7A200T in the SBG484 package. The platform file builds for the part
`xc7a200tsbg484-3` with Vivado.

Its four GTP transceivers are wired to the four lanes of the M.2 connector. They are plain
serial transceivers: connected to the hardened PCIe block of the FPGA they make a PCIe link, and
connected to other LiteX cores they make an Ethernet or a SATA link. [Host links](#host-links)
describes each use and the shared transceiver PLL.

The base design uses a fraction of the logic of the FPGA and leaves room for user processing.
[Building the gateware](#building-gateware) explains how to add it.

## RFIC interface {#hw-rfic}

The table below lists the signals between the FPGA and the AD9361, as the platform file declares
them in the `ad9361_rfic` and `ad9361_spi` resources.

| Signals | Count | Direction | Role |
|------------------------|----------|-----------|------------------------------------------------|
| `rx_clk` | 1 pair | To FPGA | Data clock of the RFIC (DATA_CLK) |
| `rx_frame`, `rx_data` | 7 pairs | To FPGA | RX frame and 6 data pairs |
| `tx_clk` | 1 pair | To RFIC | Feedback clock (FB_CLK) |
| `tx_frame`, `tx_data` | 7 pairs | To RFIC | TX frame and 6 data pairs |
| `rst_n`, `enable` | 2 | To RFIC | Reset and enable |
| `txnrx`, `en_agc` | 2 | To RFIC | TX/RX control and AGC enable |
| `ctrl` | 4 | To RFIC | Control inputs of the RFIC |
| `stat` | 8 | To FPGA | Control outputs of the RFIC |
| SPI | 4 | Both | Register access (`clk`, `cs_n`, `mosi`, `miso`) |

The data port is differential and source synchronous. The RFIC provides the clock of the RX
direction; the FPGA returns a clock with the TX data. The platform constrains the data clock at
245.76 MHz.

The platform file declares these pins with the `LVDS_25` and `LVCMOS25` I/O standards, and
enables the internal differential termination on the RX pairs. The comment at the top of the
same file gives 1.8 V as the supply of their banks, 34 and 35.

## M.2 connector {#hw-m2}

The board is an M.2 2280 Key M module. It fits M-keyed M.2 sockets that carry PCIe, carrier cards
for such modules and the baseboard described in [Carriers and hosts](#carriers).

### Transceiver lanes {#hw-m2-lanes}

The connector carries four transceiver lanes and one reference clock. The table below gives the
M.2 pins and the FPGA pins of each lane, with the names of the `M2` connector of the platform
file. The FPGA drives the PET pairs and receives the PER pairs.

| Lane | PET, M.2 | PET, FPGA | PER, M.2 | PER, FPGA | GTP channel |
|-----|---------|---------|---------|---------|------------------------------|
| 0 | 41, 43 | C7, D7 | 47, 49 | C9, D9 | `GTPE2_CHANNEL_X0Y7` |
| 1 | 29, 31 | C5, D5 | 35, 37 | C11, D11 | `GTPE2_CHANNEL_X0Y5` |
| 2 | 17, 19 | A6, B6 | 23, 25 | A10, B10 | `GTPE2_CHANNEL_X0Y6` |
| 3 | 5, 7 | A4, B4 | 11, 13 | A8, B8 | `GTPE2_CHANNEL_X0Y4` |

The reference clock of the connector arrives on M.2 pins 53 and 55, FPGA pins E6 and F6.

Lanes 1 and 2 do not follow the channel order the PCIe block expects. A PCIe build with more than
one lane therefore places each lane explicitly, from the list `M2_PCIE_GT_LOCS` of the platform
file.

Some pairs are swapped in polarity on the board. The comments of the platform file record each
swap.

### Sideband signals {#hw-m2-sideband}

Besides the lanes, the connector carries low-speed signals. Some are standard M.2 signals. Others
use pins that a standard M.2 socket leaves unconnected: a carrier must route them on purpose.

| Signal | M.2 pin | FPGA pin | Bank | Notes |
|----------------------|---------|----------|------|------------------------------------------|
| `PPS_IN` | 22 | K18 | 15 | PPS input, default of `--pps-input` |
| `PPS_OUT` | 24 | Y18 | 14 | Routed to the FPGA, not driven |
| `Synchro_GPIO1` | 28 | A19 | 16 | Routed to the FPGA |
| `Synchro_GPIO2` | 30 | A18 | 16 | Routed to the FPGA |
| `Synchro_GPIO3` | 32 | A21 | 16 | Routed to the FPGA |
| `Synchro_GPIO4` | 34 | A20 | 16 | Routed to the FPGA |
| `Synchro_GPIO5` | 36 | B20 | 16 | Routed to the FPGA |
| `SMB_CLK` | 40 | A13 | 16 | Through R82, not mounted by default |
| `SMB_DATA` | 42 | A14 | 16 | Through R83, not mounted by default |
| `ALERT#` | 44 | none | none | Not routed to the FPGA |
| `PERST#` | 50 | A15 | 16 | PCIe reset |
| `CLKREQ#` | 52 | none | none | Pulled up to the 3.3 V of the connector |
| `PEWAKE#` | 54 | B16 | 16 | Through R88, not mounted by default |
| `SUSCLK` | 68 | B17 | 16 | Through R84, 0 ohm |
| `PEDET`, `PRESENT` | 69 | A16 | 16 | Through R85, 0 ohm |
| `LED#` | 10 | none | none | Not connected |

The platform file names the pins 22 to 36 `NC22` to `NC36`, after their M.2 pin number. The
gateware uses `PPS_IN`, `PERST#` and, in a White Rabbit build, the two SMBus pins as the I2C bus
of the SFP module.

`PPS_OUT` and the five `Synchro_GPIO` pins are declared in the connector of the platform file;
no block of the SoC uses them. The gateware therefore has no PPS output: its internal PPS
reaches no pin.

`CLKREQ#` is pulled up with a 4.7 kΩ resistor (R59) and never driven by the FPGA.

## I/O voltages {#hw-io-voltages}

The board does not use a single I/O voltage. The table below gives the supply of each FPGA bank
and what the bank connects to.

| Banks | Voltage | Connected to |
|------------------|---------|---------------------------------------------------------------|
| 13, 14, 15, 16 | 3.3 V | M.2 sideband, PPS, test points, LED, clock generator, flash |
| 34, 35 | 1.8 V | RFIC data port, RFIC control and SPI |

These voltages are those of the comment at the top of the platform file. Two more outputs of the
clock generator, `si5351_clk2` and `si5351_clk3`, are declared on the RFIC banks; the SoC does
not use them.

Every sideband signal of the table above is therefore a 3.3 V signal on the board side. The lanes
and the reference clock are transceiver signals, not single-ended I/Os. The JTAG and
configuration pins of the FPGA are 3.3 V too.

> **Warning**
> The PPS input and the test points accept ground-referenced 3.3 V logic only. Never apply 5 V.

The voltage on the board side and the voltage a carrier expects on the connector are two
different things. The baseboard, for example, runs the SMBus pins of its M.2 socket at 1.8 V and
translates them to 3.3 V for the SFP modules. Check both sides before connecting a sideband pin.

## Test points and PPS input {#hw-pps}

Two test points reach the FPGA, and the annotated view shows a ground test point next to them.

| Test point | Signal | FPGA pin | Bank |
|------------|---------------|----------|------|
| TP1 | `FPGA_GPIO0` | E22 | 16 |
| TP2 | `FPGA_GPIO1` | D22 | 16 |

The gateware gives them one of two roles. With `--with-gpio` they are the two pins of the GPIO
core. With `--pps-input=tp1` or `--pps-input=tp2` one of them is the PPS input. The two options
exclude each other: the build stops when both are given.

The default PPS input is M.2 pin 22 (`--pps-input=m2`). A standard M.2 socket leaves this pin
unconnected, so it needs a carrier or an adapter that routes it. A test point is the easier
wiring on a bench:

```sh
./litex_m2sdr.py --pps-input=tp1 --build
```

The selected input is resynchronised to the system clock. Software reads its level, its rising
edges and an edge counter in the `pps_in` registers.

The gateware only monitors this input: the board time is not aligned on it.
[PPS input](#timing-pps-in) gives the registers and what is left to the software.

## Clocking {#hw-clocking}

The board has two clock sources of its own, the 100 MHz oscillator and the crystal of the clock
generator, and one external input. The figure below shows the tree for a board with the SI5351C.

![Clocking tree with the SI5351C. The cyan path is the reference clock of the RFIC; the 10 MHz
of the clock generator comes from the FPGA or, through the FPGA, from the uFL
connector.](hw_clocking.svg)

The 100 MHz oscillator enters the FPGA on pin C18. A PLL makes the system clock from it:
125 MHz, or 100 MHz in a 1000BASE-X or 5GBASE-R Ethernet build. It also makes the 200 MHz clock
of the I/O delay elements.

Except in a White Rabbit build, an MMCM makes a 10 MHz clock whose phase the gateware can shift
in fine steps.

The gateware configures the clock generator over I2C at start-up, and the software can
reconfigure it. The default configuration gives three clocks from the crystal.

| Clock | Frequency | Destination |
|------------------|------------|----------------------------------------------|
| `si5351_clk0` | 38.4 MHz | FPGA pin J19, frequency measurement |
| `si5351_clk1` | 100 MHz | FPGA pin E19, clock of the board time |
| RFIC reference | 38.4 MHz | Reference clock of the AD9361 |

The software can change the RFIC reference: `m2sdr_rf --refclk-freq` takes 38.4 MHz or 40 MHz as
typical values. The RFIC derives its sample clock and its local oscillators from this reference,
and returns the data clock to the FPGA.

### SI5351C and SI5351B versions {#hw-si5351-versions}

Boards carry one of two clock generators. The SI5351C version has a clock input and can lock to
a 10 MHz reference. The SI5351B version has a voltage-controlled crystal oscillator that the FPGA
tunes. The SI5351C version covers the uses of the SI5351B version; the gateware and the software
support both.

| Pin | SI5351C version | SI5351B version |
|----------------------------|--------------------------------|-----------------------------------|
| `ssen_clkin`, FPGA W22 | 10 MHz clock input (CLKIN) | Spread spectrum enable |
| `pwm`, FPGA W19 | Held high | PWM tuning of the VCXO |

On the SI5351C version, a clock multiplexer in the FPGA selects the 10 MHz sent to the clock
generator. It is either the 10 MHz the FPGA makes from its oscillator, or the signal of the
uFL connector that the annotated view labels ClkIn/Out and SyncIn/Out (CN6). This signal enters
the FPGA on pin V13.

The software chooses the reference at run time with `m2sdr_rf --sync`.

| `--sync` | Reference of the clock generator |
|-------------|---------------------------------------------------------|
| `internal` | Crystal of the clock generator |
| `external` | 10 MHz of the uFL connector, SI5351C version |
| `fpga` | 10 MHz generated by the FPGA, SI5351C version |

The 10 MHz of the FPGA matters because the gateware can steer its phase: this is how an Ethernet
PTP build disciplines the reference clock of the RFIC. [Time synchronisation](#timing)
describes the clock generator configurations, the discipline loops and the registers.

## User LED {#hw-led}

The platform exposes one monochrome LED (`user_led`, FPGA pin AB15, net `FPGA_LED2`). The
gateware layers several meanings on it instead of a plain on or off state. The annotated view
labels two LEDs, LD1 and LD2: the platform file declares this one only, and the gateware drives
no other.

| State | LED |
|----------------------------------|-------------------------------------------|
| Not ready | Double heartbeat |
| Ready, idle | Gentle breathing at low brightness |
| Each second of the board time | Short bright pulse over the base state |
| RX or TX stream activity | Bright pulses over the base state |

The pulse of each second comes from the internal PPS, made from the board time. The LED does not
show the external PPS input. The activity is that of the sample streams of PCIe, Ethernet and
SATA.

"Not ready" means that the board time is stopped or not set, or that the build has a host link
and none is up. Only PCIe and Ethernet count as host links here. A build without them shows the
state of the board time alone.

The software can read the LED state and override it with the `led-status`, `led-control`,
`led-pulse` and `led-release` commands of `m2sdr_util`. The LED core is `StatusLed` in
`litex_m2sdr/gateware/led.py`.

## Configuration flash and multiboot {#hw-flash}

The FPGA configures itself from an SPI flash. The flash holds two bitstreams, each in a slot of
8 MiB.

| Slot | Flash offset | Role |
|--------------|---------------|-------------------------------------------------------|
| Fallback | `0x00000000` | Loaded first, jumps to the operational image |
| Operational | `0x00800000` | Normal image, updated in the field |

When the operational image is missing or corrupted, the FPGA falls back to the first slot and the
board stays reachable. This makes an update over the host link safe.
[Flashing a release image](#gs-flash) gives the procedure over JTAG, and
[Building the gateware](#building-gateware) the update over PCIe.

## JTAG {#hw-jtag}

The board has a 6-pin JTAG connector for the FPGA. The annotated view gives its pin order: VCC
(3.3 V), TDI, TMS, TDO, TCK, GND. It needs a USB JTAG probe, either an external one or the one of
the baseboard.

The platform loads and flashes the FPGA with openFPGALoader. It detects the FTDI probe connected
to the host and picks the matching cable.

| FTDI chip | openFPGALoader cable |
|-----------|-----------------------|
| FT4232 | `ft4232` |
| FT2232 | `ft2232` |
| FT232 | `digilent_hs2` |

JTAG also carries a host bridge. Every build includes JTAGBone unless `--without-jtagbone` is
given, so the registers stay reachable when no host link works. [Debugging](#debugging) shows how
to use it.

## Power and thermal {#hw-thermal}

The M.2 connector supplies the board; an M.2 Key M socket provides 3.3 V only.

The FPGA and the RFIC are on the same side of the board and both dissipate heat in a small area.
A heatsink on each of them and some air flow are recommended; the photographs of
[Carriers and hosts](#carriers) show boards fitted this way.

The public issue tracker of the
repository records that a PCIe Gen2 x1 link runs cooler than a Gen2 x4 link.

Three mechanisms help to watch the temperature:

- The FPGA measures its die temperature and its supply voltages (`S7SystemMonitor`).
- The RFIC has its own temperature sensor.
- The bitstream enables the over-temperature shutdown of the FPGA
  (`BITSTREAM.CONFIG.OVERTEMPPOWERDOWN`).

`m2sdr_util info` prints both temperatures and the FPGA voltages. Check them after a few minutes
of streaming in the final enclosure, above all in a closed case.

## In the code {#hw-code}

| File | Content |
|----------------------------------------------|----------------------------------------------|
| `litex_m2sdr_platform.py` | Pins, I/O standards, M.2 connector, bitstream options |
| `litex_m2sdr.py` | Clock generation (`CRG`), use of each resource |
| `litex_m2sdr/gateware/si5351.py` | Clock generator control and default configuration |
| `litex_m2sdr/gateware/led.py` | Status LED |
| `litex_m2sdr/gateware/pps.py` | PPS input and generator |
| `doc/ad9361/`, `doc/si5351/` | Vendor datasheets and application notes |

Where to go next: [Carriers and hosts](#carriers) for what the board plugs into,
[The SoC](#soc) for the gateware built on these pins, [Getting started](#getting-started) to use
the board.
