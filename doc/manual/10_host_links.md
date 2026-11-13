# Host links {#host-links}

This chapter describes how the gateware connects the board to the outside through the four
transceivers of the FPGA: the PCIe, Ethernet and USB3 host links, and the SATA storage.

It is for the reader who builds a variant, combines links or
changes one of them. The cores are described in their own manuals; this chapter covers what
LiteX-M2SDR adds around them.

## At a glance {#links-glance}

| Link | Core | Role | Build option |
|----------------|--------------------|----------------------------|-------------------------------------|
| PCIe | LitePCIe | Registers, sample streams | `--with-pcie` |
| Ethernet | LiteEth | Registers, sample streams, PTP | `--with-eth` |
| SATA | LiteSATA | Record and replay on an SSD | `--with-sata` |
| White Rabbit | LiteX-WR-NIC | Time synchronisation only | `--with-white-rabbit` |
| USB3 | USB3-PIPE | Registers, sample streams | `--with-usb3` |

PCIe exists on both variants; the other links need the `baseboard` variant. The status of each
link is in the [status table](#intro-status). The options are listed in
[Build options](#build-options) and the SoC around the links is described in [SoC](#soc).

## Transceiver lanes and links {#links-lanes}

The M.2 connector carries four differential lanes, wired to the four GTP transceivers of the
FPGA. In the M.2 slot of a host they are PCIe lanes. On the LiteX Acorn Baseboard Mini the same
lanes reach two SFP cages, a SATA connector and one PCIe lane, so each lane can run another
protocol.

The figure below shows three links of the gateware. Each link core ends on one port of the
stream crossbar, which selects where the TX stream comes from and where the RX stream goes. The
crossbar and the streams behind it are described in [Streaming](#streaming).

The USB3 host link is the fourth one. It takes an SFP cage in place of Ethernet and ends on
port 3 of the crossbar; [USB3 host link](#links-usb3) has its own figure.

![The links of the gateware. Look at the crossbar ports: port 0 is PCIe, port 1 is Ethernet and
port 2 is SATA; the cyan arrow is the stream path to the RFIC.](link_overview.svg)

The lane of each link is fixed by the pin tables of the platform file.

| M.2 lane | `m2` variant | `baseboard` variant |
|------------------------|-------------------------------------|---------------------------------------|
| 0 | PCIe lane 0 | SATA connector |
| 1 | PCIe lane 1 (x2, x4) | SFP cage 1 |
| 2 | PCIe lane 2 (x4) | SFP cage 0 |
| 3 | PCIe lane 3 (x4) | PCIe x1 |

Ethernet and SATA need the baseboard: the build stops with an error when `--with-eth` or
`--with-sata` is used with the `m2` variant. White Rabbit and USB3 have the same rule. The
carriers are
described in [Carriers](#carriers).

On the way to an SFP cage one pair of each lane is inverted once and the other twice, between
the board and the baseboard. The gateware therefore inverts the receive polarity and not the
transmit polarity, for Ethernet and for White Rabbit.

## Shared QPLL {#links-qpll}

A GTP quad has one common block with two PLLs, and every transceiver of the quad takes its line
clock from one of them. The gateware instantiates this block once and gives one PLL to each
enabled protocol (`SharedQPLL`). Two protocols at most can therefore run in one bitstream.

![The shared QPLL, drawn for PCIe, Ethernet and SATA. The selection gives a PLL to two of the
protocol configurations at most; the cyan arrows are the two clock channels the transceivers
take.](link_qpll.svg)

The channels are given in a fixed order: PCIe first, then Ethernet, then SATA, then USB3. With
one protocol the map has one entry on channel 0. A link gets its channel by name (`pcie`, `eth`,
`sata` or `usb3`) with `get_channel()`; an unknown name raises an error.

| Protocol | Reference clock | Reference input | `fbdiv` | `fbdiv_45` |
|-------------------|----------------------------|-----------------------|--------------|-------------------|
| PCIe | `refclk_pcie`, from the PCIe PHY | `GTREFCLK0` | 5 | 5 |
| 1000BASE-X | `refclk_eth`, 125 MHz | `GTGREFCLK0` | 4 | 5 |
| 2500BASE-X | System clock, 125 MHz | `GTREFCLK0` | 5 | 5 |
| 5GBASE-R | `refclk_eth`, 103.125 MHz | `GTGREFCLK0` | 5 | 5 |
| White Rabbit | `refclk_eth`, 125 MHz | `GTGREFCLK0` | 4 | 5 |
| SATA | `refclk_sata`, 150 MHz | `GTGREFCLK0` | 5 | 4 |
| USB3 | System clock, 125 MHz | `GTGREFCLK0` | 4 | 5 |

The Ethernet settings give line rates of 1.25 Gbps, 3.125 Gbps and 5.15625 Gbps, which
`test/test_qpll.py` checks. The 125 MHz and 103.125 MHz references come from a PLL fed by the
system clock, and the 150 MHz reference from a second one. With White Rabbit the 125 MHz
reference comes from the tunable MMCM instead. The USB3 setting gives a PLL frequency of
2.5 GHz for the 5 Gbps line rate, which the same test checks.

White Rabbit uses the Ethernet channel, with the 1000BASE-X settings: for the QPLL,
`--with-white-rabbit` counts as Ethernet. The table below lists what can share a bitstream.

| Combination | Result |
|----------------------------------------------|------------------------------------------------------|
| PCIe alone, Ethernet alone | Channel 0 |
| PCIe and Ethernet | PCIe on channel 0, Ethernet on channel 1 |
| PCIe and SATA | PCIe on channel 0, SATA on channel 1 |
| Ethernet and SATA | Ethernet on channel 0, SATA on channel 1 |
| PCIe and White Rabbit | PCIe on channel 0, White Rabbit on channel 1 |
| Ethernet and White Rabbit | Rejected with an error |
| PCIe, Ethernet or White Rabbit, SATA | Rejected with an error |
| USB3 alone | Channel 0 |
| PCIe and USB3 | PCIe on channel 0, USB3 on channel 1 |
| USB3 with Ethernet, SATA or White Rabbit | Rejected with an error |

Ethernet and White Rabbit cannot be enabled together. Both use the `eth` channel: the two cores
would drive the reset of the same PLL, and 2500BASE-X or 5GBASE-R set a line rate that White
Rabbit cannot use. The script refuses the pair, whatever the SFP cages.

In the code: the shared PLL is in `litex_m2sdr/gateware/qpll.py`, and the reference clocks are
created by `CRG` in `litex_m2sdr.py`.

## PCIe {#links-pcie}

PCIe is the host link of the `m2` variant, enabled with `--with-pcie`. It uses the hard PCIe
block of the FPGA through LitePCIe, which is described in the
[LitePCIe manual](https://github.com/enjoy-digital/litepcie/releases/tag/manual-nightly). The
concepts are introduced in [LiteX primer](#litex-primer).

### PHY and endpoint {#links-pcie-phy}

| Item | Value |
|--------------------------|--------------------------------------------------------------------------|
| PHY class | `S7PCIEPHY` |
| Speed | Gen2 by default, Gen1 with `--pcie-gen=1` |
| Lanes | 1, 2 or 4 on the `m2` variant; 1 on the `baseboard` variant |
| PHY data width | 64 bits for x1 and x2, 128 bits for x4 |
| BAR0 | 1 MiB |
| Device class | Wireless controller, RF controller (`0x0d`, `0x10`) |
| Line clock | Channel `pcie` of the shared QPLL |

On the `m2` variant with more than one lane, the gateware constrains each transceiver to its M.2
lane. Without this the placement of the PCIe block would put lane 1 on another lane and the link
could only train as x1. On the `baseboard` variant the single lane is constrained to M.2 lane 3.

### Registers and streams {#links-pcie-mmap}

The LiteX SoC adds the three services of LitePCIe in one call (`add_pcie`). The memory-mapped
bridge gives the host access to the registers of the SoC through BAR0. The DMA carries the
sample streams. The MSI controller reports the DMA and SATA interrupts.

| Item | Value |
|-------------------------|---------------------------------------------------------------------------|
| DMA channels | 1 by default (`--pcie-dmas`), channel 0 carries the samples |
| DMA data width | 64 bits, 128 bits in oversampling mode |
| DMA buffering | Enabled, depth 8192 |
| DMA loopback | Present |
| DMA synchronizer | Present, started by the internal PPS |
| Address width | 64 bits |

The DMA synchronizer, when the kernel driver arms it, releases the two directions on the next
pulse of the internal PPS. A stream can therefore start up to one second after the request, as
described in [Streaming](#streaming).

If the time generator is disabled there is no pulse: the header logic stays in reset and PCIe
streaming stops, unless the synchronizer is bypassed by software. After a write of the board
time the pulses come in a burst, described in [Internal PPS](#timing-pps-gen): a stream armed
during the burst starts at once, not on a second.

The header logic is also held in reset until the DMA reader or
writer of the stream is enabled, so that each run starts on a buffer boundary.

Extra DMA channels are for designs derived from the gateware that send their own results to the
host next to the samples. They appear as `pcie_dma1` and following. The extension hook that uses
them is described in [Streaming](#streaming).

### Link reset workaround {#links-pcie-workaround}

Some hosts need several attempts before the PCIe link trains. With
`--with-pcie-reset-workaround` the gateware drives the reset of the PCIe PHY itself. Every 10 ms
it checks the link: if the link is down it asserts the reset, and it releases it on the next
tick. Once the link is up the reset stays released (`PCIeLinkResetWorkaround`).

### Host memory access for SATA {#links-pcie-slave}

When PCIe and SATA are both enabled, the SATA DMA engines must read and write the memory of the
host. The gateware adds a second Wishbone bus for this and a bridge that turns its accesses into
PCIe memory requests (`LitePCIeWishboneBurstReadSlave`).

A read fetches 128 words of 32 bits (512 bytes) in one request and serves the following sequential reads from a FIFO; a
read at another address starts a new request. A write is sent as one request per word. Two
timeouts end an access with a bus error when the request cannot be issued or no completion comes
back.

### Timing constraints {#links-pcie-timing}

The PCIe PHY selects its 125 MHz or 250 MHz clock through a clock multiplexer. The gateware
declares the generated clocks on its output, declares the two as exclusive for x2 and x4, and
cuts the paths between the system clock and the PHY clocks, which only meet in clock domain
crossings (`add_s7_pcie_timing_constraints`).

In the code: the PCIe section of the SoC in `litex_m2sdr.py`; the workaround, the bridge and the
constraints are in `litex_m2sdr/gateware/pcie.py`. Time synchronisation over PCIe is described
in [PCIe PTM](#timing-ptm).

## Ethernet {#links-ethernet}

Ethernet is the host link of the baseboard. One SFP cage carries the registers, the sample
streams and, as an option, PTP and VRT packets. The core is LiteEth, described in the
[LiteEth manual](https://github.com/enjoy-digital/liteeth/releases/tag/manual-nightly).

![The Ethernet host link. Look at the UDP ports on the right: each service of the gateware is
one port of the UDP crossbar of the core.](link_ethernet.svg)

### PHYs {#links-eth-phy}

| `--eth-phy` | PHY | Class | System clock | Reference |
|---------------------|------------------|-------------------------------|-------------|------------------|
| `1000basex` | 1000BASE-X | `A7_1000BASEX` | 100 MHz | 125 MHz from a PLL |
| `2500basex` | 2500BASE-X | `A7_2500BASEX` | 125 MHz | System clock |
| `5000baser` | 5GBASE-R | `A7_GTP_5G_BASER` | 100 MHz | 103.125 MHz from a PLL |

The system clock column is the default of the build script, which `--sys-clk-freq` overrides.
2500BASE-X uses the system clock as the reference of the QPLL, so its system clock must be
125 MHz. `--eth-sfp` selects the SFP cage, 0 by default.

The 2500BASE-X PHY is built with MMCM clocking, PCS buffers and autonegotiation timers
accelerated as for a 125 MHz transmit clock. Some copper SFP modules do not complete
autonegotiation with timers scaled to the real time of the 2.5 Gbps link. The option also adds
the timing constraints of the receive gearbox of the PHY.

The 2500BASE-X link was validated in SFP cage 0 (J3) with a LianGuo LG 2.5GE copper SFP module
and jumpers JP1 and JP4 of the baseboard fitted.

5GBASE-R uses the BASE-R PCS of LiteEth: 64b/66b coding at 5.15625 Gbps through the gearbox of
the GTP. The class is imported only for this PHY, so the other PHYs build with a LiteEth that
does not have it.

The PHY adds diagnostic registers: reset, loopback and PRBS31 controls, block
lock, high bit error rate, link state and PRBS error count. They are used in
[Debugging](#debugging).

> The SFP module of a 5GBASE-R link must present a 5GBASE-R host interface at 5.15625 Gbps.
> Supporting 5GBASE-T on the copper side does not guarantee it: multi-rate copper modules often
> keep a fixed 10.3125 Gbps host side with rate adaptation.

Modules built on a manageable multi-gigabit PHY can be switched to the 5GBASE-R host mode
through their I2C management path (RollBall). The PHY families are the Broadcom BCM84891L, the
Marvell Alaska 88X3310 and 88E2110, and the Marvell AQR113C, formerly Aquantia. The H!Fiber
ASF-10G-T80, built on the BCM84891L, was validated with this LiteEth stack on the baseboard.

A LiteX SoC with a BIOS switches the module with the command `sfp 0 5GBASER`, but the SoC of
LiteX-M2SDR has no CPU. Configure the module beforehand, since it keeps the setting while
powered, or from a host tool.

The EEPROM of the SFP module is not needed for the link or for traffic. Its I2C bus uses the
SMBus pins of the M.2 connector, which on revision R02 of the board need two resistors (R82 and
R83) that are not fitted by default. A module that does not answer on I2C is not a link failure.

### UDP/IP core and Etherbone {#links-eth-etherbone}

The gateware instantiates the hardware UDP/IP stack of LiteEth with a 32-bit datapath in the
system clock domain and a UDP crossbar (`LiteEthUDPIPCore`). There is no CPU: ARP, ICMP and UDP
are handled in logic.

| Item | Value |
|-------------------------------------|---------------------------------------------------------------|
| MAC address | `0x10e2d5000000` |
| IP address | `192.168.1.50` (`--eth-local-ip`) |
| ARP table | 4 entries |
| IP broadcast | Accepted |
| IGMP | Only with PTP, see [Ethernet PTP](#timing-ptp) |
| Etherbone port | UDP 1234 |
| Etherbone buffer | 160 words |

The MAC address is a constant of the gateware and the IP address is set at build time. Two
boards therefore need bitstreams built with different IP addresses, and separate network
segments, since no option changes the MAC address.

Etherbone is the host bridge of the Ethernet link: it turns UDP packets into reads and writes
on the bus of the SoC. The utilities use it with a `<device>` of the form
`eth:192.168.1.50:1234`.

The buffer of 160 words holds a record of 128 words, the burst the
software uses for the SATA host buffer, with a margin for the address and the headers.

The gateware does not use the Etherbone frontend of LiteEth as it is. That frontend sends every
reply to its own service port, so two programs on the host would compete for the replies. The
variant of the gateware sends each reply to the IP address and the source port of its request
(`SharedLiteEthEtherbone`).

A read reply can be delayed by the bus, so one "last client" register would not be enough. The
address and port of each request that contains reads are queued with the record and taken back
when its reply leaves. Several programs can therefore use the registers at the same time: a
SoapySDR application and a utility, for example.

### Sample streams {#links-eth-streams}

Two LiteEth streamers carry the samples as raw UDP payload, on port 2345 by default
(`--eth-udp-port`). Both have registers to enable them and to set the IP address and the port.

| Direction | Class | FIFO | Path |
|----------------|------------------------------------|---------------------|--------------------------|
| RX | `LiteEthStream2UDPTX` | 128 words of 64 bits | Crossbar port 1 to the host |
| TX | `LiteEthUDP2StreamRX` | 32768 words of 64 bits | The host to crossbar port 1 |

The names of the instances follow the direction of the samples, not of the Ethernet frames:
`eth_rx_streamer` sends the RX stream to the host and `eth_tx_streamer` receives the TX stream
from it.

The host sends TX samples in datagrams of 1 KiB, in bursts and with scheduling jitter. The TX
FIFO of 256 KiB absorbs this. The stream to the RFIC starts only when the FIFO is half full and
stops when it is empty, until it is half full again. When the streamer is disabled, incoming
data is discarded.

When Ethernet is the only host link, the RFIC core also gets a FIFO of depth 8192 on each
stream. With PCIe in the same bitstream these FIFOs are not added: the DMA has its own buffering.

### Sample rate over Ethernet {#links-eth-rate}

The link rate bounds the sample rate, so an Ethernet build also lowers the RFIC clock. The value
is sized for 2T2R in the SC8 format, described in [RFIC](#rfic).

| Build | RFIC clock | 2T2R sample rate |
|-----------------------------------------------|-----------------------|------------------------------|
| Ethernet, 1000BASE-X | 122.88 MHz | 30.72 MSPS in SC8 |
| Ethernet, 2500BASE-X, 5GBASE-R | 245.76 MHz | 61.44 MSPS in SC8 |
| No Ethernet | 245.76 MHz | 61.44 MSPS |
| No Ethernet, oversampling mode | 491.52 MHz | 122.88 MSPS |

The Ethernet streamers and the SATA streamers are 64 bits wide. The 128-bit datapath of the
oversampling mode is made for PCIe: the script refuses `--with-rfic-oversampling` with
`--with-eth` or `--with-sata`.

### VRT streamer {#links-eth-vrt}

With `--with-eth-vrt` the RX stream can also leave the board as VRT packets, the VITA 49 format
that many SDR tools read. A register selects the destination of the RX stream of the Ethernet
port (`main_eth_rx_mode`).

| `sel` | RX stream of crossbar port 1 |
|-------------------------------------|---------------------------------------------------------------|
| 0 | Discarded |
| 1 | Raw UDP streamer (reset value) |
| 2 | VRT streamer |

The reset value is the raw UDP route: a `--with-eth-vrt` bitstream does not send VRT packets
after load. To send them the host writes 2 to this register through a host bridge, with the
register file of the build.

> The shipped software cannot select the VRT route. The library has no definition of this
> register, so its VRT mode and the `eth_mode=vrt` argument of the SoapySDR driver return an
> unsupported error.

On the VRT path the 64-bit words are converted to 32 bits and cut into packets of 256 words
(`RFICDataPacketizer`). A header of five words is inserted in front of each packet and the
result is sent over UDP (`VRTSignalPacketStreamer`).

| Header field | Value |
|------------------------------------------|----------------------------------------------------------|
| Packet type | Signal data with stream identifier |
| Class identifier, trailer | Absent |
| Integer timestamp type | UTC |
| Fractional timestamp type | Real time, in picoseconds |
| Packet count | 4-bit counter, one step per packet |
| Packet size | 261 words: 5 of header and 256 of samples |
| Stream identifier | `0xdeadbeef` |
| Destination | `239.168.1.100`, UDP port 4991 |

The destination is set with `--vrt-dst-ip` and `--vrt-dst-port` and can be changed with the
registers of the streamer.

The timestamps come from the board time. The integer field is the
count of internal PPS pulses and the fractional field is the board time minus that count of
seconds, in picoseconds. The header says UTC, but the value is the board time, which counts from
zero until something sets it.

The internal PPS pulses half a second after each second of the board time, as described in
[PPS](#timing-pps). The integer field therefore changes in the middle of a second. The
fractional field runs from half a second to one and a half seconds, outside the range that
VITA 49 allows for a real-time fraction.

For three system clock cycles after each pulse the two fields are also not consistent: their
sum is one second ahead. A receiver that needs the board time adds the two fields and ignores a
packet whose fraction is 1.5 s or more. A strict VRT decoder can reject the packets.

After a write of the board time the pulses come in a burst, and the integer field is wrong
until the burst ends, as described in [Internal PPS](#timing-pps-gen).

A small receiver is provided for tests: `litex_m2sdr/software/user/m2sdr_vrt_rx.py`. It accepts
signal data packets with a stream identifier and prints their timestamps.

In the code:

| File | Content |
|--------------------------------------|----------------------------------------------------------|
| `litex_m2sdr.py` | Ethernet section of the SoC, Etherbone setup |
| `gateware/etherbone.py` | Etherbone variant with per-request replies |
| `gateware/vrt.py` | VRT streamer |
| `gateware/rfic.py` | Packetizer of the VRT path |

The gateware files are in `litex_m2sdr/gateware/`.

The setup of a host and the streaming recipes are in [Workflows](#workflows).

## SATA storage {#links-sata}

With `--with-sata` the board is the SATA host of an SSD on the baseboard. The gateware records
the RX stream on the SSD and replays sectors as the TX stream, with no host in the sample path.
The core is LiteSATA, described in the
[LiteSATA manual](https://github.com/enjoy-digital/litesata/releases/tag/manual-nightly).

![SATA storage. Look at the five users of the LiteSATA crossbar: two DMA engines exchange
sectors with a memory, and two streamers (cyan arrows) with the sample streams.](link_sata.svg)

### PHY and core {#links-sata-phy}

| Item | Value |
|---------------------------|-------------------------------------------------------------------------|
| PHY | `LiteSATAPHY`, 16-bit, on the channel `sata` of the QPLL |
| Reference clock | 150 MHz from a PLL (`refclk_sata`) |
| Generation | Gen2 by default (`--sata-gen`) |
| Lane | M.2 lane 0, the SATA connector of the baseboard |
| User ports | 5, on a `LiteSATACrossbar` |

The system clock must be at least the SATA clock times 16 and divided by the bus width: 37.5 MHz
for Gen1, 75 MHz for Gen2 and 150 MHz for Gen3 on the 32-bit bus. The build raises an error
otherwise, so Gen3 needs `--sys-clk-freq` at 150 MHz or more.

### Four transfer engines {#links-sata-engines}

One port of the crossbar identifies the drive. The four others are transfer engines, named after
the direction of the data.

| Engine | Direction | Instance |
|----------------------------|-------------------------------------------|-------------------------------|
| `Sector2Mem` | Sectors to a memory | `sata_sector2mem` |
| `Mem2Sector` | A memory to sectors | `sata_mem2sector` |
| `Stream2Sectors` | The RX stream to sectors | `sata_rx_streamer` |
| `Sectors2Stream` | Sectors to the TX stream or to the host | `sata_tx_streamer` |

The names and the fields of the SATA registers are used by the software: the gateware keeps them
stable. All the engines take a first sector and a number of sectors, start on a write of
`start`, and report `done` and `error`. The streamers add `progress` and `stop`.

The engines are variants of the LiteSATA frontends. They send one command for a whole request
instead of one per sector, and they reverse the bytes of each SATA word so that the samples are
stored in the order of the stream.

### Memory access {#links-sata-memory}

The two DMA engines move sectors between the drive and a memory at a base address. Which memory
depends on the host link.

| Build | Target of the DMA engines |
|------------------------------|----------------------------------------------------------------------|
| Ethernet and SATA | The SoC bus: the host buffer |
| PCIe and SATA | A second bus: the host buffer or the memory of the host |

The host buffer is a RAM of 128 KiB mapped on the SoC bus at `0x00020000` (`SATAHostBuffer`).
It holds 256 sectors. The host fills or reads it through its host bridge, with Etherbone bursts
of 128 words over Ethernet. A buffer of 256 KiB was tried and gave a block RAM cascading error
in the FPGA tool.

With PCIe, a router sends the accesses that fall in the address range of the host buffer to a
second port of the RAM and all the others to the memory of the host, through the bridge
described in [Host memory access for SATA](#links-pcie-slave) (`SATADMAMemoryRouter`). The
software reaches this path through an ioctl of the kernel driver, `LITEPCIE_IOCTL_SATA_DMA`.

A PCIe memory write is posted: its acknowledge only means that the endpoint accepted it. Before
it reports `done`, the read engine therefore reads the first word back. The completion of this
read comes after the writes, so the software sees `done` once the data is in the memory of the
host.

### Recorder {#links-sata-recorder}

The recorder writes the RX stream to consecutive sectors (`M2SDRLiteSATAStream2Sectors`). An SSD
wants long contiguous writes, and samples arrive at a constant rate. A FIFO of 512 sectors
(256 KiB) sits between the two.

- A write command covers up to 65535 sectors, about 32 MiB.
- The command starts when 384 sectors are in the FIFO.
- Data is sent to the drive until the FIFO falls to 64 sectors, then held until it is back at
  384 sectors. The same command continues.
- The last 64 sectors of a command are always drained, so a short last command does not wait for
  data that will not come.

The result is bursts of contiguous data in a few long commands, instead of single words or one
command per small buffer.

### Player {#links-sata-player}

The player reads consecutive sectors and outputs them as a stream (`M2SDRLiteSATASectors2Stream`).
It also uses commands of up to 65535 sectors and a FIFO of 512 sectors. A command is sent when
the FIFO is at most half full; while it runs, the FIFO pauses the drive when it is full.

A drive reads faster than the RFIC transmits. The register `pace` holds a rate in 64-bit words
per second, and the player releases words at this rate with a fractional accumulator. It keeps
one pending word when the destination is busy and does not accumulate more.

A value of 0, or a
value at or above the system clock frequency, disables the pacing.

### Recording while the host receives {#links-sata-tap}

The crossbar can route the RX stream to the SATA recorder alone. The tap adds a second mode,
where the recorder gets a copy of the stream that goes to the host (`SATAStreamTap`).

When the register `tap` of the recorder is set and the RX stream is not routed to SATA, each
word is given to the host path and to the recorder in the same cycle, and only when both can
take it.

Neither side sees a word twice or misses one. A slow drive therefore slows the host
path too. When the tap is off, the host path is a plain connection.

### Stopping a transfer {#links-sata-stop}

A SATA command that the drive accepted cannot be aborted. A write of `stop` is therefore taken
at the next command boundary: the streamer finishes its command, of 32 MiB at most, and reports
`done`. A stopped player does not wait for its FIFO to empty.

The register `main_sata_streamer_control` resets the frontend of each streamer with its
`rx_reset` and `tx_reset` bits. The host uses it after a stop to flush the words left in a FIFO.
It does not abort a command either. On a bitstream without the `stop` registers, an interrupted
`m2sdr_sata` lets the whole programmed transfer finish.

> The location of this register depends on the build: `0x15000` in a SATA build, and `0x15004`
> when `--with-eth-vrt` puts `main_eth_rx_mode` before it. The header committed with the
> software has `0xb000`, where a SATA build has no register. A streamer reset from an
> `m2sdr_sata` built against that header resets nothing: build the software against the
> `csr.h` of the bitstream ([The committed register header](#soc-csr-header)).

With PCIe, each of the four engines raises an interrupt at the end of a transfer, successful or
not.

In the code: the SATA section of the SoC in `litex_m2sdr.py` and `litex_m2sdr/gateware/sata.py`.
The recipes
with `m2sdr_sata` and its catalog are in [Workflows](#workflows); the measured rates are in
[Performance](#performance).

## Stream crossbar ports {#links-crossbar}

The crossbar has four ports and two selection registers, `crossbar_mux_sel` for the source of
the TX stream and `crossbar_demux_sel` for the destination of the RX stream. The port numbers
are the same as the constants of the software.

| Port | Link | TX stream source | RX stream destination |
|----------------|---------------------|----------------------------|----------------------------------|
| 0 | PCIe | DMA channel 0 | DMA channel 0 |
| 1 | Ethernet | UDP streamer | UDP or VRT streamer |
| 2 | SATA | Player | Recorder |
| 3 | USB3 | Bulk OUT streamer | Bulk IN streamer |

A port whose link is not in the bitstream is left unconnected. The player is on the TX side of
the crossbar; to show a recording to the host, the software selects the player as TX source and
enables the loopback described in [Streaming](#streaming).

## USB3 host link {#links-usb3}

USB3 is the third host link, for the `baseboard` variant, enabled with `--with-usb3`. One SFP
cage becomes a USB 3.2 Gen 1 port (5 Gbps) through an SFP to USB3 adapter such as the Xillybus
SFP2USB. The USB device is implemented in the FPGA with USB3-PIPE, with no USB chip. The core is
described in the
[USB3-PIPE manual](https://github.com/enjoy-digital/usb3_pipe/releases/tag/manual-nightly), and
the adapter in [SFP to USB3 adapter](#carriers-usb3).

| Item | Value |
|--------------------------|--------------------------------------------------------------------------|
| SFP cage | 0 by default, 1 with `--usb3-sfp=1` |
| USB identifiers | `1d50:605a` by default (`--usb3-vid`, `--usb3-pid`) |
| Device strings | Manufacturer "Enjoy-Digital", product "LiteX-M2SDR", serial "0001" |
| Device class | Vendor class, two interfaces |
| Endpoint 0 | Control: access to the SoC bus |
| Endpoint 1, interface 0 | Bulk IN: the RX stream |
| Endpoint 2, interface 1 | Bulk OUT: the TX stream |
| Burst of the Bulk endpoints | 16 packets |
| Crossbar port | 3 |
| Line clock | Channel `usb3` of the shared QPLL |
| System clock | 125 MHz, required |

The figure below shows the link. The transceiver and the PHY train the link, the device core
answers the host, and three blocks connect the endpoints to the SoC.

![The USB3 host link. The device core has three endpoints: EP0 gives access to the SoC bus, and
the two Bulk endpoints (cyan) carry the sample streams.](link_usb3.svg)

The module of the link assembles the parts of the table below (`USB3`).

| Part | Role |
|-----------------------------|-----------------------------------------------------------------------|
| `A7USB3SerDes` | Transceiver of USB3-PIPE on the lane of the SFP cage |
| `USB3PHY` | Physical layer and link training, in device role |
| `USB3DeviceCore` | Link and protocol layers, descriptors, endpoint 0 |
| `USB3EP0Wishbone` | Vendor requests of endpoint 0 to the SoC bus |
| `USB3RXStreamer` | RX stream of the crossbar to the Bulk IN endpoint |
| `USB3TXStreamer` | Bulk OUT endpoint to the TX stream of the crossbar |

The transmit pair needs no inversion, as for Ethernet. The link training detects an inverted
receive pair and corrects it, so the gateware sets no receive polarity for USB3.

### Clocking {#links-usb3-clock}

The transceiver takes its line clock from a channel of the shared QPLL, like the other links. A
small adapter presents this channel as the PLL object that the USB3-PIPE transceiver expects
(`USB3QPLLChannel`).

The reference of this PLL is the system clock, which must therefore be 125 MHz: the script
refuses another value with `--with-usb3`. The source records why the 100 MHz oscillator is not
used: through a clock buffer it gave link errors and recoveries as soon as the RFIC ran at
30.72 MSPS.

The clock generator of the SoC also makes a clock at one eighth of the system clock for the
transceiver (`cd_oob`). USB3-PIPE uses it to detect the low-frequency signalling that starts
and wakes a USB3 link.

### Registers through endpoint 0 {#links-usb3-ep0}

Endpoint 0 is a host bridge. Its vendor requests are turned into accesses of the SoC bus by a
Wishbone master, which the SoC adds to its bus next to the other bridges
([Host bridges](#litex-bridges)).

The requests follow the protocol of the LiteX USB bridge, so `litex_server --usb` works with the
board. An extension reads or writes up to 128 words in one request, which the library uses for
its block accesses.

A control request needs no claimed interface. The register tools therefore run while another
program streams samples, and two programs can use the registers at the same time.

### Sample streams {#links-usb3-streams}

The two Bulk endpoints carry 32-bit words, and the crossbar carries 64-bit words. Each streamer
holds a width converter and a FIFO; the low half of a 64-bit word travels first, which gives
the host the byte order of the PCIe DMA buffers.

| Streamer | FIFO | When disabled | When enabled |
|-------------------|----------------|-------------------------------|---------------------------------------------|
| RX, Bulk IN | 32 KiB | Drains the RX stream | Flushes the endpoint, then sends |
| TX, Bulk OUT | 256 KiB | Drains the endpoint | Waits until the FIFO is half full, then forwards |

- **RX.** While `rx_enable` is 0 the streamer accepts and drops the RX stream, so the RFIC
  datapath is never blocked. A rising `rx_enable` flushes the packets that the endpoint still
  holds from a previous run.
- **TX.** The FIFO is primed: nothing goes to the RFIC before it is half full, as for the
  Ethernet TX path. When the FIFO runs empty the streamer stops forwarding and waits for half a
  FIFO again. The bit `tx_started` tells whether it forwards.
- **RFIC FIFOs.** When PCIe is not in the bitstream, the RFIC core gets the same FIFO of depth
  8192 on each stream as in an Ethernet-only build.

The buffers have the format of the PCIe DMA: 8192 bytes, with the 16-byte header first when the
RX header is enabled ([Header](#streaming-header)). The SoC keeps the header logic in step with
the streams of this link. With port 3 selected on the RX side, the header inserter is held in
reset until `rx_enable` is set, so the first buffer sent to the host starts a frame.

The TX side works the same way with `tx_enable`, but the TX header itself is not usable on this
link: see [Limits](#links-usb3-limits).

Two interfaces separate the two directions. A host program claims interface 0 to receive and
interface 1 to transmit, so one program can record while another one plays.

### Registers of the link {#links-usb3-regs}

The block has two registers, at location 22 of the register map (`0xb000`).

| Register | Field | Bit | Meaning |
|------------------------|--------------------|-----|-------------------------------------------------|
| `usb3_control` | `rx_enable` | 0 | Enable the RX stream; its rising edge flushes the endpoint |
| | `tx_enable` | 1 | Enable the TX stream |
| `usb3_status` | `ready` | 0 | Transceiver ready |
| | `link_up` | 1 | Link in the U0 state, the active state of USB3 |
| | `tx_started` | 2 | TX FIFO primed and forwarding to the RFIC |

The capability registers report the link: bit 10 of `capability_features` is set, and bit 5 of
`capability_board_info` gives the SFP cage ([Capability registers](#soc-capability)).

The library starts and stops a stream with these registers and the crossbar.

| Step | RX start | TX start |
|--------|--------------------------------------------|----------------------------------------------|
| 1 | Stop the transfers, clear `rx_enable` | Stop the transfers, clear `tx_enable` |
| 2 | Select port 3 in `crossbar_demux_sel` | Clear the halt of the endpoint |
| 3 | Set `rx_enable` | Select port 3 in `crossbar_mux_sel` |
| 4 | Clear the halt of the endpoint, submit the transfers | Set `tx_enable` |

Clearing the halt of an endpoint resets its sequence numbers and drops the packets sent before.
With the flush of `rx_enable`, the first buffer that the host receives starts a frame. At a TX
stop, the library waits for the submitted transfers and for `tx_started` to fall, for one
second at most each, before it clears `tx_enable`.

### Sample rates {#links-usb3-rates}

The script takes about 440 MB/s as the capacity of the link. The table gives the rate of the samples in each direction for
the 2T2R configurations.

| Configuration | Samples | On USB3 |
|-------------------------------------------|------------------|---------------------------------|
| 30.72 MSPS, SC16 | 245.76 MB/s | Fits, in both directions |
| 61.44 MSPS, SC8 | 245.76 MB/s | Fits, in both directions |
| 61.44 MSPS, SC16 | 491.52 MB/s | Above the capacity of the link |

The SoapySDR driver follows this limit: on USB3 it selects SC8 above 40 MSPS unless the
application sets the format ([Device arguments](#fw-args)). The transport formats are compared
in [The RFIC interface](#rfic).

### Limits {#links-usb3-limits}

- **Combinations.** USB3 shares a bitstream with PCIe x1 only. The script refuses it with
  Ethernet, SATA or White Rabbit, on the `m2` variant and with the oversampling mode.
- **Timed TX.** The TX header and hardware timed TX work over PCIe only. The library refuses
  the TX header on this link, and the SoapySDR driver uses its software timeline.
- **Overflow and underflow.** The link reports no RX overflow and no TX underflow. The FIFOs
  absorb the jitter of the host; a host that stalls for longer loses samples without a count.
- **LED.** The state of the USB3 link is not part of the "ready" condition of the status LED
  ([Status LED](#soc-led)).

### Debug registers {#links-usb3-debug}

`--with-usb3-debug` adds the debug registers of USB3-PIPE to the `usb3` block: the state and the
history of the link training, the error counters of the receiver, the frequency meters and the
DRP port of the transceiver, and the recovery causes of the device core.

These registers follow the two registers of the link, so those keep their addresses. They are
not in the committed `csr.h`: read them by name with the register file of the build, over PCIe,
JTAGBone or the USB bridge itself. The script `bench/usb3_debug.py` of the USB3-PIPE repository
decodes them, and the USB3-PIPE manual describes them.

In the code: `litex_m2sdr/gateware/usb3.py`, and `add_usb3()` with the crossbar connections in
`litex_m2sdr.py`. The host side is `libliteusb` and the USB backend of the library, described
in [Host software](#software); the commands are in [USB3 streaming](#flow-usb3).

## Checking the links without hardware {#links-tests}

The Python tests of the repository cover the modules of this chapter and of
[Time synchronisation](#timing) in simulation, and elaborate the SoC for the main combinations.
They need no FPGA tool and no board.

| Tests | What they check |
|------------------------------------------|----------------------------------------------------------|
| `test_qpll.py` | Channel order, settings, line rates, rejected cases |
| `test_pcie_workaround.py` | Reset sequence until link up |
| `test_shared_etherbone.py` | A reply goes to the source port of its request |
| `test_vrt.py` | VRT header fields, packet count and framing |
| `test_sata_streamers.py` | Recorder and player: bursts, refill, pacing, stop |
| `test_sata_stream_tap.py` | Duplication of the stream with the tap |
| `test_usb3_streamers.py` | USB3 streamers: word order, drain when disabled, priming |
| `test_soc_usb3.py` | Elaboration with USB3, alone and with PCIe; rejected cases |
| `test_soc_eth_sata.py` | Elaboration of the SATA SoC with Ethernet and with PCIe |
| `test_soc_white_rabbit.py` | Elaboration with White Rabbit (needs LiteX-WR-NIC) |

The capture below runs the tests of both chapters. `PYTHONPATH` points to a checkout of
LiteX-WR-NIC at the commit that the continuous integration of the repository uses; without it
the White Rabbit tests are skipped, or fail with an older checkout.

```console
$ PYTHONPATH=/path/to/litex_wr_nic python3 -m pytest -q -n 8 \
    test/test_qpll.py test/test_pps.py test/test_si5351.py test/test_vrt.py \
    test/test_clock_discipline.py test/test_ptp_discipline.py \
    test/test_ptp_identity.py \
    test/test_sata_streamers.py test/test_sata_stream_tap.py test/test_shared_etherbone.py \
    test/test_usb3_streamers.py \
    test/test_pcie_workaround.py test/test_pcie_time_sync_script.py \
    test/test_ptp_check_script.py test/test_soc_eth_ptp.py test/test_soc_ptm.py \
    test/test_soc_eth_sata.py test/test_soc_usb3.py test/test_soc_clk10_discipline.py \
    test/test_soc_white_rabbit.py test/test_time_core.py
[...]
119 passed in 16.66s
```

## Where to go next {#links-next}

[Time synchronisation](#timing) for PTM, PTP and White Rabbit on these links;
[Streaming](#streaming) for the streams behind the crossbar; [Workflows](#workflows) for the
recipes; [Building the gateware](#building-gateware) for the build commands.
