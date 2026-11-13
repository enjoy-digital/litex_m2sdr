```{=latex}
\mainmatter
\part{Understand}
```

```{=html}
<h1 class="part">Part I. Understand</h1>
```

# Introduction {#intro}

[]{#introduction}This chapter says what LiteX-M2SDR is, which choices shaped it, what the
repository contains and what works today. It ends with a map of the manual. Every reader should
start here.

## What the board is {#intro-what}

LiteX-M2SDR is a software-defined radio (SDR) in the size of an M.2 2280 solid-state drive. It
pairs an Analog Devices AD9361 RFIC with an AMD/Xilinx Artix-7 XC7A200T FPGA. Plugged in an M.2
slot with antennas connected, it streams and records radio samples with the tools of the project
or with any SoapySDR application.

| Item | Value |
|--------------------|------------------------------------------------------------------------|
| Form factor | M.2 2280, Key M |
| RFIC | AD9361: two transmitters and two receivers (2T2R), 70 MHz to 6 GHz, 12-bit converters |
| Sample rate | Up to 61.44 MSPS in 2T2R |
| FPGA | Artix-7 XC7A200T, four transceivers on the M.2 connector |
| Clock generator | SI5351, from its crystal or from an external 10 MHz reference |
| Host links | PCIe Gen2 x1, x2 or x4; Ethernet 1000BASE-X or 2500BASE-X; USB3 (in development) |
| Storage | SATA record and replay on an SSD |
| Time synchronisation | 10 MHz reference, PCIe PTM, Ethernet PTP; White Rabbit (Experimental) |
| Host software | Linux kernel driver, `libm2sdr` C library, utilities, SoapySDR driver |
| License | Gateware and software: BSD-2-Clause |

The gateware and the software are open source. The hardware design of the board is not
published: the manual describes the board from its public interfaces, in [Hardware](#hardware).

## The approach {#intro-approach}

LiteX-M2SDR starts from a simple bet. The radio chip is a known one, the AD9361, and the
novelty is everything around it. The gateware reuses the LiteX cores as they are, so the same
M.2 board talks to a host over PCIe, Ethernet or USB3 and records on a SATA SSD.

The effort saved goes into features that are rare at this size, such as time synchronisation
and hardware timed TX. What the board needs and the cores lack is added to the cores, so the
board also improves LiteX.

The five sections below take these ideas one by one. The rest of the manual shows how each of
them turns into gateware and software.

### A well-known RFIC, and innovation around it {#intro-approach-rfic}

The AD9361 is one of the most widely used RFICs of the SDR world. Its behaviour, its limits and
its driver are known, and many applications already support it. For a first SDR project, the
radio itself is therefore not where the risk and the novelty should be.

The project keeps this proven radio and puts its effort around it: the links to the host, the
handling of time, and a deeper use of the chip itself. After years of designs with this chip,
the project holds that it has capabilities that open-source designs have not used.

The RF front end on the board is minimal on purpose. Filters, amplifiers and switches depend on
the application, and they are easier to add outside the board than to remove from it.

### Maximum reuse of the LiteX infrastructure {#intro-approach-reuse}

The gateware is a LiteX design. Almost everything that is not specific to the radio comes from
an existing LiteX core, used as it is. The table below lists them; [LiteX in a
nutshell](#litex-primer) explains each one.

| Need | LiteX core |
|----------------------------------|----------------------------------------------------------|
| SoC, bus, registers, build | LiteX |
| PCIe host link, DMA, Linux driver | LitePCIe |
| Ethernet host link, UDP, PTP | LiteEth |
| SATA storage | LiteSATA |
| USB3 host link (in development) | USB3-PIPE |
| I2C to the clock generator | LiteI2C |
| On-chip logic analyser | LiteScope |
| Access to the registers from the host | Host bridges of LiteX, over PCIe, Ethernet and JTAG |
| White Rabbit core integration | LiteX-WR-NIC |

What the project adds is small and focused: the interface to the RFIC, the board time and its
timestamps, the clock control, and the glue between the streams. This keeps the specific code
short enough to be read, and every improvement of a core benefits the board without any porting.

The transceivers can also carry links between boards with LiteICLink, the LiteX core for raw
serial links. The design of the repository does not instantiate it.

### One M.2 board, several links {#intro-approach-links}

An M.2 Key M connector carries four high-speed serial lanes. In a computer they carry PCIe, but
on the FPGA side they are four general-purpose transceivers. The same board can therefore speak
another protocol on the same pins when its carrier routes them elsewhere.

The figure below shows the result. One datapath handles the samples, and one LiteX core per
link carries them: PCIe in the M.2 slot of a host, Ethernet through an SFP module, USB3 through
an adapter in an SFP cage, and SATA to an SSD.

![The same board over its links. The SDR datapath is common; a stream crossbar connects it to
one LiteX core per link. Ethernet, USB3 and SATA need the baseboard; USB3 is in
development.](intro_links.svg)

The host software follows the same idea. The library hides the host link behind one device
identifier, so an application written for PCIe runs over Ethernet with a different `<device>`.

| Link | Carrier | Use |
|-----------|-----------------------------|-------------------------------------------------------|
| PCIe | M.2 slot, or the baseboard | Highest sample rates, lowest latency, several boards in one PC |
| Ethernet | Baseboard, SFP cage | Remote radio head, distributed receivers, PTP |
| USB3 | Baseboard, SFP to USB3 adapter | A host reached through a USB port (in development) |
| SATA | Baseboard, SATA connector | Recording and replaying samples without a host in the path |

The transceivers share two PLLs, so a bitstream holds at most two of these protocols at once.
[Host links](#host-links) gives the combinations, and [Carriers and hosts](#carriers) says what
each carrier provides.

### Innovative features {#intro-approach-features}

With the radio and the infrastructure settled, the project can spend its effort on features that
are rare on a board of this size.

- **Time synchronisation.** The board keeps a 64-bit time in nanoseconds. Its clocks can lock
  to an external 10 MHz reference, and its time can follow the clock of the host through PCIe
  PTM or a network clock through Ethernet PTP. [Time in a nutshell](#timing-primer) introduces
  these mechanisms.
- **Hardware timed TX.** Each transmit buffer can carry the time at which it must be on the air,
  and the FPGA holds it until that time (see [Streaming](#streaming)).
- **Several boards together.** Boards on a bifurcated M.2 carrier card share one PC, each on its
  own PCIe link, up to 8T8R with four boards.
- **Room for user logic.** The base design uses a fraction of the XC7A200T. A hook in the SoC
  lets a derived design insert its own processing between the RFIC and the host, without a fork.

Two more items are integrated with a narrower scope: a White Rabbit core, which is Experimental,
and a PPS input that the gateware monitors without aligning the board time on it. The
[status table](#intro-status) gives the state of every feature.

### A board that improves LiteX {#intro-approach-upstream}

The board is also a demanding test bench for the LiteX ecosystem. It runs the cores at their
limits, on many hosts, in the hands of many users. What is learnt on the board goes back to the
cores instead of staying in a private copy, so every LiteX user gets it.

The table below gives examples that can be checked in the public repositories.

| Core | Work that went upstream |
|-----------|------------------------------------------------------------------------------|
| LitePCIe | PTM support of the 7-series PHY, used by the PTM build of the board |
| LitePCIe | Kernel driver that stays safe when the link drops or the device is removed while files are open |
| LiteEth | 5GBASE-R on Artix-7 transceivers, with a bench on the Acorn Baseboard Mini |
| LiteEth | PTP bench on the Acorn Baseboard Mini |
| USB3-PIPE | External PLL option of the Artix-7 transceiver, to share the PLLs with PCIe, Ethernet or SATA |
| USB3-PIPE | Wishbone bridge on endpoint 0, the register access of the USB3 host link |
| LiteX | SFP module management in the Ethernet library of the BIOS, to switch modules to 5GBASE-R |

The same applies in the other direction: the project is a showcase of what the LiteX cores can
do together, and a complete example for anyone who builds a comparable design.

## What is in the repository {#intro-layout}

| Path | Content |
|----------------------------------------------------|--------------------------------------------------|
| `litex_m2sdr.py` | The SoC and its build options |
| `litex_m2sdr_platform.py` | Pins, I/O standards and clock constraints of the board |
| `litex_m2sdr/gateware/` | Gateware specific to the board: RFIC, time, PPS, clocks, streams |
| `litex_m2sdr/software/kernel/` | Linux kernel driver |
| `litex_m2sdr/software/user/` | `libm2sdr`, the utilities and their tests |
| `litex_m2sdr/software/soapysdr/` | SoapySDR driver |
| `litex_m2sdr/software/gnuradio/` | GNU Radio examples |
| `litex_m2sdr/doc/libm2sdr/` | Documentation and examples of the C library |
| `scripts/` | Board tests and helpers that need hardware |
| `test/` | Simulation tests of the gateware and tests of the scripts (pytest) |
| `release.py` | Build of the release images |
| `doc/` | This manual, its figures, dated notes and vendor documents |
| `CHANGELOG.md` | History of the project by quarter and by release |

## Status {#intro-status}

The table below is the only statement of status in the manual. The other chapters link to it.
The carrier columns say where a feature is available: in an M.2 slot (M.2) or on the LiteX
Acorn Baseboard Mini (BB).

| Feature | M.2 | BB | Status |
|--------------------------------------------|:----------:|:-----:|--------------------------|
| 2T2R TX and RX, 12-bit, up to 61.44 MSPS | yes | yes | Validated on hardware |
| 2T2R at 122.88 MSPS (oversampling mode) | yes | no | Experimental |
| PCIe streaming | x1, x2, x4 | x1 | Validated on hardware |
| Ethernet streaming, 1000BASE-X | no | yes | Validated on hardware |
| Ethernet streaming, 2500BASE-X | no | yes | Validated on hardware |
| Ethernet streaming, 5GBASE-R | no | yes | Experimental |
| VRT packets over Ethernet (RX) | no | yes | Simulation-tested |
| USB3 streaming | no | yes | In development |
| Hardware timed TX | yes | yes | Validated on hardware |
| SC16 and SC8 transport formats | yes | yes | Validated on hardware |
| BFP8 transport format | yes | yes | Experimental |
| External 10 MHz reference | yes | yes | Validated on hardware |
| PCIe PTM time synchronisation | x1 | x1 | Validated on hardware |
| Ethernet PTP time discipline | no | yes | Validated on hardware |
| PTP-disciplined RFIC reference | no | yes | Validated on hardware |
| White Rabbit | no | yes | Experimental |
| SATA record and replay | no | yes | Validated on hardware |
| Multiboot and remote update | yes | yes | Validated on hardware |
| GPIO | yes | yes | Validated on hardware |
| `libm2sdr`, utilities, SoapySDR driver | yes | yes | Validated on hardware |

A feature without a build option below is always included, or belongs to the host software.
The other features are selected when the bitstream is built; [Build options](#build-options)
lists every option.

| Feature | Build option |
|--------------------------------------------|----------------------------------------------|
| 2T2R at 122.88 MSPS (oversampling mode) | `--with-rfic-oversampling` |
| PCIe streaming | `--with-pcie`, `--pcie-lanes` |
| Ethernet streaming, 1000BASE-X | `--with-eth` |
| Ethernet streaming, 2500BASE-X | `--eth-phy=2500basex` |
| Ethernet streaming, 5GBASE-R | `--eth-phy=5000baser` |
| VRT packets over Ethernet (RX) | `--with-eth-vrt` |
| PCIe PTM time synchronisation | `--with-pcie-ptm` |
| Ethernet PTP time discipline | `--with-eth-ptp` |
| PTP-disciplined RFIC reference | `--with-eth-ptp-rfic-clock` |
| White Rabbit | `--with-white-rabbit` |
| SATA record and replay | `--with-sata` |
| GPIO | `--with-gpio` |

The Ethernet options other than `--with-eth` are given in addition to it.

Notes on the tables:

- The oversampling mode at 2T2R needs the bandwidth of PCIe Gen2 x2 or x4. It must not be
  combined with Ethernet or SATA (see [The RFIC interface](#rfic)).
- The 10 MHz reference and the PTP-disciplined RFIC reference need the SI5351C version of the
  board.
- PCIe PTM is used with PCIe Gen2 x1: the build rejects more than one lane. The repository
  records no measurement of the resulting offset.
- Hardware timed TX works over PCIe only.
- The PPS input is not a row of the table: the gateware only monitors it and reports its edges
  in registers. The board time is not aligned on it, and the board has no PPS output (see
  [Time synchronisation](#timing)).
- White Rabbit is integrated and its core runs on the board. The lock of its servo, its jitter
  and the accuracy of its PPS are not measured on this board, and its time is not connected to
  the timestamps of the samples. Do not enable it together with Ethernet.
- VRT packets are covered by simulation only, and the software of the repository does not
  select them (see [Host links](#host-links)).
- The BFP8 format is not available in the oversampling mode.
- The Ethernet builds limit the sample rate to what the link can carry (see
  [Host links](#host-links)).

The status words have one meaning throughout the manual:

| Status | Meaning |
|------------------------|------------------------------------------------------------------|
| Validated on hardware | Used on boards in normal operation. A number is given only where the repository holds a public record of the measurement |
| Experimental | Runs on hardware in a restricted setup; behaviour and interfaces may change |
| In development | Not merged in the main branch, or not tested on hardware; described to show the intent |
| Build-tested | Builds, with no record of a run on hardware |
| Simulation-tested | Covered by the simulation tests of `test/`, with no record of a run on hardware |

The measured results that the repository records are gathered in
[Performance and resources](#performance).

## License and credits {#intro-license}

The gateware and the software of LiteX-M2SDR are released under the BSD 2-Clause license by
Enjoy-Digital. Enjoy-Digital manages the project and develops the gateware and the software;
LambdaConcept designs the hardware.

The software includes the Analog Devices no-OS driver of the AD9361, and the White Rabbit build
relies on the White Rabbit core of CERN through LiteX-WR-NIC. The contributors of the project
are listed in the history of the repository.

The board is available from the
[Enjoy-Digital shop](https://enjoy-digital-shop.myshopify.com). Questions and requests are
welcome at the address printed on the title page.

## How to read this manual {#intro-reading}

A new user can go straight to [Getting started](#getting-started), the first chapter of
Part IV, and come back later. A reader who wants to understand the design reads the three
primers first. The table below sends each question to its chapter, in the order of the manual.

| Question | Chapter |
|------------------------------------------------|----------------------------------------|
| What are I/Q samples, and what does the AD9361 do? | [SDR in a nutshell](#sdr-primer) |
| Which LiteX cores does the gateware reuse? | [LiteX in a nutshell](#litex-primer) |
| Why does a radio need a time reference? | [Time in a nutshell](#timing-primer) |
| What is on the board, and on which pins? | [Hardware](#hardware) |
| Which carrier gives which feature? | [Carriers and hosts](#carriers) |
| How is the SoC organised? | [The SoC](#soc) |
| How does the FPGA talk to the RFIC? | [The RFIC interface](#rfic) |
| How do samples and timestamps travel? | [Streaming](#streaming) |
| How does each host link work? | [Host links](#host-links) |
| How is the board time synchronised? | [Time synchronisation](#timing) |
| How do I install the software and get samples? | [Getting started](#getting-started) |
| Which library and tools run on the host? | [Host software](#software) |
| How do I use GNU Radio, GQRX or srsRAN? | [SDR frameworks](#frameworks) |
| How do I build or extend the gateware? | [Building the gateware](#building-gateware) |
| How do I record, replay or synchronise? | [Workflows](#workflows) |
| The board does not answer: what now? | [Debugging](#debugging) |
| What does this register do? | [Register reference](#registers) |
| What does this acronym mean? | [Glossary](#glossary) |
