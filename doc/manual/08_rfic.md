# The RFIC interface {#rfic}

This chapter describes the gateware between the sample streams of the SoC and the pins of the
AD9361: the control pins, the SPI master, the LVDS data port, the transport formats and the test
features. It also documents the oversampling mode, which is Experimental.

Read it to understand what a sample word contains, or before working on the RFIC interface. The
AD9361 itself is introduced in the [SDR primer](#sdr-primer).

## At a glance {#rfic-glance}

| Item                    | Value                                                                |
|----------------------------------|--------------------------------------------------------------------------------------------------------------------|
| Top-level class         | `AD9361RFIC` (`litex_m2sdr/gateware/ad9361/core.py`)                 |
| Data port               | LVDS, dual port, full duplex, 6 data pairs per direction             |
| Sample width            | 12 bits, two's complement                                            |
| Stream word             | 64 bits: I and Q of two channel slots, 16 bits each                  |
| Transport formats       | SC16, SC8, BFP8                                                      |
| Sample rate             | Up to 61.44 MSPS, 2T2R, 12-bit                                       |
| Oversampling mode       | Experimental: 122.88 MSPS, in 2T2R with `--with-rfic-oversampling`   |
| Configuration           | SPI master driven from registers; the AD9361 driver runs on the host |
| Test features           | PRBS generator and checker, PHY loopback, saturation counters        |

## Overview {#rfic-overview}

The RFIC core has one stream in each direction on the SoC side and the LVDS pins on the other
side. Between them, the samples change format, change clock domain and are serialised.

The figure below shows the stages. The TX stream crosses them from left to right, the RX stream
from right to left. The stages on the left run on the system clock, the stages on the right on
the clock that the AD9361 sends with its data.

![Stages of the RFIC core. TX samples enter on the left and reach the AD9361 on the right; RX
samples take the same stages in the other direction. The FIFO stage only exists in the
Ethernet-only builds. The converters of the oversampling build, between the FIFO and the GPIO
bits, are not drawn.](rfic_core.svg)

| Stage          | Role                                                              | Class                     |
|--------------------|---------------------------------------------------------|-------------------------------------------------------------------------|
| Buffer         | Register stage that eases timing                                  | LiteX `stream.Buffer`     |
| Bit mode       | Converts between the transport format and 12-bit samples          | `AD9361TXBitMode`, `AD9361RXBitMode` |
| CDC            | Crosses between `sys` and `rfic`                                  | LiteX `ClockDomainCrossing` |
| FIFO           | Absorbs the bursts of an Ethernet link                            | LiteX `SyncFIFO`          |
| Converters     | Oversampling build only: split a 128-bit word into two groups     | LiteX `stream.Converter`  |
| GPIO bits      | Extracts or inserts the GPIO bits of the samples                  | `GPIOTXUnpacker`, `GPIORXPacker` |
| PHY            | Serialises and captures the LVDS pairs                            | `AD9361PHY`               |
| PRBS           | Replaces the TX samples by a test sequence, checks the RX samples | `AD9361PRBSGenerator`, `AD9361PRBSChecker` |

The gateware holds no knowledge of the registers of the AD9361. The chip is configured by the
host: the library contains the AD9361 driver and reaches the chip through the SPI master of the
FPGA. [Software](#software) describes that side.

## Control pins and SPI {#rfic-control}

The AD9361 has a few static control pins and a SPI port. Both are driven from registers.

| Register          | Field     | Pin or use                                                 |
|------------------------------------------------------|----------------------------|--------------------------------------------------------------------|
| `ad9361_config`   | `rst_n`   | Reset of the AD9361, active low. Resets to 0: the chip is held in reset after configuration of the FPGA |
| `ad9361_config`   | `enable`  | `ENABLE` pin                                               |
| `ad9361_config`   | `txnrx`   | `TXNRX` pin                                                |
| `ad9361_config`   | `en_agc`  | `EN_AGC` pin                                               |
| `ad9361_ctrl`     | `ctrl`    | Four `CTRL_IN` pins                                        |
| `ad9361_stat`     | `stat`    | Eight `CTRL_OUT` pins, read-only                           |

The SPI master sends frames of 8, 16 or 24 bits, most significant bit first, with the clock idle
low and the data sampled on the falling edge (`AD9361SPIMaster`). Its clock is the system clock
divided by 8.

A transfer takes four register accesses, in the order of the table.

| Step | Register             | Access                                         |
|------------|--------------------------------------------|-------------------------------------------------------------|
| 1    | `ad9361_spi_mosi`    | Write the bits to send                         |
| 2    | `ad9361_spi_control` | Write the length and the start bit             |
| 3    | `ad9361_spi_status`  | Poll the `done` bit                            |
| 4    | `ad9361_spi_miso`    | Read the bits returned by the chip             |

Three more registers are plain storage. The library writes the RF settings it applied there, so
that a second program, such as the SATA recorder, can describe a stream it did not configure. The
gateware does not use their values.

| Register                         | Content written by the library   |
|-----------------------------------------------------------------|----------------------------------------------|
| `ad9361_active_sample_rate`      | Sample rate                      |
| `ad9361_active_rx_frequency_khz` | RX frequency                     |
| `ad9361_active_bandwidth`        | Bandwidth                        |

These three registers sit before the SPI registers in the block, which moved the SPI registers:
see [Locations that move](#soc-csr-moving).

## LVDS data port {#rfic-lvds}

The AD9361 exchanges samples with the FPGA over a source-synchronous LVDS port. In each
direction there are a clock pair, a frame pair and six data pairs. The gateware uses the dual
port full duplex mode of the chip: RX and TX have their own pairs and run at the same time.

| Signal      | Direction     | Role                                       |
|----------------------------------------|-----------------------------------------|---------------------------------------------------------------------|
| `DATA_CLK`  | AD9361 to FPGA | Clock of the RX pairs; source of the `rfic` clock domain |
| `RX_FRAME`  | AD9361 to FPGA | Marks the start of an RX word              |
| `RX_D[5:0]` | AD9361 to FPGA | RX data, double data rate                  |
| `FB_CLK`    | FPGA to AD9361 | Clock of the TX pairs, a copy of `DATA_CLK` |
| `TX_FRAME`  | FPGA to AD9361 | Marks the start of a TX word               |
| `TX_D[5:0]` | FPGA to AD9361 | TX data, double data rate                  |

A 12-bit sample does not fit on six pairs, so it is sent in two halves, the upper six bits
first. The I half travels on the rising edge of the clock and the Q half on the falling edge.
One I/Q sample therefore takes two clock cycles.

The figure below shows the two layouts that the PHY supports. In 2R2T mode a word of four cycles
carries one sample of each channel, and the frame signal is high during the first channel. In
1R1T mode the same four cycles carry two successive samples of the single channel, and the frame
signal toggles every cycle.

![Content of the six data pairs over four `DATA_CLK` cycles. In 2R2T mode the two slots of a
word are the two channels; in 1R1T mode they are two successive samples of one
channel.](rfic_interface.svg)

The PHY always works on a group of four values, called `ia`, `qa`, `ib` and `qb`: slot a is the
first half of the word and slot b the second. The meaning of the slots follows the mode, and
this carries through to the host: a 64-bit stream word holds two channels in 2T2R and two
successive samples in 1T1R.

The mode is the `mode` bit of the register `ad9361_phy_control`: 0 for 2R2T, 1 for 1R1T. A
configuration with one receiver and two transmitters, or the reverse, uses the 2R2T mode and
ignores the unused slot.

## Sample rate and RFIC clock {#rfic-rates}

The frequency of `DATA_CLK` follows from the layout above: a word takes four cycles.

| Mode  | Samples per word       | `DATA_CLK` for a sample rate *fs* | `DATA_CLK` at 61.44 MSPS        |
|-----------------|-------------------------------|-----------------------------------------------|------------------------------------------------------|
| 2R2T  | 1 per channel          | 4 × *fs*                          | 245.76 MHz                      |
| 1R1T  | 2                      | 2 × *fs*                          | 122.88 MHz                      |

The AD9361 is specified up to 61.44 MSPS, and the standard PHY is constrained for the `DATA_CLK`
of this rate in 2R2T mode, 245.76 MHz. The sample rates above 61.44 MSPS belong to the
[oversampling mode](#rfic-oversampling), which is Experimental.

## Standard PHY {#rfic-phy}

The standard PHY is deliberately simple: it has no delay element and no calibration logic
(`AD9361PHY`). The AD9361 can shift its clock and data in small steps on both directions, and
the library programs these delays. The FPGA only has to capture and drive on its clock edges.

On the RX side, `DATA_CLK` goes through a global clock buffer and becomes the `rfic` clock.
Double data rate input cells capture the frame and the six data pairs. A two-bit counter, set
by the rising edge of the frame signal, tells which half of which slot is on the pins, and the
halves are assembled into a group.

The RX side cannot be paused. A group is valid for one cycle every four cycles, whether the
stages after the PHY are ready or not. When the host does not read fast enough, groups are
lost. The gateware does not count them; the host sees the overflow of its DMA ring, or a gap in
the RX timestamps.

On the TX side, a free-running two-bit counter paces the serialiser. The PHY accepts one group
every four cycles and sends its four halves through double data rate output cells, with
`TX_FRAME` and `FB_CLK` generated the same way.

When no group is available at the moment the PHY needs one, it sends zeros. The transmitter
then outputs no signal instead of repeating the last sample. The timed TX gate relies on this:
a buffer that waits for its time leaves the RF output silent.

## Stream word and transport formats {#rfic-formats}

The LVDS port always carries 12-bit samples. What changes is the way a group travels between the
FPGA and the host: this is the transport format, selected by the `mode` field of the register
`ad9361_bitmode`.

| `mode` | Format | Bytes per sample and channel | Content                                          |
|------------------|-----------------------|--------------------------------|-----------------------------------------------------------------------------|
| 0      | SC16   | 4                            | 12-bit values, sign-extended to 16 bits          |
| 1      | SC8    | 2                            | Values rounded to 8 bits                         |
| 2      | BFP8   | 2.016                        | 8-bit mantissas with one exponent per block      |

### SC16 {#rfic-sc16}

SC16 is the native format. A 64-bit stream word is one group: `ia` in bits 15:0, `qa` in bits
31:16, `ib` in bits 47:32 and `qb` in bits 63:48. On RX each 12-bit value is sign-extended to 16
bits. On TX the PHY uses bits 11:0 of each field and ignores the rest.

The value range is that of a 12-bit number, -2048 to 2047, also written Q11. The four upper bits
of each field are free, and the [GPIO](#soc-gpio) block can use them.

### SC8 {#rfic-sc8}

SC8 halves the size of the samples for the links whose rate is the limit, Ethernet first. A
64-bit stream word then holds two groups of four bytes.

On RX each 12-bit value is rounded to its upper 8 bits. The rounding is to the nearest value,
ties away from zero, and the result saturates at 127 instead of wrapping. On TX each byte is placed in bits 11:4 of the 12-bit sample and the four lower bits
are zero.

Rounding instead of truncating matters: it removes the offset that truncation adds to the signal
and gains several decibels of signal to noise ratio, as the comparison below shows.

### BFP8 {#rfic-bfp8}

BFP8 is Experimental, see [Status](#intro-status).

SC8 keeps the 8 upper bits whatever the signal level, so a weak signal loses most of its
resolution. BFP8, for block floating point, keeps the 8 most useful bits of each block of
samples. The gateware measures the largest magnitude of the block and derives a shift of 0 to 4
bits, the exponent, shared by the whole block.

A block is 128 words of 64 bits, 1024 bytes: one header word followed by 127 payload words. A
payload word carries two groups, so a block covers 254 groups. The header word has the layout
below.

| Bits   | Field          | Value                                             |
|-------------------------------|-----------------------------------------------|------------------------------------------------------------------------|
| 31:0   | Magic          | `0x38504642`, the characters "BFP8"               |
| 35:32  | Exponent       | 0 to 4                                            |
| 39:36  | Reserved       | 0                                                 |
| 47:40  | Payload words  | 127                                               |
| 55:48  | Sequence       | Block counter, wraps at 256                       |
| 63:56  | Version        | 1                                                 |

On RX the gateware stores the 254 groups of a block in a FIFO while it tracks the largest
magnitude, then sends the header and the mantissas. A mantissa is the sample shifted right by the
exponent, rounded and saturated to 8 bits.

On TX the gateware reads the exponent of each header and shifts the mantissas left by it. A
decoded sample is thus the mantissa multiplied by two to the power of the exponent.

A block costs 8 bytes of header for 1016 bytes of samples, hence 2.016 bytes per sample and
channel. A buffer of 8192 bytes holds exactly 8 blocks.

Since the blocks have their own alignment, BFP8 is not combined with the 16-byte header of the
[streaming](#streaming-header) path, and the host handles a block as one unit: the library exposes
BFP8 as encoded blocks and leaves the decoding to the application.

BFP8 exists on the 64-bit datapath only. The oversampling build accepts SC16 and SC8.

> Do not select BFP8 on an oversampling build. With `mode` at 2 the 128-bit bit mode stage stops
> both streams, and neither the gateware nor the library rejects the setting.

### Comparison {#rfic-format-loss}

The script `scripts/evaluate_sample_formats.py` models the three formats on a sine wave and
reports the signal to noise ratio of the quantised signal at several amplitudes. Its output is
reproduced below with the header abridged: the four last columns are the loss against SC16 at 0,
-6, -20 and -40 dBFS, and the separator row is removed.

```console
$ python3 scripts/evaluate_sample_formats.py
| Format | B/complex | SNR 0 dBFS | SNR -6 dBFS | SNR -20 dBFS | SNR -40 dBFS | Loss [...]
| SC16/Q11 | 4.000 | 73.8 | 68.0 | 54.0 | 33.7 | 0.0 | 0.0 | 0.0 | 0.0 |
| SC8 trunc | 2.000 | 44.3 | 38.3 | 24.3 | 4.2 | 29.5 | 29.7 | 29.6 | 29.6 |
| SC8 rounded | 2.000 | 49.1 | 44.1 | 29.7 | 11.1 | 24.7 | 23.9 | 24.2 | 22.7 |
| BFP8 | 2.016 | 49.1 | 44.1 | 45.4 | 33.7 | 24.7 | 23.9 | 8.6 | 0.0 |
```

The figure below plots the same model. It is regenerated with the `--plot` option of the script.

![Model of the transport formats: signal to noise ratio against the input level, loss against
SC16, and bytes per sample. BFP8 follows SC8 near full scale and recovers the resolution of SC16
for weak signals.](rfic-transport-format-loss.png)

The choice follows from these curves:

- SC16 whenever the host link has the rate for it.
- SC8, rounded, when the rate of the link is the limit, as on Ethernet.
- BFP8 when weak signals matter more than the small overhead of the block headers.

The rates each host link can carry in each format are in [Host links](#host-links).

## Stream FIFOs {#rfic-fifos}

A PCIe link delivers samples smoothly enough for the small FIFO of the clock domain crossing.
An Ethernet link delivers them in packets. The builds with Ethernet and without PCIe therefore
add two FIFOs of 8192 words in the `rfic` domain, one per direction.

The TX FIFO also delays the start of the transmission: the PHY receives its first group when the
FIFO is half full, and the FIFO must empty completely before this start condition applies again.
The margin protects the transmitter against the jitter of the packets.

## Oversampling mode {#rfic-oversampling}

> **Experimental.** The oversampling mode is Experimental, see [Status](#intro-status). It runs
> the AD9361 outside its specification and has the limits listed in this section.

The AD9361 is specified up to 61.44 MSPS, with baseband filters limited to about 56 MHz. The
oversampling mode runs it at sample rates between 61.44 and 122.88 MSPS, with a wider
analog bandwidth. This section explains how, and what the `--with-rfic-oversampling` build adds.

### Principle {#rfic-oversampling-principle}

When the host asks for a sample rate above 61.44 MSPS, the library selects the wide bandwidth
configuration of the AD9361 by itself. Two things change in the chip:

- Half-band filter stages of the converters are bypassed, so the data port runs at twice the
  rate of the clock chain of the chip.
- The analog baseband filters of RX and TX are tuned again, by the calibration of the chip, to a
  wider corner frequency. The target is a corner of about 54/62 MHz, which the library obtains
  by programming the tune dividers past the bandwidth clamp of the AD9361 driver. The measured
  analog bandwidth is about 100 MHz.

The `DATA_CLK` frequency then depends on the channel configuration, with the factors of
[Sample rate and RFIC clock](#rfic-rates).

| Configuration at 122.88 MSPS | `DATA_CLK`   | Rate of a data pair | Gateware                        |
|----------------------------------|----------------------------|-----------------|-----------------------------------------------------------------------|
| 1T1R                         | 245.76 MHz   | 491 Mbps            | Standard build                  |
| 2T2R                         | 491.52 MHz   | 983 Mbps            | `--with-rfic-oversampling`      |

1T1R at 122.88 MSPS therefore needs no special bitstream. 2T2R at 122.88 MSPS needs the
oversampling build and a host link that carries the samples, PCIe Gen2 with 2 or 4 lanes. The
library checks the `rfic_oversampling` capability bit and refuses 2T2R above 61.44 MSPS on a
standard bitstream, with a message, instead of configuring an interface that cannot align.

### Oversampling PHY {#rfic-oversampling-phy}

At 983 Mbps per pair, the assumptions of the standard PHY no longer hold. The oversampling PHY
keeps the same stream interface and the same word layout, and changes the cells at the pins
(`AD9361OversamplingPHY`). The figure below shows the two directions.

![Oversampling PHY. TX groups cross a small FIFO to the divided clock of the serialisers; each
RX pair has its own delay element and an error counter used by the host to align
it.](rfic_oversampling.svg)

**TX serialisers.** A double data rate output cell clocked from the FPGA fabric does not produce a
clean enough eye at 983 Mbps: the AD9361 then separates the two channels wrongly, and a clean
single-channel tone splatters to *fs*/2. The six data pairs and the frame pair are driven by 8 to 1
serialisers instead (`OSERDESE2`).

Two other causes were ruled out. It is not a limit of the chip: 1T1R transmits at 122.88 MSPS,
with pairs at 491 Mbps. It is not skew between the pairs either: a clock phase per pair does not
recover the tone.

A dedicated MMCM derives their two clocks from `DATA_CLK`: the bit clock at 491.52 MHz and the
load clock at 122.88 MHz. One load carries the eight half-cycles of a group, in the same order as
the standard PHY sends them.

**Group FIFO.** The groups are produced in the `rfic` domain and loaded on the divided clock. The
two clocks have a fixed frequency ratio but an arbitrary phase, which changes with each lock of
the MMCM.

A direct register transfer could then capture a group while it changes. The groups cross through a
small asynchronous FIFO of 8 entries, which makes the transfer independent of the phase.

**Common reset.** A serialiser fixes its load phase when its reset is released. The seven
serialisers therefore share one reset, synchronous to the load clock, so that they all start in
the same cycle. With an asynchronous reset, each serialiser could leave reset on a different edge
of the load clock, and one pair could come up a full bit period away from the others.

The symptom depends on the build: one lane with a deterministic 100 % error rate that no delay
tap can fix.

**RX delays.** The skew between the six RX pairs of the board is larger than the bit period at 983
Mbps, and the delay registers of the AD9361 move all pairs together.

Each RX pair therefore has its own delay element with 32 steps (`IDELAYE2`), loaded from a
register. This is why the build instantiates the delay calibration block of the FPGA.

**Lane error counters.** To align a pair the host needs a quality measure per pair. The AD9361
can send a test sequence in which both channel slots hold the same word. The PHY compares the
two slots bit by bit and counts, for each pair, the words in which its bits differ.

**Slot rotation.** The frame pair has no delay element. Its skew can place the frame edge one
cycle away from the data, which rotates the slots of a word, an error that the per-pair
comparison cannot see. A two-bit field shifts the slot decoding by 0 to 3 cycles.

| Register                      | Field             | Role                                            |
|----------------------------------------------------------------------------|-------------------------------------------|------------------------------|
| `ad9361_phy_control`          | `mode`            | 0 for 2R2T, 1 for 1R1T                          |
| `ad9361_phy_control`          | `rx_frame_offset` | Slot rotation, 0 to 3 cycles                    |
| `ad9361_phy_rx_deskew_idelay` | `value`, `lane`   | Loads a delay step, 0 to 31, on one RX pair     |
| `ad9361_phy_rx_deskew_ctrl`   | `latch`           | Latches the six error counters and clears them  |
| `ad9361_phy_rx_deskew_err0` to `err5` | 24 bits   | Latched error count of each RX pair             |

The oversampling PHY has two limits of its own:

- It transmits at 2T2R and 122.88 MSPS only. The MMCM of the serialisers locks at the 491.52 MHz
  `DATA_CLK` and not at lower frequencies. Reception works at every rate. For transmission at
  lower rates, use a standard bitstream.
- It has no PHY loopback. The `loopback` bit of `ad9361_phy_control` is kept so that the other
  fields do not move, and has no effect.

### 128-bit datapath {#rfic-oversampling-datapath}

2T2R at 122.88 MSPS is 122.88 million groups per second. A 64-bit stream on the 125 MHz system
clock would have to carry a word on almost every cycle. The oversampling build doubles the
width of the datapath instead: a stream word is 128 bits and holds two groups, so the word rate
is halved.

The change applies from the RFIC core to the DMA: the buffers, the bit mode stage, the clock
domain crossings, the header, the loopback and the crossbar are all 128 bits wide.

Inside the RFIC core, two converters in the `rfic` domain split a 128-bit word into two groups for
the PHY and assemble two groups into a word on RX.

The byte order in the host buffers is unchanged: the first group of a word is in its lower 64
bits. An application sees the same sample layout with both builds.

The 491.52 MHz clock also changes the building blocks:

- The clock domain crossings use an asynchronous FIFO written for this rate, with registered
  flags and a storage made of registers (`ClockDomainCrossingRegistered`). The small
  distributed memories of the FPGA cannot be written at this frequency.
- The PRBS checkers are retimed and have multicycle constraints, since the PHY gives them a
  sample at most every four cycles.
- The clock measurement counts a clock divided by four and adds four per cycle, see
  [Clock measurement](#rfic-measurement).

The constraints are added by `AD9361RFIC.add_timing_constraints()`, which does nothing in a
standard build.

The build command is in [Building the gateware](#building-gateware).

### Limits of the build {#rfic-oversampling-limits}

The oversampling build supports PCIe only. The script does not reject it with `--with-eth` or
`--with-sata`, but the result does not work.

- The Ethernet and SATA streamers and the SATA RX tap are 64 bits wide, so the second group of
  each word, half of the samples, is lost. With SATA the tap is on the RX path to every
  destination, so the PCIe RX stream loses them too.
- The RFIC clock constraint of the Ethernet builds ignores the option.

Three more limits apply on a PCIe oversampling build.

| Limit                        | Effect                                                            |
|-----------------------------------------------|----------------------------------------------------------------------------|
| BFP8                         | Stops both streams, see [BFP8](#rfic-bfp8)                        |
| Saturation counters          | Observe one group out of two, see [Saturation counters](#rfic-agc) |
| Register offsets             | Shifted in the `ad9361` block, see [Locations that move](#soc-csr-moving) |

### Bring-up by the host {#rfic-oversampling-bringup}

The gateware provides the adjustments; the library runs the alignment each time it applies a
configuration above 61.44 MSPS (`m2sdr_apply_config()`). The steps below are in
`litex_m2sdr/software/user/libm2sdr/m2sdr_rf.c`.

1. The library programs the sampling clocks of the AD9361 and enables the wide bandwidth
   configuration.
2. In 2R2T mode it aligns the RX pairs. It enables the test sequence of the AD9361, sweeps the
   delay steps while it reads the lane error counters, and centres each pair in its widest
   error-free window. In 1R1T mode the delays are set to zero.
3. It verifies the interface with the PRBS checker of the gateware, first on the test sequence
   of the AD9361, then on the PRBS of the FPGA returned by the data port loopback of the chip.
   In 2R2T mode it tries the four slot rotations.
4. When the verification fails, it programs the clocks again, which changes the framing phase of
   the interface, and repeats from step 2. The loop is limited to 20 passes.
5. In 2R2T mode it checks the TX framing. The AD9361 locks its TX channel separation on one of
   several phases at each start and some of them are wrong. The library sends the PRBS of the
   FPGA through the data port loopback of the chip and reads the lane error counters: a wrong
   phase breaks the sequence. In that case it initialises the chip again and repeats the whole
   configuration, at most 12 times.

Steps 4 and 5 recover differently because the two framings do. Programming the clocks again
changes the RX framing phase but not the TX framing: only a full SPI initialisation, with the
GPIO reset masked, establishes the TX framing again.

The check of step 5 is safe for a good lock. The loop test only disturbs the RX capture, which
the library aligns again after it, and it preserves the TX lock.

The maintainers measured the usual cost of these loops on one board: one or two passes of the
clock programming, about 20 % of wrong TX phases, hence no or one new initialisation in most
cases and four at most in the observations.

No cable is needed for these checks: both loopbacks are inside the AD9361. An application only
asks for the sample rate and the channel configuration. The recipe is in
[Workflows](#workflows).

The delay elements add a delay to the RX pairs even at step zero, and they keep their steps
after a 2R2T configuration. On an oversampling bitstream the library therefore also acts at
61.44 MSPS or less: it clears the six delay steps and programs interface delays of the AD9361
that were calibrated for this PHY.

### Measured performance {#rfic-oversampling-performance}

The values below were measured by the maintainers on one board, in the wide bandwidth
configuration at 122.88 MSPS. They are a record of one setup, not a specification.

The measurement was taken at 3.6 GHz on the internal reference. The error vector magnitude uses a
50 MHz 5G NR test signal (NR-FR1-TM3.1, 64QAM) and is computed with the MATLAB 5G Toolbox. The
in-band signal to noise ratio uses the notch method, also called NPR. The RX figures use a clean
external transmitter.

| Quantity                                   | Measured                              |
|-----------------------------------------------------------------------------------|-------------------------------------------------------------------|
| Usable analog bandwidth                    | About 100 MHz, full quality to ±40 MHz |
| RX EVM, 50 MHz signal centred              | About 6.0 to 6.6 % RMS                |
| RX EVM, 50 MHz signal shifted by ±25 MHz   | About 8.7 to 9.2 % RMS, the signal touches the band edge |
| RX in-band SNR, ±10 to ±31 MHz             | 26 to 29.5 dB                         |
| RX in-band SNR at ±41 and ±45 MHz          | About 21 and 17 dB                    |
| RX passband flatness with equaliser FIR    | ±1.2 dB to ±44 MHz                    |
| RX floor without signal, RX gain 55        | -35.6 dBFS integrated                 |
| RX image rejection, quadrature tracking on | About 40 to 43 dBc                    |
| TX EVM, 50 MHz signal, attenuation 0 to 18 dB | About 5.2 to 6 % RMS               |
| TX image rejection, attenuation 0 dB       | About 44 dBc                          |
| TX passband flatness with TX FIR equaliser | ±1.3 dB to ±42 MHz, from -8 dB without it |

A few facts help to read these figures:

- The RX EVM is lowest when the converter is filled to about -12 dBFS RMS at the lowest gain
  that reaches this level. The noise floor at the band edges does not depend on the gain.
- The TX EVM does not depend on the drive level over the attenuation range of the table.
- The remaining limit on both sides is the phase noise of the local oscillators, which comes
  from the reference chain: the SI5351 multiplies by 94 into the RF synthesisers. Beyond about
  ±40 MHz on RX, the limit is the shaped noise of the converter at this oversampling ratio.
  Both are limits of the chip and of the architecture, not of the configuration.
- The TX flatness over the full 100 MHz is obtained in the chip, with a 16-tap TX FIR used as an
  equaliser. A real-time transmitter needs no pre-emphasis on the host; waveform pre-emphasis
  remains available as an alternative.
- The best EVM is obtained on the internal reference. An external 10 MHz reference costs about
  6 % of EVM, because the SI5351 then multiplies by a larger factor, N = 84 instead of N = 34.

For 2T2R at 122.88 MSPS, the maintainers measured on an analog TX to RX loopback that the TX tone
of the second channel is recovered at -0.1 dB of the total power, in SC8 and in SC16, as with the
61.44 MSPS reference. The result was obtained reliably across fresh bring-ups.

The oversampling build of that record met timing at 491.52 MHz with a positive slack of about
0.016 ns.

### Tuning variables {#rfic-oversampling-tuning}

The library reads a few environment variables for tuning and diagnostics. Their defaults are
the measured optimum and normal use does not need them.

| Variable                     | Effect                                                         |
|------------------------------------------------------------------------------|------------------------------------------------------------------------|
| `M2SDR_OC_BBF_TUNE`          | Corner frequency target of the baseband filter tuning, about 54/62 MHz by default |
| `M2SDR_OC_BBF_FORCE`         | Forces the widening by direct register writes                  |
| `M2SDR_OC_RX_FIR_FILE`       | Taps of the RX equaliser FIR, 16 at most                       |
| `M2SDR_OC_TX_FIR_FILE`       | Taps of the TX equaliser FIR, 16 at most, designed for the 245.76 MHz rate of the FIR in this mode |
| `M2SDR_QEC_KEXP`             | Loop gain of the RX quadrature tracking                        |
| `M2SDR_RFPLL_CP_PERCENT`     | Scale of the charge pump current of the RX synthesiser         |
| `M2SDR_OC_RX_DELAY`, `M2SDR_OC_TX_DELAY` | Override of the interface delays                   |

## PRBS {#rfic-prbs}

The interface can be tested without any RF signal. The AD9361 and the FPGA both implement the
same 16-bit pseudo-random sequence, of which the 12 lower bits are sent as a sample.

| Register         | Field     | Role                                                              |
|-----------------------------------------------------------|-----------------------------|--------------------------------------------------------------|
| `ad9361_prbs_tx` | `enable`  | Replaces the TX groups by the sequence                            |
| `ad9361_prbs_rx` | `synced`  | 1 when the RX groups follow the sequence                          |

The checker is self-synchronising. It restarts from the seed on each mismatch, and reports
`synced` after 1024 cycles of the RFIC clock without error.

The two PHY modes need different checkers, and the gateware selects the right one from the
`mode` bit. In 2R2T mode each slot carries the whole sequence, and the two slots are checked
separately. In 1R1T mode the two slots carry successive values of one sequence, so a dedicated
checker verifies the pair and advances by two steps per word.

The library uses the PRBS to calibrate the interface delays of the AD9361 and for the
verifications of the [oversampling mode](#rfic-oversampling-bringup).
[Debugging](#debugging) gives the commands.

## Saturation counters {#rfic-agc}

Four counters tell the host when the RX signal gets close to the full scale of the converter.
They help to set the gain, by hand or in a software gain loop (`AGCSaturationCount`).

Each counter watches one channel, RX1 or RX2. It counts the stream words in which the magnitude
of I or of Q reaches a threshold: one event per word, not per sample. Each channel has a low and a high counter, with different
default thresholds.

| Counter                                    | Channel | Default threshold                        |
|------------------------------------------------------------------------------------------|---------------------|--------------------------------------|
| `ad9361_agc_count_rx1_low`                 | RX1     | 1536, 2.5 dB below full scale            |
| `ad9361_agc_count_rx1_high`                | RX1     | 2016, close to clipping                  |
| `ad9361_agc_count_rx2_low`                 | RX2     | 1536                                     |
| `ad9361_agc_count_rx2_high`                | RX2     | 2016                                     |

Each counter has a `control` register with an enable bit, a clear bit and the threshold in bits
31:16, and a `status` register with the 32-bit count. The count is zero while the counter is
disabled and stops at its maximum.

The counters observe the stream after the clock domain crossing, before the bit mode stage, so
they see the 12-bit samples in every transport format. In 1T1R the two slots are successive
samples of the single channel, and the RX1 and RX2 counters then watch the same channel.

In the oversampling build the counters only observe the first group of each 128-bit word. Half of
the samples are not watched, so a short saturation can be missed.

## PHY loopback {#rfic-loopback}

The standard PHY can return the TX groups to the RX side without going through the pins. The
`loopback` bit of `ad9361_phy_control` enables it. The groups cross a FIFO of 16 entries, so
that the pace of the TX serialiser is independent of the reading of the RX stream.

This loopback tests everything in the FPGA down to the PHY, including the clock domain
crossings and the bit mode stage. It needs the AD9361 to run, since the `rfic` clock comes
from the chip. The other loopback points of the sample path are compared in
[Loopbacks](#streaming-loopbacks).

## Clock measurement {#rfic-measurement}

A wrong RFIC clock is the first thing to rule out when samples look wrong. The SoC can measure
the frequency of six clocks against the time of the host (`MultiClkMeasurement`).

Each clock drives a free-running 64-bit counter. A write to a latch register copies the counter
to a value register through a handshake, so the host never reads a value that is changing. The
host latches twice, some time apart, and divides the difference of the counts by the elapsed
time.

| Counter | Clock                         | Expected frequency                              |
|-------------------------------|---------------------------------------------------------|--------------------------------------------------------------|
| `clk0`  | `sys`                         | 125 or 100 MHz                                  |
| `clk1`  | PCIe core clock, 0 without PCIe | 125 MHz                                       |
| `clk2`  | `si5351_clk0`                 | 38.4 MHz, reference of the AD9361               |
| `clk3`  | `rfic`                        | Follows the sample rate                         |
| `clk4`  | `si5351_clk1`                 | 100 MHz, board time clock                       |
| `clk5`  | `clk10`                       | 10 MHz                                          |

`clk_measurement_latch_all` latches the six counters with one write. The script
`scripts/test_clks.py` and `m2sdr_util` use these registers; see [Debugging](#debugging).

## In the code {#rfic-code}

| File                                 | Content                                                      |
|-------------------------------------------------------------------------------------------|-----------------------------------------------------------|
| `gateware/ad9361/core.py`            | `AD9361RFIC`: registers, stages, PRBS, saturation counters   |
| `gateware/ad9361/phy.py`             | `AD9361PHY`, standard PHY                                    |
| `gateware/ad9361/phy_oversampling.py` | `AD9361OversamplingPHY`                                     |
| `gateware/ad9361/spi.py`             | `AD9361SPIMaster`                                            |
| `gateware/ad9361/bitmode.py`         | SC16, SC8 and BFP8 conversions, 64-bit and 128-bit           |
| `gateware/ad9361/prbs.py`            | PRBS generators and checkers                                 |
| `gateware/ad9361/agc.py`             | `AGCSaturationCount`                                         |
| `gateware/cdc.py`                    | Asynchronous FIFO and crossing for the 491.52 MHz clock      |
| `gateware/measurement.py`            | `ClkMeasurement`, `MultiClkMeasurement`                      |
| `test/test_ad9361.py`, `test_agc.py`, `test_spi.py`, `test_cdc.py` | Unit tests                    |

The paths of the gateware files are relative to `litex_m2sdr/`.

## Where to go next {#rfic-next}

[Streaming](#streaming) follows the stream words between the RFIC core and the host, with the
header, the board time and timed TX.
