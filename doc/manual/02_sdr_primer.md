# SDR in a nutshell {#sdr-primer}

This chapter explains software-defined radio to an FPGA engineer who has never used one. It
follows a signal from the antenna to the samples a host receives: the I/Q representation, the
sample rate, the direct conversion transceiver, the chain of the AD9361 and its data port. It
ends with the part the FPGA plays.

The reference documents of the AD9361 are in `doc/ad9361/`: the AD9361 data sheet, the AD9361
Reference Manual (UG-570) and the AD9361 Register Map Reference Manual (UG-671).

## What software-defined radio means {#sdr-what}

A classical radio does its job with analog circuits: one design per modulation and per band. A
software-defined radio keeps only a generic analog part. It converts a slice of the spectrum to
numbers, and software or gateware does the rest: filtering, demodulation, decoding.

The same hardware therefore receives FM broadcast, tracks satellites or runs a cellular base
station. Only the processing changes. The price is a stream of samples that somebody has to
carry: this is the job of the host links of the board.

| Term | Meaning |
|-------------------|---------------------------------------------------------------------|
| Carrier frequency | Centre of the slice of spectrum that is received or transmitted |
| Baseband | The same slice moved down to 0 Hz, where it is sampled |
| Sample rate | Number of complex samples per second |
| Bandwidth | Width of the slice that the analog filters let through |
| RX, TX | Reception from the air, transmission to the air |

## I/Q samples {#sdr-iq}

A radio signal around a carrier is described by two slow quantities: its amplitude and its phase
relative to the carrier. A single real number per instant cannot hold both. An SDR therefore
gives two numbers per sample: I (in phase) and Q (in quadrature).

I and Q are the two coordinates of a point in a plane. The distance of the point to the origin
is the amplitude of the signal; its angle is the phase. Together they form one complex number,
I + jQ, and a stream of them is the *complex baseband* signal.

This representation has a useful property. A tone above the carrier turns the point one way, a
tone below turns it the other way. Positive and negative frequencies are distinct, so a stream of
complex samples at a rate Fs describes a slice of spectrum Fs wide, centred on the carrier.

On the board, each of I and Q is a 12-bit signed number, as the converters of the AD9361
produce them. The host receives them in 16-bit or 8-bit words; [The RFIC interface](#rfic)
describes these transport formats.

## Sample rate and bandwidth {#sdr-rate}

The sample rate and the bandwidth are two different settings, and both matter.

The sample rate is a property of the numbers: 30.72 MSPS means 30.72 million complex samples per
second for each channel. It bounds the width of spectrum the samples can represent, and it sets
the load of the host link.

The bandwidth is a property of the analog filters in front of the converter. They remove what
lies outside the slice of interest. Without them, signals outside the slice would fold back
inside it when sampled: this is *aliasing*.

A usable setup keeps the bandwidth somewhat below the sample rate, so that the filters have
room to fall. The AD9361 supports channel bandwidths from less than 200 kHz to 56 MHz, and
sample rates up to 61.44 MSPS in its usual operation.

The data rate follows directly. One complex sample of one channel takes 4 bytes in the 16-bit
format. Two channels at 61.44 MSPS therefore produce about 491.5 MB/s in each direction, close
to 4 Gbps. This number explains the interest of a fast host link.
[Carriers and hosts](#carriers) compares it with what one PCIe lane carries.

## The direct conversion transceiver {#sdr-zero-if}

The AD9361 is a *direct conversion* transceiver, also called zero-IF. The receiver moves the
slice of spectrum from the carrier down to baseband in one step, with no intermediate frequency.
The figure below shows the receive path.

![A direct conversion receiver. The local oscillator (LO) is tuned to the carrier. Two mixers,
driven with a 90 degree phase difference, produce the I and Q baseband signals, which are
filtered and sampled.](sdr_receiver.svg)

The receive path works in five steps:

1. A low-noise amplifier (LNA) raises the level of the antenna signal.
2. A local oscillator (LO) produces the carrier frequency.
3. Two mixers multiply the signal by the LO, one directly and one with the LO shifted by a
   quarter of a period. Their outputs are the analog I and Q.
4. Low-pass filters keep the wanted bandwidth, and two converters sample I and Q.
5. Digital filters reduce the sample rate to the one that was asked for.

The transmit path is the mirror image: digital filters, two converters, two mixers, and an
output stage that adds the two products and drives the antenna.

Direct conversion needs few components, which is why it fits in one chip. It has three typical
defects, all visible in a spectrum display.

| Defect | Cause | Sign in the spectrum |
|-------------------|--------------------------------------|---------------------------------|
| DC offset | Offsets of the baseband circuits | A spike at the centre frequency |
| LO leakage | The LO reaches the transmit output | A carrier at the centre of a transmission |
| I/Q imbalance | Gain and phase mismatch of the two paths | A weak mirror image of each signal across the centre |

The AD9361 measures and corrects these defects with calibrations: DC offset calibration and
tracking, and quadrature calibration for the receiver and the transmitter. The driver of the
chip runs them as part of its configuration.

## The AD9361 at a glance {#sdr-ad9361}

| Item | Value |
|-------------------------|--------------------------------------------------------------|
| Channels | Two receivers and two transmitters (2T2R) |
| Receive LO range | 70 MHz to 6.0 GHz |
| Transmit LO range | 47 MHz to 6.0 GHz |
| Channel bandwidth | Less than 200 kHz to 56 MHz |
| Converters | 12-bit ADCs and DACs |
| Duplex | Time division (TDD) and frequency division (FDD) |
| Digital filters | Half-band stages and a programmable 128-tap FIR per direction |
| Gain control | Manual, or automatic with a slow or a fast attack |
| Control | 4-wire SPI port and real-time control pins |
| Data port | 12-bit parallel CMOS or 6-pair LVDS |

These values come from the AD9361 data sheet. The project also has an oversampling mode that
runs the data port above the usual sample rate. It is Experimental (see
[Status](#intro-status)); [The RFIC interface](#rfic) describes it.

## The digital chain {#sdr-chain}

The converters of the AD9361 run much faster than the sample rate the host asks for. A chain of
digital filters links the two rates. The figure below shows it for both directions.

![The digital chain of the AD9361. On receive, each stage filters and divides the rate; on
transmit, each stage multiplies it. The data port carries the samples at the rate of the
application.](sdr_chain.svg)

| Stage | Function |
|-----------|----------------------------------------------------------------------------|
| HB3 | Fixed filter; changes the rate by 2 or 3, or is bypassed |
| HB2, HB1 | Fixed half-band filters; each changes the rate by 2, or is bypassed |
| FIR | Programmable filter of up to 128 taps, with its own rate change |

A *half-band* filter is a low-pass filter made to halve or double a sample rate at a low cost.
The programmable FIR gives the final shape of the channel. Its coefficients are loaded through
the registers of the chip.

All the clocks of this chain come from one synthesizer of the chip, the baseband PLL (BBPLL). It
multiplies the reference clock to a frequency between 715 MHz and 1430 MHz, and dividers produce
the converter and filter clocks from it. A change of sample rate is therefore a change of the
BBPLL and of the rate of each stage.

## LO, gain and channels {#sdr-lo}

The chip holds two more synthesizers: one LO for the receivers and one for the transmitters.
Both are fractional-N PLLs with integrated oscillators and loop filters. The receive and
transmit frequencies are independent, which allows frequency division duplex.

The two receivers share the receive LO, and the two transmitters share the transmit LO. The two
channels of a direction are therefore tuned to the same frequency, with a fixed phase relation.
This is what makes 2T2R useful for multi-antenna techniques.

The gain of a receiver is spread over several analog and digital stages. A gain table of the
chip maps one gain index to the setting of each stage. The index is set by the host in manual
mode, or by the automatic gain control (AGC) of the chip.

| Gain mode | Behaviour |
|-------------------|-------------------------------------------------------------------|
| Manual | The host sets the gain |
| Slow attack AGC | The chip adjusts the gain slowly, for continuous signals |
| Fast attack AGC | The chip adjusts the gain at the start of each burst |

On transmit, the level is set by an attenuation relative to the full output power.

The names of the configurations give the number of active channels: *2T2R* is two transmitters
and two receivers, *1T1R* is one of each. The Analog Devices documents write them 2R2T and 1R1T.

## The LVDS data port {#sdr-lvds}

The samples leave and enter the chip through its data port. The project uses the LVDS mode in
full duplex: each direction has its own clock, its own frame signal and six differential data
pairs. The figure below shows the signals.

![The LVDS data port. The AD9361 provides DATA_CLK with the receive data. The FPGA returns it as
FB_CLK with the transmit data.](sdr_lvds.svg)

| Signal | Direction | Role |
|-------------|--------------|-----------------------------------------------------------|
| `DATA_CLK` | RFIC to FPGA | Clock of the receive data, generated by the RFIC |
| `RX_FRAME` | RFIC to FPGA | Marks the start of each group of receive words |
| `RX_D[5:0]` | RFIC to FPGA | Receive data, 6 bits per clock edge |
| `FB_CLK` | FPGA to RFIC | Copy of `DATA_CLK` returned with the transmit data |
| `TX_FRAME` | FPGA to RFIC | Marks the start of each group of transmit words |
| `TX_D[5:0]` | FPGA to RFIC | Transmit data, 6 bits per clock edge |

The interface is source synchronous: the side that sends the data also sends the clock. Data
changes on both edges of the clock (double data rate).

A 12-bit value does not fit on six pairs, so it travels as two 6-bit words, the upper half
first. With I and Q, a complex sample of one channel takes four words. With two channels, a
frame takes eight words, as the figure below shows.

![The 2R2T interleave on the receive side. Each clock edge carries one 6-bit word. RX_FRAME is
high for the four words of channel 1 and low for the four words of
channel 2.](sdr_interleave.svg)

The clock rate follows from this framing. In 2T2R, eight words per sample on two edges per
period give a clock at four times the sample rate: 245.76 MHz at 61.44 MSPS. In 1T1R, four words
per sample give a clock at twice the sample rate.

The chip can delay its clock and data pins in small steps. These delays place the sampling
point of each side in the middle of the data eye, and a pseudo-random test pattern of the chip
checks the result. The application note AN-1441 of Analog Devices, in `doc/ad9361/`, describes
this calibration.

## What the FPGA does {#sdr-fpga}

The AD9361 is a complete radio, but it only speaks its data port and its SPI port. The FPGA
turns them into something a host can use.

| Task | Why |
|---------------------------|--------------------------------------------------------------|
| Capture and drive the data port | Recover 12-bit I and Q of each channel from the 6-bit words, and the reverse |
| Cross the clock domains | Move the samples between the clock of the RFIC and the clock of the SoC |
| Pack the samples | Build the 16-bit or 8-bit words of the transport format |
| Stamp and gate | Attach the board time to receive buffers; hold transmit buffers until their time |
| Carry the streams | Exchange the buffers with the host over the host link |
| Relay the configuration | Give the host an SPI master to reach the registers of the RFIC |

The configuration of the radio itself stays in software. The driver of the AD9361 runs on the
host, inside the library, and reaches the chip through the SPI master of the FPGA. The FPGA does
not process the samples: filtering and rate conversion happen in the RFIC, modulation and
demodulation on the host.

This split is a starting point. The FPGA has room left, and the SoC provides a hook to insert
user processing between the RFIC and the host (see [Building the gateware](#building-gateware)).

## Where to go next {#sdr-next}

[The RFIC interface](#rfic) describes the gateware that drives the data port.
[Streaming](#streaming) follows the samples to the host. [Host software](#software) covers the
library that configures the radio.
