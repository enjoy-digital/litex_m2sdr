# Carriers and hosts {#carriers}

The board is an M.2 module: what it can do depends on what it is plugged into. This chapter lists
the carriers, what each one connects to the four transceiver lanes, and the hosts the project is
used with. Read it to choose a setup; the commands are in [Getting started](#getting-started).

## At a glance {#carriers-glance}

| Carrier | Variant | Host links | Other connections |
|-----------------------------|----------------|-------------------------|---------------------------|
| M.2 slot of a host | `m2` | PCIe Gen2 x1, x2, x4 | None |
| M.2 carrier card, several boards | `m2` | One PCIe link per board | None |
| LiteX Acorn Baseboard Mini | `baseboard` | PCIe Gen2 x1, Ethernet | SATA, 2 SFP cages, USB JTAG |
| SFP to USB3 adapter on the baseboard | `baseboard` | USB3, in development | See the baseboard |

A carrier fixes the build variant, and the variant fixes the pins of the lanes. A bitstream built
for one variant does not work on the other carrier: flash the image that matches the carrier
before moving the board. [Flashing a release image](#gs-flash) lists the release images of each
variant.

## The four lanes {#carriers-lanes}

The figure below shows where each transceiver lane goes. In an M.2 slot the four lanes face the
PCIe port of the host. On the baseboard each lane goes to a different connector.

![Use of the four transceiver lanes. In the M.2 slot of a host they form one PCIe link; on the
baseboard each lane reaches its own connector.](carrier_lanes.svg)

| Lane | Variant `m2` | Variant `baseboard` | Platform resource |
|------|-----------------------|--------------------------|----------------------------|
| 0 | PCIe lane 0 | SATA connector | `sata` |
| 1 | PCIe lane 1 | SFP1 cage | `sfp` 1 |
| 2 | PCIe lane 2 | SFP0 cage | `sfp` 0 |
| 3 | PCIe lane 3 | PCIe x1 edge connector | `pcie_x1_baseboard` |

The `m2` variant has three PCIe resources, `pcie_x1_m2`, `pcie_x2_m2` and `pcie_x4_m2`, which
use lane 0, lanes 0 and 1, and the four lanes. The baseboard resources are added to the platform
only when the variant is `baseboard`. [Transceiver lanes](#hw-m2-lanes) gives the pins.

## M.2 slot of a host {#carriers-m2}

This is the default setup and it needs nothing else: the board goes in an M-keyed M.2 slot that
carries PCIe, the kind of slot an NVMe SSD uses. The slot powers the board and provides the PCIe
reference clock and reset.

The host link is PCIe Gen2 over one, two or four lanes, chosen at build time with `--pcie-lanes`.
One lane carries the standard 61.44 MSPS 2T2R rate. The README of the project gives about
14 Gbps for the four lanes.

> **Note**
> The margin of one lane is small at this rate: 2T2R at 61.44 MSPS is 491.5 MB/s of samples in
> the SC16 format, against 500 MB/s of raw rate for a Gen2 lane.

The oversampling mode, which is Experimental (see [Status](#intro-status)), needs two or four
lanes.

Two limits come with a plain M.2 slot:

- The transceivers are all taken by PCIe: no Ethernet, no SATA.
- The PPS pin is M.2 pin 22, which a standard slot leaves unconnected. Use a test point for the
  PPS input, as [Test points and PPS input](#hw-pps) explains.

A slot of 80 mm takes the board directly. Secure it with the screw of the slot: the RF cables pull
on the module.

The public issue tracker of the repository also reports first tests in an external NVMe
enclosure with a Thunderbolt port. This setup is not part of the [status table](#intro-status).

## Several boards on an M.2 carrier card {#carriers-multi}

A PCIe card with several M.2 slots takes several boards in one PC. The photograph below shows four
boards on an ASUS Hyper M.2 x16 Gen 4 card: eight transmit and eight receive channels in one PCIe
x16 slot.

![Four LiteX-M2SDR boards on an ASUS Hyper M.2 x16 Gen 4 card. Each board carries a heatsink on
its FPGA and on its RFIC.](m2sdr_hyper_m2_8t8r.png){width=55%}

Such a card has no PCIe switch: it splits the 16 lanes of the slot into four links of four lanes.
The motherboard must support this split, called PCIe bifurcation, and it must be enabled for the
slot in the BIOS setup of the PC.

Each board is then an independent PCIe device with its own link, its own kernel driver instance
and its own device node. Each board uses the `m2` variant, exactly as in a single slot.
[Workflows](#workflows) covers the use of several boards together, and
[Time synchronisation](#timing) their synchronisation.

## LiteX Acorn Baseboard Mini {#carriers-baseboard}

The LiteX Acorn Baseboard Mini is a small carrier made by Enjoy-Digital for M.2 FPGA modules. It
breaks the four lanes out to separate connectors, which gives the board Ethernet and SATA. Its
schematic is public: `doc/litex_acorn_baseboard_mini_schematic.pdf` in this repository, and the
repository `enjoy-digital/litex-acorn-baseboard` for the connector reference.

![The LiteX-M2SDR on the LiteX Acorn Baseboard Mini, with a heatsink on the FPGA and on the RFIC.
A copper SFP module carries Ethernet, a Power over Ethernet splitter on the Ethernet cable feeds a
USB-C connector of the baseboard, and an antenna is connected with a coaxial
pigtail.](m2sdr_baseboard_ethernet.png){width=80%}

### Connectors {#carriers-baseboard-connectors}

| Reference | Connector | Use with the board |
|-----------|------------------------------|-------------------------------------------------------|
| J2 | M.2 Key M socket | The board |
| J1 | PCIe x1 card edge | PCIe Gen2 x1 to a host, power from the slot |
| J3 | SFP0 cage | Ethernet, White Rabbit, `--eth-sfp=0` |
| J8 | SFP1 cage | Ethernet, White Rabbit, `--eth-sfp=1` |
| J5 | SATA connector | SATA storage, `--with-sata` |
| J7 | USB-C | JTAG and UART, FTDI FT2232H or FT4232H |
| J9 | USB-C | Power input with USB Power Delivery |
| J10 | Pico-EZmate, 6 pins | JTAG towards the M.2 module |
| J13 | Header, 6 pins | External JTAG probe |

SFP0 is the cage closest to the M.2 socket and SFP1 the one closest to the SATA connector. The
FTDI chip depends on the revision of the baseboard; the build scripts detect it.

### Power {#carriers-baseboard-power}

The baseboard works in a PCIe slot or on a desk. Two jumpers select the power source.

| Jumper | Position 1-2 | Position 2-3 |
|--------|-------------------------------------|---------------------------------------|
| JP2 | Regulator fed by the USB-C J9 | Regulator fed by the 12 V of the slot |
| JP3 | Baseboard 3.3 V from the regulator | Baseboard 3.3 V from the slot |

The M.2 socket is always powered by the on-board 3.3 V regulator, so JP2 must be set in every
case: 1-2 on a desk, 2-3 in a PCIe slot. On a desk, a Power over Ethernet splitter with a USB-C
output can feed the power input, as in the photograph above.

### JTAG and UART {#carriers-baseboard-jtag}

The USB-C connector J7 gives the host a JTAG probe. The JTAG signals leave the baseboard on J10,
and a 6-pin Pico-EZmate cable brings them to the JTAG connector of the module. The pin order of
J10 (not connected, TDI, TMS, TDO, TCK, GND) follows the order of
[the JTAG connector of the board](#hw-jtag).

The same FTDI chip has a UART, which the baseboard routes through the `ALERT#` and `CLKREQ#` pins
of the M.2 socket. The LiteX-M2SDR does not connect these two pins to its FPGA: this UART is not
available with the board. Use JTAGBone or Etherbone instead, as [Debugging](#debugging) explains.

### SFP modules and their I2C bus {#carriers-baseboard-sfp}

The data pairs of a cage go straight to one transceiver lane. The gateware selects the cage with
`--eth-sfp` for Ethernet and `--wr-sfp` for White Rabbit. Do not enable Ethernet and White Rabbit
together, even on two different cages: the two cores would share one PLL channel, and the build
does not reject the pair.

Two SFP modules were used for the validation. [Host links](#host-links) gives the Ethernet PHYs
and their rates, and [Workflows](#workflows) the setup of each module.

| Ethernet PHY | SFP module | Setup |
|---------------|-----------------------------------|-----------------------------------|
| 2500BASE-X | LianGuo LG 2.5GE, copper | SFP0 (J3), JP1 and JP4 fitted |
| 5GBASE-R | H!Fiber ASF-10G-T80, copper | Switched to a 5GBASE-R host side |

The I2C bus of the SFP modules is a separate matter. The baseboard connects it to the SMBus pins
of the M.2 socket through a level shifter, behind four jumpers.

| Jumpers | Effect |
|------------|------------------------------------------------|
| JP1, JP4 | Connect the I2C bus of SFP0 to the SMBus |
| JP5, JP6 | Connect the I2C bus of SFP1 to the SMBus |

Every SFP module answers at the same I2C addresses: close the jumpers of one cage at a time. On
the board side, the SMBus pins reach the FPGA through two resistors that are not mounted by
default, as [Sideband signals](#hw-m2-sideband) shows. Only the White Rabbit build uses this bus.

The public documentation of the baseboard reports one known issue. The pull-down resistors on the
transmit disable pin of the cages are 4.7 kΩ, which is too weak for some modules, mostly copper
modules with an internal PHY. A module that behaves erratically may need 1 kΩ or less there:
R75 for SFP0 and R7 for SFP1.

### What the baseboard allows together {#carriers-baseboard-combos}

On the baseboard, PCIe is limited to one lane, the lane of the card edge. The other lanes are free
for Ethernet and SATA. The transceivers share the two PLLs of the quad, so the build refuses
PCIe, Ethernet or White Rabbit, and SATA all at once. [Host links](#host-links) explains the
combinations that work.

## SFP to USB3 adapter {#carriers-usb3}

The USB3 host link is in development; see the [status table](#intro-status). It uses the
baseboard: an SFP to USB3 adapter, such as the Xillybus SFP2USB, plugs in one SFP cage and
brings one transceiver lane to a USB connector.

The FPGA then implements the USB 3.2 physical layer and a device with USB3-PIPE,
without any USB chip. [Host links](#host-links) describes the intended architecture.

## Upcoming carrier {#carriers-upcoming}

A baseboard with integrated Power over Ethernet is an upcoming carrier.

## Which feature needs which carrier {#carriers-features}

The carrier decides three groups of features. The [status table](#intro-status) gives the
carrier, the build option and the maturity of each feature, one by one.

| Needs | Features |
|-----------------------------|-----------------------------------------------------------------|
| The baseboard | Ethernet host link, Ethernet PTP, PTP-disciplined RFIC reference, White Rabbit, SATA storage, USB3 host link |
| An M.2 slot | PCIe over two or four lanes, oversampling mode |
| Either carrier | PCIe over one lane, PCIe PTM, hardware timed TX, 10 MHz reference, multiboot, GPIO |

[Build options](#build-options) lists every option.

## Hosts {#carriers-hosts}

The reference host is a PC with Linux. The project is also used on three single-board computers.
The table below gives the hardware side; the software steps of each one are in
[Platform guides](#gs-platforms).

| Host | M.2 connection | Notes |
|------------------------|-------------------------------------|---------------------------------------|
| PC, x86-64 | M.2 slot, carrier card or baseboard | Reference host |
| Raspberry Pi 5 | M.2 HAT on the PCIe connector | One PCIe lane, 5 V 4 A supply |
| Orange Pi 5 Max | On-board M.2 slot | 5 V 5 A supply on USB-C |
| NVIDIA Jetson Orin | M.2 slot | NVIDIA L4T kernel |

The Raspberry Pi 5 has no M.2 slot: it needs an M.2 HAT that takes 2280 modules, such as the
Pineboards HatDrive BM1L used for the guide. Its PCIe port has one lane. On the Raspberry Pi 5
and on the Orange Pi 5 Max, an M2 screw of 3 mm locks the module in place.

An Ethernet setup needs no M.2 slot on the host at all. The board sits on the baseboard, and any
host with a suitable network port reaches it. [Workflows](#workflows) gives the network setup.

Where to go next: [Getting started](#getting-started) to install the software and run the board,
[Host links](#host-links) for the gateware of each link, [Hardware](#hardware) for the pins.
