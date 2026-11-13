# LiteX in a nutshell {#litex-primer}

This chapter introduces the LiteX infrastructure that the gateware reuses. It gives the
vocabulary of a LiteX design (SoC, registers, streams, host bridges), then presents each core in
a few paragraphs: what it does, how the board uses it, and where its own manual is. A reader who
already builds LiteX designs can skip to [The SoC](#soc).

## At a glance {#litex-glance}

| Core | Role on the board | Described in |
|----------------|------------------------------------------|----------------------------------|
| LiteX | SoC, bus, registers, build, host tools | [The SoC](#soc) |
| LitePCIe | PCIe host link: registers, DMA, driver, PTM | [Host links](#host-links) |
| LiteEth | Ethernet host link: PHY, UDP, Etherbone, PTP | [Host links](#host-links) |
| LiteSATA | SATA storage | [Host links](#host-links) |
| USB3-PIPE | USB3 host link (in development) | [Host links](#host-links) |
| LiteI2C | I2C master of the clock generator | [Time synchronisation](#timing) |
| LiteScope | Logic analyser inside the FPGA | [Debugging](#debugging) |
| LiteX-WR-NIC | White Rabbit core and its support | [Time synchronisation](#timing) |

## LiteX and Migen {#litex-what}

LiteX is a framework to build FPGA designs in Python. A design is a Python program: it
instantiates cores, connects them, and generates the Verilog, the constraints and the scripts of
the FPGA tools. Migen is the Python library in which the logic itself is described.

Two consequences matter for a reader of this project. First, the options of a design are plain
Python arguments: a build option of the board, such as `--with-eth`, selects which cores the
program instantiates. Second, the same program also generates the description of its registers
for the host software, so gateware and software stay aligned.

A LiteX design is organised as a system on chip (SoC): a bus, peripherals with registers, and
usually a soft CPU. The board uses the variant without CPU (`SoCMini`). The host computer plays
the role of the CPU, through the host bridges described below.

## Bus and registers {#litex-soc}

Each core exposes its controls and its status as registers. LiteX places them on a register bus
and gives every core a slot in the address map. A main bus, of the Wishbone type, connects the
bus masters to the register bus and to the memories. The figure below shows the structure.

![A LiteX SoC without CPU. The host reaches the bus through a bridge. The bus gives access to
the registers of every core.](litex_soc.svg)

LiteX names this register bus the CSR bus, for Control and Status Registers. The build writes
the list of registers and their addresses in two forms: a CSV file for the Python tools and a C
header for the driver and the library.

On the board, the SoC fixes the slot of most cores, so that their registers keep their address
whatever the build options are. A few registers have no fixed slot and move between builds.
[The SoC](#soc) gives the map and its rule, and the [Register reference](#registers) lists the
registers.

## Streams {#litex-streams}

Data moves between cores on *streams*. A stream is a one-way channel with a handshake: the
source says that its data is valid, the sink says that it is ready. A word is transferred on
each clock cycle where both are true. The figure below shows the signals.

![A LiteX stream. The source drives valid and the data; the sink drives ready. The last signal
marks the end of a packet.](litex_stream.svg)

The handshake gives back-pressure for free: a slow sink holds the source without losing data.
Every LiteX core speaks this convention, so the output of one core connects to the input of
another with one statement.

LiteX provides the usual stream tools: FIFOs, clock domain crossings, width converters,
multiplexers. The board uses a stream crossbar to connect its SDR datapath to the host link
selected by the software (see [Streaming](#streaming)).

## Host bridges {#litex-bridges}

A host bridge turns a link to a computer into a master of the SoC bus. The host reads and
writes any register through it, exactly as a CPU of the SoC would. The board has one bridge per
host link, and one over JTAG that needs no host link at all.

| Bridge | Link | Host side |
|------------|------------------|-----------------------------------------------------|
| PCIe | PCIe | Kernel driver, or `litex_server --pcie` |
| Etherbone | Ethernet, UDP | The library, or `litex_server --udp` |
| JTAGBone | JTAG cable | `litex_server --jtag` |
| USB | USB3 (in development) | Vendor requests on endpoint 0 |

The LiteX host tools sit on top of these bridges. `litex_server` opens a bridge and the other
tools connect to it: `litex_cli` reads and writes registers by name, and `litescope_cli` drives
the logic analyser. The same tools work whatever the bridge is.

This is the reason the board needs no soft CPU and no embedded software for its normal
operation. The
configuration of the RFIC, of the clock generator and of the streams is host software that
writes registers. [Debugging](#debugging) shows the bridges in use.

## LitePCIe {#litex-pcie}

LitePCIe is the PCIe core of LiteX. It wraps the PCIe block of the FPGA and gives a design two
services: access to the registers, and DMA channels that move streams to and from the memory of
the host. The figure below shows them.

![LitePCIe. The memory-mapped path (MMAP) serves the registers. The DMA writer sends the receive
stream to host memory; the DMA reader fetches the transmit stream from it.](litex_pcie.svg)

- **PHY.** The PCIe block built in the Artix-7 sits below LitePCIe, wrapped for the 7-series
  (`S7PCIEPHY`). It holds the physical, data link and transaction layers. The board builds it
  for one, two or four lanes.
- **MMAP.** The host maps a memory region of the device (BAR0). Reads and writes in this region
  become bus accesses of the SoC.
- **DMA.** A DMA channel copies a stream into a ring of buffers in host memory, or the reverse,
  with no copy by the CPU. An interrupt (MSI) tells the driver when buffers are done.
- **PTM.** An optional requester exchanges time messages with the host (see
  [Time in a nutshell](#timing-primer)).
- **Driver.** LitePCIe comes with a Linux kernel driver and user-space helpers. The driver of
  the board derives from them. The core and its driver are mature: the README of the project
  records their use in several commercial projects.

The samples of the board travel on one DMA channel: receive samples through the writer,
transmit samples through the reader. [Host links](#host-links) gives the settings.

## LiteEth {#litex-eth}

LiteEth is the Ethernet core of LiteX. It is a stack of layers, each of them optional, from the
physical layer up to UDP.

| Layer | What the board uses |
|-----------------|------------------------------------------------------------------|
| PHY | 1000BASE-X or 2500BASE-X on a transceiver; 5GBASE-R (Experimental) |
| MAC | Framing, preamble, CRC |
| ARP, IP, ICMP | The board answers to its IP address and to ping |
| UDP | A crossbar that gives each user a port |
| Etherbone | Host bridge over UDP |
| Stream to UDP | Receive and transmit samples as UDP datagrams |
| PTP | IEEE 1588 slave with hardware timestamps (option) |

The stack is hardware only: no CPU and no software run on the board for networking. The price
is a fixed feature set. The board has a static IP address, set at build time, and speaks UDP,
not TCP.

The PHYs are made for an SFP cage: the transceiver of the FPGA connects directly to the module.
This is why the Ethernet host link needs the baseboard, which carries the cages.

## LiteSATA {#litex-sata}

LiteSATA is the SATA core of LiteX. It implements the host side of SATA: the FPGA drives a disk
or an SSD directly, as the controller of a computer does.

The core has the layers of the standard: a PHY on a transceiver, the link layer, the transport
layer and the command layer. On top, a crossbar shares the disk between several users, each
reading or writing ranges of sectors.

On the board, two kinds of users share the SSD. Streamers write the receive stream to sectors
and read sectors into the transmit stream, at the pace of the radio. Other ports let the host
read and write sectors through the host link, to retrieve a recording or to prepare a replay.

## Transceivers and the shared PLL {#litex-transceivers}

PCIe, Ethernet, SATA and USB3 all run on the transceivers of the FPGA. A transceiver serialises
and deserialises data at several gigabits per second, and needs a clean high-frequency clock
from a PLL.

In the Artix-7, transceivers are grouped by four in a *quad*, and the quad has two PLLs for its
four transceivers. The figure below shows this structure.

![The PLLs of a transceiver quad. Each of the two PLLs has its own reference clock. Each
transceiver takes its clock from one of the two.](litex_qpll.svg)

Each protocol needs its own line rate, hence its own PLL setting. The LiteX cores usually create
the PLL they need. On the board this would not work: two cores would each claim the same
resource. The PLLs are therefore created once, by the SoC, and each core receives the PLL
channel it needs (`SharedQPLL`).

Two PLLs mean two protocols at once. A bitstream can hold PCIe and Ethernet, PCIe and SATA, or
Ethernet and SATA, but not the three of them. [Host links](#host-links) gives the valid combinations.

LiteICLink is the LiteX core for the transceivers themselves and for raw links between FPGAs.
The design of the repository does not instantiate it; it is the natural tool for a derived
design that links several boards through their spare lanes.

## USB3-PIPE {#litex-usb3}

USB3-PIPE is a USB 3 device implemented in the FPGA, with no USB chip. Its PHY trains the link
on a transceiver, and its device core provides the endpoints.

The USB3 host link of the board is in development (see [Status](#intro-status)). It reaches a
USB connector through an adapter plugged in an SFP cage of the baseboard. Bulk endpoints carry
the receive and transmit samples, and the control endpoint gives access to the registers.

## LiteI2C, LiteScope and the others {#litex-others}

**LiteI2C** is a small I2C master. The board uses it to program the SI5351 clock generator. A
sequencer in the gateware loads a default configuration at power-up, and the host can then
rewrite it.

**LiteScope** is a logic analyser built in the design. It records chosen signals in a memory of
the FPGA when a trigger fires, and the host reads the capture through a host bridge. The build
options of the board ending in `-probe` each add an analyser on a group of signals.

**LiteX-WR-NIC** packages the White Rabbit core for LiteX designs. The White Rabbit build of the
board uses its integration helper, its clocking blocks and its host tools.

**LiteDSP** gathers generic signal processing blocks. The extension hook of the SoC is meant to
host them (see [Building the gateware](#building-gateware)).

## Contributions to the cores {#litex-upstream}

The reuse works in both directions. When the board needs something a core lacks, the feature is
added to the core, not to the board. The next project that uses the core then finds it there.

The introduction lists examples under
[A board that improves LiteX](#intro-approach-upstream): PTM and a more robust driver in
LitePCIe, 5GBASE-R on Artix-7 in LiteEth, the external PLL option and the Wishbone bridge of
USB3-PIPE, SFP module management in LiteX.

The shared PLL is a good illustration. USB3-PIPE created its own PLL, as most cores do. To sit
next to PCIe, Ethernet or SATA on the board, its Artix-7 transceiver gained an option to use a
PLL owned by the SoC. Any design that mixes USB 3 with another protocol in one quad now uses the
same option.

## Manuals of the cores {#litex-manuals}

Each core has its own documentation. The cores of Enjoy-Digital publish a manual, as PDF and
HTML, built from their repository.

- LiteX: [repository](https://github.com/enjoy-digital/litex), [wiki](https://github.com/enjoy-digital/litex/wiki).
- LitePCIe: [repository](https://github.com/enjoy-digital/litepcie), [manual](https://github.com/enjoy-digital/litepcie/releases/tag/manual-nightly).
- LiteEth: [repository](https://github.com/enjoy-digital/liteeth), [manual](https://github.com/enjoy-digital/liteeth/releases/tag/manual-nightly).
- LiteSATA: [repository](https://github.com/enjoy-digital/litesata), [manual](https://github.com/enjoy-digital/litesata/releases/tag/manual-nightly).
- USB3-PIPE: [repository](https://github.com/enjoy-digital/usb3_pipe), [manual](https://github.com/enjoy-digital/usb3_pipe/releases/tag/manual-nightly).
- LiteICLink: [repository](https://github.com/enjoy-digital/liteiclink), [manual](https://github.com/enjoy-digital/liteiclink/releases/tag/manual-nightly).
- LiteScope: [repository](https://github.com/enjoy-digital/litescope), [manual](https://github.com/enjoy-digital/litescope/releases/tag/manual-nightly).
- LiteDSP: [repository](https://github.com/enjoy-digital/litedsp), [manual](https://github.com/enjoy-digital/litedsp/releases/tag/manual-nightly).
- LiteI2C: [repository](https://github.com/litex-hub/litei2c).
- LiteX-WR-NIC: [repository](https://github.com/enjoy-digital/litex_wr_nic).

## Where to go next {#litex-next}

[Time in a nutshell](#timing-primer) closes the primers. [The SoC](#soc) then describes how the
board assembles these cores.
