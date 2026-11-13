# Streaming {#streaming}

This chapter follows the samples between the host links and the RFIC core. It describes the
crossbar, the header that carries the timestamps, the board time, hardware timed TX, the DMA and
its low latency settings, the loopbacks and the hook for user processing. Read it to understand
what a buffer of samples contains and when it is transmitted.

## At a glance {#streaming-glance}

| Item                 | Value                                                                    |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| Stream               | LiteX stream, 64-bit words, `sys` clock domain; 128-bit in the oversampling build |
| Sources and sinks    | PCIe DMA (port 0), Ethernet UDP (port 1), SATA (port 2)                  |
| Buffer               | 8192 bytes by default                                                    |
| Header               | Optional, 16 bytes: sync word and 64-bit timestamp in nanoseconds        |
| Board time           | 64-bit nanosecond counter on a 100 MHz clock, 10 ns step                 |
| Timed TX             | PCIe only: a buffer with a timestamp is held until the board time reaches it |
| Late buffer          | Dropped whole, replaced by zeros, counted                                |
| User processing      | `BaseSoC.add_rfic_datapath()`, between the loopback and the RFIC core    |

## Sample path {#streaming-path}

The samples cross five blocks between a host link and the RFIC core. The figure below shows
them. The TX stream goes from left to right, the RX stream from right to left, and both are
LiteX streams on the system clock.

![Sample path. TX in cyan: the crossbar selects a source, the header block removes the header
and applies the timed gate, then the loopback and the user hook lead to the RFIC core. RX takes
the reverse path and receives its timestamp in the header block.](stream_path.svg)

| Block       | TX direction                                  | RX direction                                 |
|------------------------------|---------------------------------------------------------|--------------------------------------------------------------|
| Crossbar    | Selects one source                            | Selects one destination                      |
| Header      | Removes the header, holds timed buffers       | Inserts the header with the timestamp        |
| Loopback    | Passes, or returns TX to RX                   | Passes, or is replaced by the TX stream      |
| User hook   | Wire by default                               | Wire by default                              |
| RFIC core   | Converts and serialises                       | Captures and converts                        |

The stream between these blocks has no notion of channel or of sample rate. It carries words,
and the pace is set by the RFIC core: the PHY takes and gives one group of samples every four
cycles of the RFIC clock. The content of a word is defined in
[Stream word and transport formats](#rfic-formats).

## Crossbar {#streaming-crossbar}

One RFIC serves up to three sources and destinations, so two registers select which one is
connected. The crossbar is a LiteX stream multiplexer for TX and a demultiplexer for RX.

| Register             | Value | TX source or RX destination |
|----------------------------------------------------------------|-----------------|---------------------------------------------------------------------|
| `crossbar_mux_sel`   | 0     | TX from the PCIe DMA        |
|                      | 1     | TX from Ethernet UDP        |
|                      | 2     | TX from SATA                |
| `crossbar_demux_sel` | 0     | RX to the PCIe DMA          |
|                      | 1     | RX to Ethernet UDP          |
|                      | 2     | RX to SATA                  |

Both registers reset to 0. The port numbers are fixed and match the constants of the software;
a port whose host link is not in the bitstream is left unconnected. The two directions are
independent: a board can receive over PCIe while it plays a recording from SATA.

The library sets the selection when a stream is opened on Ethernet, and the SATA utilities set it
for a recording or a replay. SATA builds add a tap on the RX stream that copies the samples to the
SSD while the host keeps receiving them.

The Ethernet and SATA streamers, with their FIFOs and start conditions, are described in [Host
links](#host-links).

## PCIe DMA {#streaming-dma}

On PCIe the samples are moved by the DMA of LitePCIe, introduced in the
[LiteX primer](#litex-primer). The DMA reader turns buffers of host memory into the TX stream,
and the DMA writer stores the RX stream into buffers of host memory. The SoC enables three
options of the DMA.

| Option        | Setting                 | Role                                                       |
|-------------------------------|----------------------------|-------------------------------------------------------------------------------------------|
| Buffering     | 8192 bytes per direction | FIFO that smooths the gaps of the PCIe bus                |
| Loopback      | Present                 | Returns the reader to the writer, see [Loopbacks](#streaming-loopbacks) |
| Synchronizer  | Present                 | Starts the streams on a pulse of the board time            |

The RFIC cannot wait: its samples leave and arrive at a fixed rate, while the PCIe bus delivers
data in bursts. The buffering FIFO, one buffer deep, absorbs the difference.

The synchronizer gives the streams a defined start. When the kernel driver starts a stream, it
arms the synchronizer, and the DMA stays blocked until the next pulse of the internal PPS
generator, which is derived from the board time.

A first stream therefore starts up to one second after the request, on the pulse that the
generator emits half a second after each second of the board time. The delay has three
conditions.

- A direction started while the other one runs starts at once: the driver does not arm the
  synchronizer again, since this would cut the running stream.
- A TX stream started first also needs data in the DMA at the pulse. Without data it waits for
  the next pulse, one second later.
- The synchronizer has a bypass register that removes the wait. No software of the repository
  enables it.

> The synchronizer depends on the board time. When the time generator is disabled, no pulse
> occurs and the PCIe streams never start.

The opposite case exists too. After a write of the board time, the PPS generator emits one pulse
per cycle of the system clock until it has caught up with the new time. For a UNIX time this
lasts about 14 s at 125 MHz, and a stream armed during this period starts at once, not on a
second.

The width of the DMA stream is 64 bits, or 128 bits in the oversampling build. `--pcie-dmas`
adds DMA channels: channel 0 always carries the samples, and the other channels are free for
the [user hook](#streaming-hook).

Every DMA channel has its own start synchronizer, and the script connects the PPS pulse to all
of them (`test/test_soc_pcie_dmas.py`). An extra channel therefore starts on the pulse like
channel 0.

## Header {#streaming-header}

A stream of samples alone does not tell when a sample was received. The header solves this: it
is a 16-byte record placed at the start of each buffer, optional in each direction.

The figure below shows a buffer with a header and, as a reminder, the two sample word layouts.
All values are little-endian.

![Layout of a buffer with a header, and of a sample word in SC16 and SC8. The header takes the
first 16 bytes; the samples follow in the format of the stream.](stream_header.svg)

| Bytes   | Field      | Content                                                         |
|-------------------------|------------------------------------|-----------------------------------------------------------------------------------------|
| 0 to 7  | Sync word  | `0x5aa55aa55aa55aa5`                                            |
| 8 to 15 | Timestamp  | Board time in nanoseconds, 64 bits                              |

On RX, the timestamp is the board time of the first sample of the buffer. On TX, it is the time
at which the first sample must be transmitted, or 0 for a buffer that is transmitted as soon as
it arrives.

### Frames {#streaming-frames}

The header block does not see buffers. It counts stream words, and a frame is a header followed
by a programmed number of words (`frame_cycles`). The header and the buffer coincide when a
frame is exactly one buffer long.

| Datapath | Header                 | Default `frame_cycles` | Buffer                       |
|-------------------------------|--------------------------|--------------------------------------------------|-------------------------------------------|
| 64-bit   | 2 words                | 1022                   | 1024 words of 8 bytes        |
| 128-bit  | 1 word                 | 511                    | 512 words of 16 bytes        |

The defaults match a buffer of 8192 bytes. The kernel driver accepts other buffer sizes, so the
library computes `frame_cycles` from the size of the buffers of the DMA channel each time it
enables a header. With a wrong value, the headers would drift through the buffers.

The start must be aligned too. On PCIe, the SoC holds each header state machine in reset until
the synchronizer has released the streams and the DMA of that direction is enabled. The first
word after the reset is thus the first word of a buffer.

### RX: inserting the header {#streaming-header-rx}

On RX the block inserts the sync word and the timestamp, then passes the samples of the frame
(`RXHeaderInserter`). The timestamp is the board time at the moment the frame starts, minus a
programmable offset.

The offset exists because the samples are older than the frame. They took a fixed time to cross
the receiver, the converter and the RFIC core. `header_rx_offset` holds this time in
nanoseconds, and the block subtracts it, so that the timestamp refers to the antenna. The
offset resets to 0, and the result is clamped at 0.

While the SoC holds the block in reset, before the stream of the host has started, the block
discards the RX stream. The RFIC core is therefore never blocked by a stream that has not
started.

### TX: extracting the header {#streaming-header-tx}

On TX the block removes the header of each frame and keeps its two fields
(`TXHeaderExtractor`). The samples that follow go to the RFIC core, after the timed gate
described in [Hardware timed TX](#streaming-timed-tx).

> The TX header only works on PCIe. The extractor recognises a header by the first-word marker
> of the DMA reader, which the Ethernet, SATA and USB3 streamers do not provide. With the TX
> header enabled on these links, the extractor would consume every word and discard the TX
> stream. The library therefore refuses it: `m2sdr_set_tx_header()` returns
> `M2SDR_ERR_UNSUPPORTED` when it is enabled on another link than PCIe. The gateware has no
> such check: software that bypasses the library must not set the bit there.

### Registers {#streaming-header-regs}

| Register                   | Role                                                            |
|--------------------------------------------------------------------------------|----------------------------------------------------------------------|
| `header_tx_control`        | `enable` (bit 0, resets to 1), `header_enable` (bit 1, resets to 0) |
| `header_tx_frame_cycles`   | Words per TX frame, header excluded                             |
| `header_rx_control`        | Same fields for RX                                              |
| `header_rx_frame_cycles`   | Words per RX frame, header excluded                             |
| `header_last_tx_header`    | Sync word of the last TX frame                                  |
| `header_last_tx_timestamp` | Timestamp of the last TX frame                                  |
| `header_last_rx_header`    | Sync word of the last RX frame                                  |
| `header_last_rx_timestamp` | Timestamp inserted in the last RX frame                         |
| `header_tx_offset`         | TX pipeline delay in nanoseconds, 64 bits                       |
| `header_tx_underflow`      | Count of timed frames dropped because they were late            |
| `header_rx_offset`         | RX pipeline delay in nanoseconds, 64 bits                       |

With `header_enable` at 0 the block is transparent: the stream has no header and the words pass
unchanged. With `enable` at 0 the state machine of that direction is held in reset and its
stream is stopped: the RX stream is blocked, not discarded, and the TX stream is not consumed.

The last three registers were added with timed TX. They are placed after the older ones, so the
addresses of the older registers did not change, and the `timed_tx` capability bit tells the
software that they exist. Software that does not know them keeps working with this gateware, and
software that knows them keeps working with a gateware that does not have them.

In the library, `m2sdr_set_rx_header()` and `m2sdr_set_tx_header()` enable the headers. The
receive functions return the timestamp in their metadata and can remove the header from the
buffer given to the application.

## Board time {#streaming-time}

The board time is a 64-bit counter of nanoseconds (`TimeGenerator`). Every timestamp of the
board comes from it. The reasons to keep a time on the board are in the
[Timing primer](#timing-primer).

| Item            | Value                                                               |
|-----------------------------|-------------------------------------------------------------------------------------------------------------------------|
| Clock           | `si5351_clk1`, 100 MHz, the `time` clock domain                     |
| Step            | 10 ns per cycle by default, programmable                            |
| Step format     | 32 bits, 8 integer bits and 24 fractional bits of a nanosecond      |
| Value at start  | 0, counting as soon as the clock runs                               |
| Users           | Header, PPS generator, PCIe PTM, PTP discipline, VRT timestamps     |

The counter runs on a clock of the SI5351, the same clock generator that feeds the reference of
the AD9361. The board time and the sample clock therefore share one frequency reference, and a
count of samples and a difference of timestamps agree.

Each cycle, the counter adds the step to a 64-bit nanosecond value extended by a 24-bit
fraction. The default step is exactly 10 ns. A slightly different step makes the time run
faster or slower by a fraction of a part per million, which is how a discipline loop steers it.

### Operations {#streaming-time-ops}

On each cycle of its clock, the time generator does one of the following, by decreasing
priority.

| Priority | Condition               | Effect                                                   |
|------------------------|-------------------------------------------|-----------------------------------------------------------------------------------|
| 1        | Disabled                | Time held at 0                                           |
| 2        | Write                   | Time set to a given value                                |
| 3        | Second alignment        | Time set to the next full second                         |
| 4        | Phase trim              | A given amount added to or subtracted from the time      |
| 5        | Otherwise               | Time advanced by the step                                |

The write and the step can come from the registers or from a discipline block. The phase trim
only comes from a discipline block, the PTP discipline in this SoC; it is described in
[Timing](#timing).

> The second alignment is not available. Its input of the time generator is not connected in
> this SoC, so the `sync_enable` bit of the control register has no effect.

### Registers {#streaming-time-regs}

| Register                    | Role                                                            |
|------------------------------------------------------------------------------|------------------------------------------------------------------------|
| `time_gen_control`          | `enable` (bit 0, resets to 1), `read` (bit 1, pulse), `write` (bit 2, pulse), `sync_enable` (bit 3, no effect) |
| `time_gen_read_time`        | Time captured by the last `read` pulse                          |
| `time_gen_write_time`       | Value loaded by the next `write` pulse                          |
| `time_gen_time_adjustment`  | Offset added to the time that the generator outputs             |
| `time_gen_time_inc`         | Step, resets to `0x0a000000`, 10 ns                             |

The host reads and sets the time in two steps, which the library wraps in one function each.

| Operation | Steps                                                  | Library function    |
|------------|------------------------------------------------------------|----------------------------|
| Read      | Set the `read` bit, then read `time_gen_read_time`     | `m2sdr_get_time()`  |
| Set       | Write `time_gen_write_time`, then set the `write` bit  | `m2sdr_set_time()`  |

The registers are on the system clock and the counter on its own clock. Each 64-bit value
crosses with its strobe through one asynchronous FIFO, so neither side ever uses a value of which
only some bits have changed.

### Use in the system clock domain {#streaming-time-sys}

The blocks that use the board time run on the system clock. The time generator sends each new
value through a small asynchronous FIFO and the SoC uses the copy that comes out
(`TimeGenerator.add_cdc()`).

The copy is late by the delay of the crossing, 2 to 4 cycles of the system clock with a jitter
of one cycle. This delay is part of what the TX and RX offsets calibrate.

The copy also tells when the time was changed by a write. The PPS generator restarts on this
event so that its pulses follow the new time. It then emits a burst of pulses while it catches
up, as [PCIe DMA](#streaming-dma) explains.

## Hardware timed TX {#streaming-timed-tx}

Some applications must transmit at an exact time, a slot of a cellular uplink for instance. The
host cannot do it alone: the delay between a write of the application and the antenna varies by
much more than a sample.

The gateware does it instead. The host writes the transmit time in the header of a buffer, sends
the buffer early, and the FPGA holds it until the board time reaches that time.

### The gate {#streaming-gate}

The gate is part of the TX header extractor. The figure below shows its states for the 64-bit
datapath. With the 128-bit datapath the sync word and the timestamp arrive in one word and the
`TIMESTAMP` state does not exist.

![States of the TX header extractor. After the header, the gate compares the timestamp with the
board time and sends the frame, waits for its time, or drops it. The `RESET` and `IDLE` states,
and the path without header that goes straight to `FRAME`, are not drawn.](stream_gate.svg)

When the timestamp of a frame is known, the gate compares it with the board time plus the TX
offset and takes one of four decisions.

| Timestamp                         | Decision | Effect                                              |
|-----------------------------------------------------|------------------------------|-------------------------------------------------------------------|
| 0                                 | Untimed  | The frame is sent at once                           |
| Later than the board time         | Wait     | The frame is held, then sent when the time is reached |
| Equal to the board time           | On time  | The frame is sent at once                           |
| Earlier than the board time       | Late     | The frame is dropped                                |

There is no tolerance window. A frame that is late by one step of the time is dropped.

**Untimed frames.** The value 0 is reserved to mean "no timestamp". It cannot be a real transmit
time, since the board time starts at 0 and any transmit time of 0 would be in the past. A
continuous stream leaves the timestamps at 0 and crosses the gate without any added delay, so
timed and untimed buffers can be mixed.

**Waiting.** While the gate holds a frame, it sends nothing to the RFIC core, and the PHY
transmits zeros. The RF output is silent until the frame is released.

**Dropping.** A late frame is not sent late. The gate replaces each of its words by zeros, at the
pace of the RFIC, and adds one to `header_tx_underflow`. The frame thus takes the same time as if
it had been transmitted.

Draining it at full speed would empty the buffers of the host faster than real time, and every
following frame would be late in turn.

**The TX offset.** The samples need a fixed time to go from the gate to the antenna, through the
RFIC core, the converter and the transmitter. `header_tx_offset` holds this time in nanoseconds.

The gate adds it to the board time before the comparison, so a frame is released early by this
amount and its first sample is at the antenna at the time of its timestamp.

### Contract with the host {#streaming-timed-tx-contract}

The gate has a simple contract, which the library and the SoapySDR driver implement.

- The TX header must be enabled. Without it the stream has no timestamp. The library refuses a
  timed transmission in that case and returns an error.
- Only the first buffer of a burst carries the transmit time. The following buffers have a
  timestamp of 0 and follow the first one without gap.
- A buffer must reach the gate before its time. How early depends on the host; a buffer that
  arrives too late is counted in `header_tx_underflow`, which `m2sdr_get_tx_underflow()` reads.
- The board time advances in steps of 10 ns, so a transmit time is a multiple of 10 ns. The
  library rounds the requested time to the nearest step (`M2SDR_TX_TIME_GRID_NS`).

This counter is not the underflow of the DMA. The DMA statistics count the buffers that the host
did not provide in time; the gate counts the timed frames that were provided, but too late.

The gate is built in every bitstream and the `timed_tx` capability bit is always set, but timed
TX only works on PCIe. It needs the TX header, which the Ethernet, SATA and USB3 streamers
cannot carry: see [TX: extracting the header](#streaming-header-tx).

The software follows the same limit. The offset and underflow functions of the library return an
unsupported error on another host link, and the SoapySDR driver selects the hardware gate on
PCIe and a software timeline otherwise.

### One reference plane {#streaming-timed-tx-plane}

TX and RX timestamps come from the same counter and use the same header. With the two offsets
set, they also refer to the same place, the antenna. "Received at T, transmit at T plus D" is
then exact arithmetic on one clock.

The two offsets do not have the same importance. A TX offset calibrated over a loopback from TX
to RX absorbs the sum of both delays, which is enough for a board that answers to what it
receives. The RX offset is needed in addition when the timestamps are compared with an absolute
time or between boards.

The TX offset is of the order of a microsecond and depends on the sample rate and on the channel
configuration. The table below gives the delay measured by the maintainers on one board over a
cabled loopback, as mean and standard deviation. The values are those of
`litex_m2sdr/software/soapysdr/README.md`. The 122.88 MSPS row belongs to the experimental
[oversampling mode](#rfic-oversampling).

| Configuration | Sample rate  | Measured delay, mean | Standard deviation |
|----------------------------------------|-------------------------------------|-------------------------------------|-----------------------------------|
| 1T1R          | 30.72 MSPS   | 1358 ns              | 19 ns              |
| 1T1R          | 61.44 MSPS   | 694 ns               | 10 ns              |
| 1T1R          | 122.88 MSPS  | 373 ns               | 5 ns               |
| 2T2R          | 30.72 MSPS   | 1215 ns              | 10 ns              |
| 2T2R          | 61.44 MSPS   | 627 ns               | 8 ns               |

With the calibrated offset programmed, the same measurement gives a remaining error of the
transmit time of 3 ns in mean and 8.5 ns in standard deviation.

These values are examples. The offset must be measured for each board and configuration:
`scripts/timed_tx_selftest.c` does it over a loopback. The procedure, the library calls and the
SoapySDR arguments are in [Workflows](#workflows) and [SDR frameworks](#frameworks).

## Low latency {#streaming-low-latency}

The default settings favour throughput: large buffers, many of them, few interrupts. A
real-time loop, which answers to what it receives within a few hundred microseconds, wants the
opposite. Three settings of the software trade throughput for latency. None changes the
gateware, and the defaults are unchanged for those who do not use them.

### Where the latency comes from {#streaming-latency-source}

On PCIe, the host memory of a DMA channel is a ring of buffers. The kernel driver programs the
addresses of the buffers once and the DMA walks through them in a loop, without waiting for the
host.

On TX, a buffer written by the application is transmitted when the DMA reader reaches it. The
delay is the content of the ring ahead of it, at most the time to drain the whole ring:

```
drain time = buffer count × buffer size / (sample rate × bytes per sample)
```

At 30.72 MSPS in 2T2R and SC16, a sample of both channels takes 8 bytes. The default ring of
256 buffers of 8192 bytes then holds about 8.5 ms of signal, and a ring of 8 buffers about
0.27 ms. In 1T1R, with 4 bytes per sample, these times double.

### The three settings {#streaming-latency-settings}

| Setting           | Where                                  | Effect                                           |
|----------------------|---------------------------------------------------------------------------------|-----------------------------------------------|
| Short ring        | Parameters of the kernel module        | Less signal queued between the host and the RFIC |
| RX wake           | `M2SDR_RX_WAIT=mwaitx`                 | The application wakes when a buffer is written   |
| TX fill lead      | `m2sdr_set_tx_lead_buffers()`          | The application stays a few buffers ahead of the reader |

**Short ring.** The kernel module has three parameters. `dma_buffer_count` is the depth of the
ring, a power of two from 2 to 256. `dma_buffer_size` is the size of a buffer and
`dma_buffer_per_irq` the number of buffers per interrupt; both are given per DMA channel.

The number of buffers per interrupt must divide the depth of the ring and be smaller than it. A
short ring absorbs less jitter of the host, so the application must be real-time.

The per-channel parameters let a second DMA channel, used for a slow side stream, have small
buffers and one interrupt per buffer while the sample channel keeps its settings. The records of
the side stream then become visible to the host after the buffer size divided by the byte rate of
the stream. The constraints are listed in `litex_m2sdr/software/kernel/README.md`.

**RX wake.** By default the library waits for RX buffers with `poll()`, and is woken by the
interrupt of the DMA. With `M2SDR_RX_WAIT=mwaitx`, on a processor that has the MONITORX and MWAITX
instructions, the zero-copy receive path sleeps on the memory of the next buffer instead.

The write of the DMA into this buffer wakes it, without waiting for the interrupt. The wake is
sub-microsecond, with no spinning and no register traffic. In both cases
the library checks the position of the DMA right before it sleeps, so a buffer that is already
complete is returned at once.

**TX fill lead.** By default the zero-copy transmit path fills the whole ring. With
`m2sdr_set_tx_lead_buffers()`, it only stays a given number of buffers ahead of the DMA reader,
and the TX latency becomes about this number of buffers.

The lead must be larger than the number of buffers per interrupt: the library only knows the
position of the reader at each interrupt, and with a smaller lead the reader could pass a buffer
that is being filled. A value of 0 keeps the default behaviour.

The commands, the pinning of the interrupt and the real-time setup of the host are in
[Workflows](#workflows).

## Clock domain crossings {#streaming-cdc}

The sample path and the board time touch three unrelated clocks: `sys`, `rfic` and `time`. Each
kind of signal that crosses has its own block.

| Signal                                 | Block                               | File          |
|-------------------------------|-------------------------------------------------------------------------------|----------------------------------------|
| Sample streams, `sys` and `rfic`       | LiteX `ClockDomainCrossing`         | `ad9361/core.py` |
| Sample streams, oversampling build     | `ClockDomainCrossingRegistered`     | `cdc.py`      |
| Board time to `sys`                    | LiteX `ClockDomainCrossing`         | `time.py`     |
| 64-bit values with a strobe            | `ValueStrobeCDC`                    | `cdc.py`      |
| Single control bits                    | Two-register synchroniser           | Each core     |
| Single pulses                          | Pulse synchroniser                  | Each core     |

**Values with a strobe.** A 64-bit value that crosses bit by bit can be sampled while only some of
its bits have changed. `ValueStrobeCDC` sends the value and its strobe together through one
asynchronous FIFO, so the receiving side gets a whole value and a one-cycle strobe.

The time registers and the clock measurement use it. An option starts a transfer each time the
value changes, without a strobe.

**Fast asynchronous FIFO.** The asynchronous FIFO of the oversampling build is written for a
491.52 MHz clock (`AsyncFIFORegistered`). Its full and empty flags are registers, computed one
cycle early from the next value of the local pointer. The flags are exact for the local side and
see the other side one cycle late, which only makes them conservative.

Its storage can be made of registers instead of a small memory, with a registered read or a
registered write for the side that runs fastest.

All these pairs of clocks are declared asynchronous in the timing constraints, see
[Build name and timing](#soc-build).

## Loopbacks {#streaming-loopbacks}

A loopback returns the TX samples to the RX path at a chosen point. Looping at successive
points, from the host towards the antenna, shows which part of the chain fails. The figure
below places the four loopbacks of the board.

![Loopback points, in cyan, from the host to the AD9361. Each one tests everything on its
left.](stream_loopbacks.svg)

| Loopback   | Location                          | Enabled by                                      | Tests                                    |
|--------------------|--------------------------|---------------------------------------------------------------------------|-----------------------------|
| DMA        | Stream side of the PCIe DMA       | `pcie_dma0_loopback_enable`                     | Host, driver, PCIe link, DMA             |
| Stream     | Between the header and the hook   | `txrx_loopback_control`, bit `enable`           | The above, the crossbar and the headers  |
| PHY        | Pin side of the RFIC core         | `ad9361_phy_control`, bit `loopback`            | The above, formats and clock crossings   |
| RFIC       | Inside the AD9361                 | Register of the AD9361, written over SPI        | The above and the LVDS interface         |

A cable from a TX connector to an RX connector, with an attenuator, is the fifth loopback and
the only one that includes the RF path.

The stream loopback is a block of this chapter (`TXRXLoopback`). When it is enabled, the TX stream
of the header block feeds the RX stream of the header block. The RFIC core receives no TX samples,
and the RX samples of the RFIC are discarded so that the RFIC core is not blocked.

Since it sits on the host side of the user hook, it tests the path without the user processing. It
works with every host link, including Ethernet, where the DMA loopback does not exist.

The PHY loopback is described in [PHY loopback](#rfic-loopback); it does not exist in the
oversampling build. The library has one function per loopback, and `m2sdr_reset_datapath()`
disables them all together with the headers and the PRBS, and returns to SC16. The test procedures are in
[Debugging](#debugging).

## User processing hook {#streaming-hook}

A project that needs processing in the FPGA, a channeliser, a correlator or a repeater from RX to
TX, does not have to copy and edit `litex_m2sdr.py`. `BaseSoC` exposes the place where the samples
can be intercepted as a method that a subclass overrides.

The base class keeps providing the host links, the clocks, the board time and the RFIC interface.

### The hook {#streaming-hook-method}

`add_rfic_datapath()` is called once while the SoC is built. It receives four stream
endpoints, all on the system clock and at the width of the datapath, 64 bits, or 128 bits in the
oversampling build.

| Argument      | Direction seen from the hook | Content                                           |
|---------------------------------------------|----------------------------------|-----------------------------------------------------------------------|
| `tx_source`   | Input                        | TX samples from the host, after the header, the gate and the loopback |
| `rfic_sink`   | Output                       | TX samples to the RFIC core                       |
| `rfic_source` | Input                        | RX samples from the RFIC core                     |
| `rx_sink`     | Output                       | RX samples to the host, before the loopback and the header |

The default method connects `tx_source` to `rfic_sink` and `rfic_source` to `rx_sink`: two
wires, so the design is unchanged when the hook is not used.

The place of the hook has three consequences:

- The stream carries sample words only. The headers are removed before it on TX and added
  after it on RX, so processed RX samples receive their timestamp as usual.
- Timed TX applies before the hook. A delay added by TX processing must be added to the TX
  offset.
- The stream loopback is on the host side of the hook, so the host loopback tests keep running
  on the path without processing.

### Example {#streaming-hook-example}

The example below inserts a block on the RX path and leaves TX untouched. The register stage
stands for the processing.

```python
import importlib.util

from litex.soc.interconnect import stream

# Run from the repository root.
spec  = importlib.util.spec_from_file_location("litex_m2sdr_soc", "litex_m2sdr.py")
m2sdr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m2sdr)

class MySoC(m2sdr.BaseSoC):
    def add_rfic_datapath(self, rfic_sink, rfic_source, tx_source, rx_sink):
        # RX: RFIC -> processing -> host, at the width of the datapath.
        self.dsp = stream.Buffer(rfic_source.description)
        self.comb += [
            tx_source.connect(rfic_sink),
            rfic_source.connect(self.dsp.sink),
            self.dsp.source.connect(rx_sink),
        ]

soc = MySoC(variant="m2", with_pcie=True, pcie_dmas=2)
```

`BaseSoC` is the class of the target script `litex_m2sdr.py`, which is a script and not a module
of the `litex_m2sdr` package. The example therefore loads it from the file, as the tests of the
repository do.

### Topologies {#streaming-hook-topologies}

| Topology            | Connections                                                             |
|--------------------------------------|----------------------------------------------------------------------------------------------------------------|
| RX processing       | `rfic_source` to the block, the block to `rx_sink`                      |
| Lossless tap        | `rfic_source` to `rx_sink` and, in parallel, to the block               |
| TX injection        | The block to `rfic_sink`, instead of or mixed with `tx_source`          |
| RX to TX repeater   | `rfic_source` to the block, the block to `rfic_sink`; `rx_sink` can carry a copy for observation |
| Side results        | The block to an extra DMA channel, `self.pcie_dma1` and the following   |

The hook is called after the host links are built, so the extra DMA channels requested with
`--pcie-dmas` or `pcie_dmas` exist when it runs. They let a block send correlation results or
measurements to the host without touching the sample stream of channel 0.

Every DMA channel has a synchronizer, which the driver arms. The script connects the PPS pulse
to the synchronizer of every channel, so an extra channel starts like channel 0 and the subclass
has nothing to connect.

The RX side of the RFIC core cannot be paused, so a block that stops accepting samples loses
them.

The generic processing blocks, oscillators, mixers and filters, are gathered in LiteDSP
(<https://github.com/enjoy-digital/litedsp>), which this hook is meant to host.
[Building the gateware](#building-gateware) shows how to build a subclass.

## Packets for VRT {#streaming-vrt}

One more block of the gateware works on the RX stream. The VRT streamer of the Ethernet builds
needs packets of a fixed length, while the RX stream is continuous. `RFICDataPacketizer` cuts the
stream into packets, 256 words of 32 bits in the SoC, and stores each packet in a FIFO before it
is sent.

The VRT path itself, with its timestamps taken from the board time, is described in [Host
links](#host-links).

## In the code {#streaming-code}

| File                          | Content                                                          |
|---------------------------------------------------------------------------|---------------------------------------------------------------------------|
| `litex_m2sdr.py`              | Sample path wiring, crossbar, DMA options, `add_rfic_datapath()` |
| `gateware/header.py`          | `HeaderInserterExtractor`, `TXHeaderExtractor`, `RXHeaderInserter`, `TXRXHeader` |
| `gateware/time.py`            | `TimeGenerator`, `TimeNsToPS`                                    |
| `gateware/loopback.py`        | `TXRXLoopback`                                                   |
| `gateware/cdc.py`             | `ValueStrobeCDC`, `AsyncFIFORegistered`, `ClockDomainCrossingRegistered` |
| `gateware/rfic.py`            | `RFICDataFramer`, `RFICDataPacketizer`                           |
| `software/user/libm2sdr/m2sdr.h` | Header layout, timed TX and low latency functions             |
| `software/kernel/main.c`      | DMA ring, module parameters, synchronizer arming                 |
| `test/test_header.py`         | Header frames, gate decisions, offsets                           |
| `test/test_time_core.py`, `test_loopback.py`, `test_rfic.py` | Unit tests                        |

The paths of the gateware and software files are relative to `litex_m2sdr/`.

## Where to go next {#streaming-next}

[Host links](#host-links) describes the sources and destinations of the crossbar, and
[Timing](#timing) how the board time is disciplined.
