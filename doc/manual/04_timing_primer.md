# Time in a nutshell {#timing-primer}

This chapter explains why a radio cares about time and frequency, and how a reference reaches a
board. It introduces the reference clock, the 10 MHz and PPS signals, timestamps, and the three
network mechanisms the board supports: PCIe PTM, IEEE 1588 PTP and White Rabbit. The
implementation on the board is in [Time synchronisation](#timing).

## Why time matters {#timingp-why}

A radio depends on its clock in three ways.

**Frequency.** The carrier frequency is a multiple of the reference clock. An error of one part
per million (ppm) on the reference is an error of 1 ppm on the carrier: 2.4 kHz at 2.4 GHz. Two
radios with free-running oscillators never agree exactly on a frequency.

**Rate.** The sample clock comes from the same reference. Two radios with different references
produce samples at slightly different rates, and their streams slide against each other over
time.

**Instant.** Many applications need to know *when* a sample was received, or to decide when a
sample is transmitted. A time-division system transmits in its slot. A direction-finding or
localisation system compares the arrival times of one signal at several receivers.

A single board with its own oscillator is enough to listen to a broadcast. As soon as several
radios work together, or a radio follows a network schedule, they need a common reference.

| Need | Requires |
|-----------------------------------------|-------------------------------------------|
| Tune exactly on a frequency | An accurate reference frequency |
| Keep several receivers sample-aligned | A common reference frequency |
| Know the time of a received sample | A board time, and timestamps |
| Transmit at a chosen instant | A board time, and timed transmission |
| Agree on time between distant boards | A board time locked to a common source |

## One reference clock {#timingp-reference}

Everything that has a frequency in an SDR derives from one reference clock. The figure below
shows the three things it feeds.

![The reference clock of a radio. The sample clock, the local oscillators and the time counter
all derive from it. An error of the reference appears in all three.](timingp_reference.svg)

This has a practical consequence. Locking the reference clock of a board to an external source
corrects the carrier, the sample rate and the pace of the board time at once.

Two words describe what can be shared. Two clocks are *syntonised* when they have the same
frequency: they tick at the same pace. They are *synchronised* when they also agree on the
time: they show the same value at the same instant. Frequency alone is enough for some uses;
others need both.

On the board, the clock generator produces the reference of the RFIC and the clock of the board
time. It runs from its own crystal or from an external 10 MHz reference (see
[Hardware](#hardware)).

## 10 MHz and PPS {#timingp-pps}

The classical way to share a reference between instruments uses two signals on two cables.

| Signal | Carries | Use |
|---------|---------------------------------------|-----------------------------------------|
| 10 MHz | A frequency | Reference of the clock generator: syntonisation |
| PPS | One pulse per second, on the second | Marks an instant: alignment of the time |

A typical source is a GPS-disciplined oscillator. It locks a stable oscillator to the satellite
time and outputs both signals. Every instrument fed by the same source, or by sources locked to
the same satellites, shares frequency and second boundaries.

PPS says where a second starts, not which second it is. The time of day comes from another
channel, usually the host, which only needs to be right within a fraction of a second. The pulse
then gives the precise instant.

On the board, the SI5351C version accepts a 10 MHz reference. A PPS input is available, but
the gateware only monitors it: it detects the edges and reports them in registers. The board
time is not aligned on this input, and the board has no PPS output.

[Hardware](#hardware) gives the pins and their levels, and
[PPS input](#timing-pps-in) the registers.

## Board time and timestamps {#timingp-timestamps}

A *board time* is a counter in the FPGA that counts time in a fixed unit. On the board it is a
64-bit count of nanoseconds. It advances with the clock of the clock generator, by an increment
that has a fractional part, so its pace can be adjusted finely.

A *timestamp* is the value of the board time attached to a buffer of samples. The figure below
shows its two uses.

![Timestamps. Each receive buffer carries the board time of its first sample. A transmit buffer
with a timestamp is held by the FPGA and released when the board time reaches
it.](timingp_timestamps.svg)

On receive, the timestamp of a buffer and the sample rate give the time of every sample of the
buffer. The host can be late to read the buffer; the information stays exact.

On transmit, the host writes the instant at which a buffer must be on the air. The FPGA waits
for that instant. The latency and the jitter of the host and of the host link no longer decide
when the signal leaves: this is *hardware timed TX* (see [Streaming](#streaming)).

Both uses share one counter. "Received at T, transmit at T plus a delay" is therefore exact
arithmetic, which is what a time-division protocol needs.

## Disciplining a clock {#timingp-discipline}

A board time that runs alone drifts with its oscillator. To follow a reference, it is
*disciplined*: a control loop compares it with the reference at regular intervals and corrects
it.

The loop has two tools. A *step* writes a new value in the counter; it is used once, when the
error is large. A *rate adjustment* makes the counter run slightly faster or slower; it removes
small errors smoothly, with no jump in the timestamps.

The same idea applies to a frequency. A loop can steer an oscillator, or the phase of a
generated clock, so that the reference of the radio follows an external one.

When the reference disappears, a disciplined clock keeps its last correction and drifts slowly:
this is *holdover*.

## Two-way time transfer {#timingp-two-way}

A pulse on a cable is simple, but it needs a dedicated cable and it ignores the delay of that
cable. Network mechanisms send the time in messages instead, and measure the delay of the path.
PTM, PTP and White Rabbit all rely on the same exchange, shown in the figure below.

![Two-way time transfer in PTP terms. The master timestamps the departure of Sync (t1) and the
arrival of Delay_Req (t4). The slave timestamps the arrival of Sync (t2) and the departure of
Delay_Req (t3). Follow_Up and Delay_Resp carry t1 and t4 to the
slave.](timingp_two_way.svg)

The slave ends with four timestamps: t1 and t4 read on the clock of the master, t2 and t3 read
on its own clock. If the path has the same delay in both directions, two unknowns remain and two
equations give them:

- delay of the path: ((t2 - t1) + (t4 - t3)) / 2;
- offset of the slave: ((t2 - t1) - (t4 - t3)) / 2.

The slave then corrects its clock by the offset. Two points decide the quality of the result.

- **Where the timestamps are taken.** A timestamp taken in hardware, when the message crosses
  the physical layer, is exact. A timestamp taken by software includes the delays of the
  operating system.
- **Symmetry.** Any difference between the two directions of the path becomes an error of half
  that difference.

## IEEE 1588 PTP {#timingp-ptp}

The Precision Time Protocol (PTP), standardised as IEEE 1588, applies this exchange to a
network. A *grandmaster* is the source of time. The other clocks are *slaves* and discipline
themselves to it. On Ethernet with IP, PTP messages travel in UDP datagrams.

| Term | Meaning |
|--------------------|---------------------------------------------------------------------|
| Grandmaster | The clock that the whole network follows |
| Event messages | Sync and Delay_Req: the messages that are timestamped |
| General messages | Follow_Up and Delay_Resp: the messages that carry timestamps |
| End-to-end delay | The delay is measured between slave and master, as in the figure |
| Peer-to-peer delay | The delay is measured on each link, between neighbours |
| PHC | PTP hardware clock: a clock of a device that Linux exposes to PTP tools |

On a Linux host, the linuxptp project provides the usual tools: `ptp4l` runs the protocol on a
network interface, and `phc2sys` copies the time between a PHC and the system clock.

On the board, PTP runs in the FPGA, inside the Ethernet core, with hardware timestamps. The
board is a slave: a host or a grandmaster on the network gives the time, and the board time
follows it. An optional loop also steers the reference of the RFIC.

## PCIe PTM {#timingp-ptm}

Precision Time Measurement (PTM) is an optional feature of PCIe. It runs the same two-way
exchange on a PCIe link, between a device and the port above it. The messages are handled by
the hardware of both sides, so the timestamps are taken at the link.

The device is the *requester*: it asks, and it obtains the time of the host side together with
the delay of the link. A board in an M.2 slot can then relate its board time to the clock of
the host, with no extra cable.

On the board, the kernel driver exposes the board time as a PHC. The standard tools then
discipline it from the system clock of the host. If the host follows a network time source, the
board follows it too.

## White Rabbit {#timingp-wr}

White Rabbit is an extension of PTP born at CERN for the timing of particle accelerators. It
targets a synchronisation better than a nanosecond over optical fibre, between many nodes and
over kilometres.

It combines three techniques.

1. **Frequency over the link.** Every node recovers the clock of the incoming data and uses it
   as its reference. All nodes are syntonised by the link itself.
2. **PTP.** The two-way exchange gives the coarse offset and the delay.
3. **Phase measurement.** Since both ends run at the same frequency, the remaining offset is a
   phase. A phase detector measures it far more finely than a timestamp counter can.

White Rabbit needs equipment that speaks it: a White Rabbit switch or another node, and optical
SFP modules.

On the board, it runs as a complete White Rabbit core with its own soft CPU and
firmware, on an SFP cage of the baseboard. This integration is Experimental, and its time is
kept apart from the board time that stamps the samples (see [Time synchronisation](#timing)).

## The mechanisms compared {#timingp-compare}

The figure below places the four mechanisms side by side, and the table summarises what each of
them brings.

![Four ways to bring a reference to the board. PCIe PTM and Ethernet PTP end at the board time.
The 10 MHz reference, Ethernet PTP as an option and White Rabbit end at the reference clock.
The PPS input is monitored only and is not drawn.](timingp_sources.svg)

| Mechanism | Medium | Brings | Needs |
|----------------|-----------------|--------------------------|----------------------------------|
| 10 MHz and PPS | Two cables | Frequency; the PPS is monitored only | A reference source; the SI5351C version for 10 MHz |
| PCIe PTM | The PCIe link | Time of the host | A host and a slot that support PTM |
| Ethernet PTP | The Ethernet link | Time, and frequency as an option | The baseboard; a PTP master on the network |
| White Rabbit | Optical fibre | Time and frequency | The baseboard; White Rabbit equipment |

The mechanisms differ in build options and in what they are validated for. The
[status table](#intro-status) gives their state, and [Time synchronisation](#timing) describes
each of them on the board: its gateware, its registers and its limits.

## Where to go next {#timingp-next}

[Hardware](#hardware) shows the clock and PPS connections of the board.
[Time synchronisation](#timing) describes the mechanisms in the gateware, and
[Workflows](#workflows) gives the recipes to set them up.
