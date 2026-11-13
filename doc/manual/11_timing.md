# Time synchronisation {#timing}

This chapter describes the gateware that keeps the board in step with the outside: the reference
clock of the RFIC, the PPS, and the three ways to synchronise the board over a link, PCIe PTM,
Ethernet PTP and White Rabbit.

The concepts are explained in [Timing primer](#timing-primer);
here they are linked to the modules, the registers and the tools. The use-oriented recipes are
in [Workflows](#workflows).

## At a glance {#timing-glance}

| Mechanism | Build option | What it sets | Needs |
|-----------------|----------------------------------------------|-------------------|------------------|
| 10 MHz reference | None, selected by software | Reference of the SI5351 | SI5351C version |
| PPS input | `--pps-input` | Nothing, monitored only | 3.3 V PPS signal |
| PCIe PTM | `--with-pcie-ptm` | Board time, from the host | PCIe x1, PTM host |
| Ethernet PTP | `--with-eth-ptp` | Board time, from a PTP master | Baseboard, Ethernet |
| PTP RFIC clock | `--with-eth-ptp-rfic-clock` | 10 MHz clock of the FPGA | Ethernet PTP |
| White Rabbit | `--with-white-rabbit` | White Rabbit time, 10 MHz | Baseboard, SFP |

There are two things to synchronise, and the mechanisms do not all act on both. The *board time*
is the 64-bit nanosecond counter that stamps the buffers. The *reference clock* sets the
frequency of the RFIC. The status of each mechanism is in the [status table](#intro-status).

## Board time and who sets it {#timing-board-time}

The board time is produced by the time generator, described in [Streaming](#streaming). It runs
on a 100 MHz clock from the SI5351 and adds a programmable increment to a nanosecond counter on
each cycle. The increment is a fixed-point value with 24 fractional bits, 10 ns by default.

![The board time. Look at the left: three sources can set or steer the same counter, and the
fourth input is its clock; on the right, the three users of the time.](timing_overview.svg)

Three sources act on this counter, and all pass through the same update logic (`TimeGenerator`).

| Source | Action | Path |
|--------------------|------------------------------------------|--------------------------------------|
| Host software | Write, offset, increment | Registers of `time_gen` |
| PCIe PTM | Write, increment, from `phc2sys` | Kernel driver, same registers |
| Ethernet PTP | Write, phase trim, increment | Discipline interface |

When several actions arrive in the same cycle the order is fixed: a write first, then the sync
input, then a phase trim, then the normal increment. When the PTP discipline is enabled, its
increment replaces the one of the register.

One counter for every source has a consequence: the timestamps of the headers, the internal PPS,
the VRT timestamps and the clock that the host sees all follow the same time, whatever
synchronises it. White Rabbit is the exception: it keeps its own time, as described in
[White Rabbit](#timing-wr).

## Reference clock: SI5351 {#timing-si5351}

The clock generator of the board provides the reference of the RFIC and the 100 MHz clock of the
board time. Its place on the board is described in [Hardware](#hardware). The gateware selects
its reference input and configures it over I2C (`SI5351`).

![Reference clock selection. Look at the clock mux: on the SI5351C version, it sends either the
external 10 MHz or the 10 MHz clock of the FPGA to the reference input of the
SI5351.](timing_clocks.svg)

### Two versions of the board {#timing-si5351-versions}

One pin of the clock generator has a different role on the two versions of the board. The
register `si5351_control` tells the gateware which version is fitted.

| Field | Value | Meaning |
|---------------------|-------------|------------------------------------------------------------------|
| `version` | 0 | SI5351B version: the pin is the spread spectrum enable |
| `version` | 1 | SI5351C version: the pin is the 10 MHz reference input |
| `clkin_src` | 0 | Reference from the 10 MHz clock of the FPGA |
| `clkin_src` | 1 | Reference from the uFL connector |
| `ss_en` | 0, 1 | SI5351B version: spread spectrum disabled, enabled |
| `seq_reset` | 1 | Restart the configuration sequencer |

On the SI5351B version the clock generator runs from its crystal, and the gateware tunes it
with a PWM output: 50 % duty by default, set by the registers `si5351_pwm_*`. On the SI5351C
version the PWM pin is held high and the pin shared with the spread spectrum enable outputs the
selected 10 MHz clock.

### Reference selection {#timing-si5351-reference}

The software selects the reference with one of three clock sources. It writes the register above
and then loads the matching configuration of the clock generator.

| Clock source | `version` | `clkin_src` | Reference of the SI5351 |
|-------------------|-----------------|--------------------|----------------------------------------------|
| `internal` | 0 | 0 | The crystal of the clock generator |
| `external` | 1 | 1 | 10 MHz on the uFL connector |
| `fpga` | 1 | 0 | 10 MHz clock of the FPGA (`clk10`) |

The clock source is the `--sync` option of `m2sdr_rf` and the `clock_source` argument of the
SoapySDR driver, described in [Software](#software). The software has configurations for an
RFIC reference of 38.4 MHz and of 40 MHz.

The 10 MHz clock of the FPGA comes from an MMCM fed by the 100 MHz oscillator of the board. By
itself it is no better than this oscillator.

Its interest is that the gateware can steer its
phase: from PTP, as described in [PTP-referenced RFIC clock](#timing-ptp-rfic), or from White
Rabbit, which replaces it with a 10 MHz clock made from the White Rabbit clock.

### Configuration at power-up {#timing-si5351-sequencer}

The clock generator must run before the host is there, since the board time uses its 100 MHz
output. A small sequencer writes a default configuration through the I2C master of the gateware
as soon as the FPGA is configured (`LiteI2CSequencer`).

The default configuration uses the crystal and provides 38.4 MHz on outputs 0 and 4 and 100 MHz
on output 1; the other outputs are off. The field `seq_done` of the register `si5351_status`
reads 1 when the sequence is finished. The software later rewrites the configuration through
the same I2C master, whose registers are `si5351_i2c_*`.

In the code: `litex_m2sdr/gateware/si5351.py`. `test/test_si5351.py` simulates the sequencer and
the version logic.

## PPS {#timing-pps}

### Internal PPS {#timing-pps-gen}

The gateware makes its own PPS from the board time (`PPSGenerator`). A pulse is produced each
time the board time passes the next second mark, and the PPS signal stays high for 100 ms, a
duty cycle of 10 %.

The signal is also high during the first 100 ms after a reset, before any pulse. Only the
LiteScope probe uses this level.

| Item | Value |
|--------------------------------------|--------------------------------------------------------------|
| First pulse | When the board time passes 1.5 s |
| Period | 1 s of board time |
| Offset to the second | 500 ms after each second of the board time |
| Pulse counter | 32 bits, cleared when the time is written |
| Start | When the board time is not zero |

The internal PPS has four users: the DMA synchronizer of PCIe, the seconds field of the VRT
timestamps, the reference of the PTP RFIC clock loop, and the status LED.

> The gateware has no PPS output: the internal PPS is not connected to a pin of the board.

The generator restarts when the board time is written or offset. The marks are counted from
zero: after a write of a large time, the generator produces one pulse per system clock cycle
until the marks reach the new time.

> A write of a Unix time gives one pulse per system clock cycle for about 14 s at 125 MHz and
> 17 s at 100 MHz. This happens at the first step of the PTP discipline, at each step made by
> `phc2sys` and at each offset written by the kernel driver.

During the burst the seconds of the VRT timestamps are wrong, and a DMA stream that starts is
not aligned on a second.

### PPS input {#timing-pps-in}

An external PPS can be connected to the board. The gateware synchronises it to the system clock,
detects its rising edges and exposes the result in two registers (`PPSInput`).

| Register | Field | Meaning |
|----------------------------------|---------------------|----------------------------------------------|
| `pps_in_status` | `level` | Level of the input |
| `pps_in_status` | `pulse` | One cycle at 1 on a rising edge |
| `pps_in_count` | | Number of rising edges, 32 bits |

| `--pps-input` | Pin | Note |
|--------------------------|----------------------|----------------------------------------------------|
| `m2` | M.2 pin 22 | Default; the carrier must route this pin |
| `tp1` | Test point TP1 | Not with `--with-gpio` |
| `tp2` | Test point TP2 | Not with `--with-gpio` |

All three inputs take a ground-referenced 3.3 V logic signal. Do not apply 5 V. TP1 and TP2 are
the pins of the GPIO feature, which is why the two options exclude each other.

> The PPS input is a monitor only: the board time is not aligned on it.

The time generator has a sync input that loads the next second on a pulse, enabled by the field
`sync_enable`. The SoC connects neither the pulse nor the second number: the field has no
effect.

No software of the repository reads the two registers. Aligning the board time on an external
PPS needs a derived design.

In a White Rabbit build the two registers of the PPS input are not present. In the other builds
their bank has no fixed location: it is at `0x15000` in a PCIe build and at `0x15800` in a
build with SATA, for example. Read the addresses in the register file of the build
([Locations that move](#soc-csr-moving)).

In the code: `litex_m2sdr/gateware/pps.py`; `test/test_pps.py`.

## Phase steering of the 10 MHz clock {#timing-clk10}

The MMCM that makes the 10 MHz clock has a dynamic phase shift port: each request moves the
phase of the output by one fine step. A small module drives this port from registers
(`MMCMPhaseDiscipline`). It is present in every build without White Rabbit.

| Register | Role |
|---------------------------------------------------|-------------------------------------------------|
| `clk10_discipline_control` | `enable`, `manual_inc`, `manual_dec`, `clear_counters` |
| `clk10_discipline_rate` | Signed rate, in steps per update, 32 fractional bits |
| `clk10_discipline_update_cycles` | System clock cycles between updates |
| `clk10_discipline_status` | `busy`, `pending`, `last_direction` |
| `clk10_discipline_step_count` | Completed steps; also `inc_count` and `dec_count` |
| `clk10_discipline_dropped_count` | Requests dropped while a step was in progress |

A step can be requested by hand, one at a time. In automatic mode the rate is added to an
accumulator at each update, and each overflow requests a step in the direction of the sign. The
default update interval is 1 µs. A steady rate therefore shifts the phase continuously, which
is a small frequency offset.

A step is 1/56 of the period of the VCO of the MMCM. The build selects a VCO of 1.28 GHz for
this MMCM, which gives a step of about 14 ps.

The port of the MMCM runs on the 200 MHz clock. The accumulator and the registers stay in the
system clock domain; only the request pulse, the direction and the completion pulse cross. The
PTP RFIC clock loop takes over this module through an override input when it is enabled.

In the code: `litex_m2sdr/gateware/clock_discipline.py`; `test/test_clock_discipline.py` and
`test/test_soc_clk10_discipline.py`.

## PCIe PTM {#timing-ptm}

PCIe PTM lets the board time follow the clock of the host over the PCIe link, as explained in
[Timing primer](#timing-primer). If the host clock is itself synchronised by NTP or PTP, the
board follows that time. PTM uses LitePCIe only; it does not need White Rabbit.

### Gateware {#timing-ptm-gateware}

`--with-pcie-ptm` adds three blocks from LitePCIe, described in the
[LitePCIe manual](https://github.com/enjoy-digital/litepcie/releases/tag/manual-nightly).

| Block | Role |
|--------------------------------|--------------------------------------------------------------------|
| PTM sniffer | Observes the PTM messages in the receive path of the PHY |
| `PTMCapabilities` | Declares the PTM capability to the host |
| `PTMRequester` | Sends PTM requests and stores their timestamps |

The requester takes its timestamps from the board time. Its registers hold the results of the
last request.

| Register | Content |
|---------------------------------------------------|-------------------------------------------------|
| `ptm_requester_control` | `enable`, `trigger` |
| `ptm_requester_status` | `valid`, `busy` |
| `ptm_requester_t1_time` | Board time when the request left |
| `ptm_requester_t4_time` | Board time when the response arrived |
| `ptm_requester_master_time` | Time of the host in the response |
| `ptm_requester_link_delay` | Delay reported by the host port |
| `ptm_requester_phy_tx_delay` | Delay of the transmit path of the PHY |
| `ptm_requester_phy_rx_delay` | Delay of the receive path of the PHY |

PTM is used with PCIe Gen2 x1 and requires `--with-pcie`. The build raises an error with
`--pcie-lanes` above 1; it does not check the speed. The LitePCIe checkout must provide the PTM
sniffer of the PHY (`create_ptm_sniffer`).

### Kernel driver {#timing-ptm-driver}

The kernel driver exposes the board time as a PTP hardware clock, the standard Linux interface
for a clock that can be read, set and steered. Its clock name is `m2sdr`.

| Operation of the clock | What the driver does |
|-------------------------------|---------------------------------------------------------------------|
| Read | Latches and reads the board time |
| Set | Writes the board time |
| Offset | Reads the time, adds the offset and writes it back |
| Frequency | Scales the increment of the time generator |
| Cross timestamp | Triggers a PTM request and pairs board time and host time |

The cross timestamp is where PTM is used. The driver triggers a request and waits for its end.
It returns the board time of the request with the host time of the response.

The host time is corrected by the one-way delay: half of the round trip of the previous
request, once the delay reported by the host port is removed.

A module parameter, `ptm_offset_ns`, is subtracted from the time on a read and added on a write.
Its default is -500 ns and it can be changed at run time; it is the place for a calibration of
the constant offset between the host and the board.

When the bitstream has the Ethernet PTP discipline and it is active, the driver refuses to set,
offset or steer the time and returns a busy error. The library has the same rule for its time
writes. Two masters cannot steer the same clock.

### Host side {#timing-ptm-host}

Synchronising the board is then the job of `phc2sys`, the linuxptp tool that steers one clock
from another. A helper finds the clock of the board and starts `phc2sys` with the host as the
source and the board as the sink.

```sh
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=1 --with-pcie-ptm --build --load
scripts/m2sdr_pcie_time_sync.py --dry-run
sudo scripts/m2sdr_pcie_time_sync.py --stdout
```

The helper looks for a clock whose name is `m2sdr` under `/sys/class/ptp` and waits up to 60 s
for it, so it can start before the driver. With several boards it stops and asks for `--phc`.
`--dry-run` prints the command instead of running it. The capture below shows it for an explicit
clock.

```console
$ scripts/m2sdr_pcie_time_sync.py --dry-run --phc /dev/ptp0 --phc2sys /bin/true
/bin/true -s CLOCK_REALTIME -c /dev/ptp0 -O 0.0 -R 8.0 -N 5
```

| Option | Default | Passed to `phc2sys` as |
|----------------------------------------|----------------------------|---------------------------------|
| `--source` | `CLOCK_REALTIME` | `-s`, the source clock |
| `--offset` | 0 s | `-O`, offset between sink and source |
| `--rate` | 8 Hz | `-R`, update rate |
| `--readings` | 5 | `-N`, source reads per update |
| `--first-step-threshold` | Not set | `-F`, stepping threshold at start |
| `--step-threshold` | Not set | `-S`, stepping threshold while running |
| `--wait-ptp4l` | Off | `-w`, wait for `ptp4l` and use its UTC offset |
| `--stdout` | Off | `-m`, log to the standard output |

To start the synchronisation at boot, install a systemd service that runs the helper, with the
path of the repository on the host:

```ini
[Unit]
Description=Synchronize LiteX-M2SDR board time to host time
After=multi-user.target

[Service]
ExecStart=/path/to/litex_m2sdr/scripts/m2sdr_pcie_time_sync.py --stdout
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

In the code: the PTM section of `BaseSoC` in `litex_m2sdr.py`, the PTP clock functions of
`litex_m2sdr/software/kernel/main.c` and `scripts/m2sdr_pcie_time_sync.py`. `test/test_soc_ptm.py`
elaborates the PTM SoC and `test/test_pcie_time_sync_script.py` covers the helper.

## Ethernet PTP {#timing-ptp}

With `--with-eth-ptp` the board is a PTP slave on its Ethernet link. LiteEth receives the PTP
messages and keeps a PTP time; the gateware then steers the board time to it. The board time is
not replaced: PTP becomes its source. PTP is introduced in [Timing primer](#timing-primer).

![Ethernet PTP. Follow the cyan arrows: the servo compares the PTP time with the board time and
corrects the time generator. The lower row is the optional loop on the 10 MHz
clock.](timing_ptp.svg)

### PTP core {#timing-ptp-core}

The PTP slave is the one of LiteEth (`LiteEthPTP`), described in the
[LiteEth manual](https://github.com/enjoy-digital/liteeth/releases/tag/manual-nightly). The
gateware connects it to two ports of the UDP crossbar and gives it an identity.

| Item | Value |
|-------------------------------|---------------------------------------------------------------------|
| Transport | UDP over IPv4, event port 319 and general port 320 |
| Delay mechanism | End to end; peer to peer with `--eth-ptp-p2p` |
| Clock identity | The device DNA of the FPGA |
| Port number | Index of the SFP cage plus 1 |
| Registers | `eth_ptp_locked`, `eth_ptp_master_ip` |

PTP uses multicast groups, and a switch may only forward them to members. The core therefore
sends IGMP membership reports for `224.0.1.129` and `224.0.1.130`, and for `224.0.0.107` in peer
to peer mode. The interval is 2 s (`--eth-ptp-igmp-interval`) and `--eth-ptp-no-igmp` removes
the reports.

### Board time discipline {#timing-ptp-discipline}

The servo samples the PTP time and the board time 64 times per second by default and computes
their difference in nanoseconds (`PTPTimeDiscipline`). It then takes one of three actions,
through the discipline interface of the time generator.

| Action | When | Effect on the board time |
|---------------|--------------------------------------------|-----------------------------------------|
| Coarse step | First sample, or large error while not locked | The PTP time is written |
| Phase trim | Error at or above the phase threshold | A bounded number of ns is added or removed |
| Rate trim | Every sample without a coarse step | The increment is corrected |

The phase trim is the error divided by 4, between 1 ns and the maximum step. The rate trim is a
proportional and integral correction of the increment around its nominal value, bounded by the
trim limit.

A sample is taken only when the seconds of the PTP time are stable, so that a
rollover of the second is never read half way.

The commands cross from the system clock to the clock of the time generator with their values:
a write or a trim is applied in one piece (`TimeDisciplineCDC`).

### States {#timing-ptp-states}

The figure below shows the four states of the servo and the main transitions between them.

![States of the board time discipline. The cyan arrow is the normal path; "misses" is the count
of consecutive samples out of the lock window.](timing_ptp_states.svg)

| State | Value | Meaning |
|------------------|--------------|--------------------------------------------------------------------|
| Manual | 0 | Discipline disabled, the registers of `time_gen` rule |
| Acquire | 1 | Enabled, board time not yet within the lock window |
| Locked | 2 | PTP locked and board time within the lock window |
| Holdover | 3 | PTP lock lost, the last rate is kept |

The discipline is enabled at reset. The lock state is filtered. Once locked, a sample out of the
lock window is corrected but does not drop the state: the lock is lost only after a number of
consecutive misses, or at once when the PTP core loses its master.

A large error that appears while locked is treated with care. By default it is rejected: the
board time is not rewritten, the last accepted error is kept and only the counter
`time_lock_misses` advances.

> With the default settings the state stays Locked as long as the large error lasts, even if it
> is permanent. A real step of the master is then seen only in this counter: the last error
> reads as before and a soak of `m2sdr_ptp_check.py` passes.

While locked, an error within 1 ms of one second (0.999 s to 1.001 s) is never copied to the
board time: it is taken as a one-second excursion of the PTP time, not as a real step.

When the PTP core loses its lock, the servo goes to Holdover if it has been locked before and
holdover is enabled: the last rate is kept and no correction is made. Otherwise it returns to
Acquire with the nominal rate. This also applies in Acquire, once the servo has been locked.

When the PTP lock comes back, the servo leaves Holdover for Locked if the error is within the
lock window, and for Acquire otherwise.

### Parameters {#timing-ptp-parameters}

All the parameters are registers of `ptp_discipline`, with the reset values below. The utility
`m2sdr_util` reads and writes them with its `ptp-config` command. The fields of the command are
the register names with hyphens, such as `lock-window`, `unlock-misses` or `coarse-confirm`.

```sh
make -C litex_m2sdr/software/user m2sdr_util
m2sdr_util --ip 192.168.1.50 ptp-config
m2sdr_util --ip 192.168.1.50 ptp-config lock-window 4096
```

| Register | Reset | Meaning |
|-------------------------------|--------------|--------------------------------------------------------|
| `control` | 3 | Bit 0 `enable`, bit 1 `holdover` |
| `update_cycles` | Clock / 64 | System clock cycles between samples |
| `coarse_threshold` | 100000 ns | Error treated as large |
| `phase_threshold` | 128 ns | Error from which a phase trim is made |
| `lock_window` | 4096 ns | Error below which the time is locked |
| `unlock_misses` | 64 | Consecutive misses before the lock is lost |
| `coarse_confirm` | 0 | Consecutive large errors before a step while locked |
| `phase_step_shift` | 2 | Right shift of the error for a phase trim |
| `phase_step_max` | 4096 ns | Largest phase trim |
| `trim_limit` | 200000 | Largest rate trim, in steps of 2^-24 ns per cycle |
| `p_gain` | 8 | Proportional gain of the rate trim |

The lock window is wider than the phase threshold on purpose. Corrections start at 128 ns, but
jitter of a few microseconds, common with a host that timestamps in software, is not reported as
a loss of lock. With 64 samples per second, 64 misses are about one second.

The default trim limit is about 1190 ppm of the increment.

`coarse_confirm` at 0 disables the steps while locked. A value above 0 allows a step after that
many consecutive large errors: use it only when the board must follow a master that really
steps its time.

The status registers of `ptp_discipline` report the state, the last error, the last pair of
sampled times, the increment in use and seven counters.

| Counter | Counts |
|------------------------------------------|----------------------------------------------------------|
| `coarse_steps` | Writes of the PTP time to the board time |
| `phase_steps` | Phase trims |
| `rate_updates` | Rate trims |
| `ptp_lock_losses` | Losses of lock of the PTP core |
| `time_lock_misses` | Samples out of window tolerated while locked |
| `time_lock_miss_count` | Consecutive misses at this moment |
| `time_lock_losses` | Losses of the filtered lock |

A write of the register `clear_counters` clears them. They let a long test tell corrected
jitter, steps and real losses of lock apart.

### Master identity {#timing-ptp-identity}

A small module records which master the board follows (`PTPIdentityTracker`). It takes the
source port identity of the Sync, Follow_Up, Announce and delay response messages, and of their
peer delay equivalents, that come from the IP address of the selected master, or from any sender
while no master is selected.

| Register | Content |
|-----------------------------------------------------|-----------------------------------------------|
| `ptp_identity_local_clock_id` | Clock identity of the board, 64 bits |
| `ptp_identity_local_port_number` | Port number of the board |
| `ptp_identity_master_clock_id` | Clock identity of the master |
| `ptp_identity_master_port_number` | Port number of the master |
| `ptp_identity_update_count` | Number of updates of the master identity |

### Host side {#timing-ptp-host}

The host, or another device of the network, is the PTP master. With linuxptp, `ptp4l` is started
on the interface that reaches the board. Two configuration files are provided, for a direct
cable between the host and the board.

| File in `doc/ptp/` | Timestamps of the host | Equivalent options |
|---------------------------------------------|------------------------------|--------------------------|
| `ptp4l-m2sdr-software.cfg` | Software | `-4 -E -S` |
| `ptp4l-m2sdr-hardware.cfg` | Hardware | `-4 -E -H` |

Both select UDP over IPv4 and the end to end delay mechanism, with one Sync and one delay
request per second, and an Announce and a summary every two seconds.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-ptp --eth-sfp=0 --build --load
ethtool -T <interface>
sudo ptp4l -i <interface> -f doc/ptp/ptp4l-m2sdr-software.cfg -m
```

`ethtool -T` tells which file to use. Use the hardware file only when the output reports a PTP
hardware clock and hardware transmit and receive timestamps. Keep the software file when it
only lists `software-transmit`, `software-receive` and `software-system-clock`.

With software timestamps the board locks as well, but the timestamp path of the host dominates
the jitter that is observed.

### Checking the lock {#timing-ptp-check}

The board reports its PTP state through the utility `m2sdr_util`, described in
[Software](#software).

| Command | Shows |
|-------------------------|---------------------------------------------------------------------------|
| `info` | PTP core locked, board time locked, holdover |
| `ptp-status` | State, last error, identities, counters; `--watch`, `--json` |
| `ptp-smoke` | A pass or fail check for automation |
| `ptp-config` | The parameters of the servo, read or written |

A script wraps these commands for longer checks (`scripts/m2sdr_ptp_check.py`).

| Subcommand | What it does |
|-------------------|---------------------------------------------------------------------------------|
| `smoke` | Ping, `ptp-smoke`, status, and the PTP message types seen by `tcpdump` |
| `soak` | Samples the status for a duration and checks lock and error limits |
| `calibrate` | Collects the residual error and proposes a compensation |

```sh
scripts/m2sdr_ptp_check.py smoke --ip 192.168.1.50 --iface <interface> --duration 10
scripts/m2sdr_ptp_check.py soak --ip 192.168.1.50 --duration 1800 --interval 5 \
    --max-error-ns 1000000 --json-out build/ptp/soak.json --csv-out build/ptp/soak.csv
```

The report of a soak holds the result, the statistics of the signed and absolute error with
their percentiles, the changes of the counters, the last status and the samples. The CSV file
is easier to plot over a long soak.

The limit of 1 ms is the default: keep it with software timestamps until enough data exists for
the host, and lower it with hardware timestamps. A soak does not detect the rejected large error
described in [States](#timing-ptp-states): compare the counter `time_lock_misses` too.

Without an external reference, the board can only report the residual error of its servo
against the master. `calibrate` collects it. For an absolute offset an external reference is
needed, and the gateware has no PPS output: a derived design must first bring the internal PPS
to a pin. The measured offset is then given with `--reference-offset-ns`.

```sh
scripts/m2sdr_ptp_check.py calibrate --ip 192.168.1.50 --duration 300 --interval 1 \
    --json-out build/ptp/calibration.json --csv-out build/ptp/calibration.csv
```

The median and the suggested compensation use only the samples that are locked and within the
limit. The compensation is the field `calibration.suggested_compensation_ns` of the JSON
report. They are measurements of one setup. Repeat them with the host interface, the SFP module,
the cable and the timestamp mode of the target before using them.

In the code: the PTP part of the Ethernet section of `BaseSoC`; `ptp_discipline.py` and
`ptp_identity.py` in `litex_m2sdr/gateware/`. `test/test_ptp_discipline.py`,
`test/test_ptp_identity.py`, `test/test_soc_eth_ptp.py` and `test/test_ptp_check_script.py` cover
them.

## PTP-referenced RFIC clock {#timing-ptp-rfic}

The PTP discipline sets the board time, not the frequency of the RFIC. With
`--with-eth-ptp-rfic-clock` a second, slow loop steers the 10 MHz clock of the FPGA so that it
follows the PTP time too (`PTPClock10Discipline`). On the SI5351C version this clock can be the
reference of the clock generator, and so of the RFIC.

This does not make a low-jitter RF reference out of packet timestamps. The loop compares two
1 Hz events and corrects slowly. It gives two boards the same average frequency; it does not
align the phase of their RF or of their samples.

### Loop {#timing-ptp-rfic-loop}

The loop compares the internal PPS, which follows PTP, with a 1 Hz marker made by dividing the
10 MHz clock by ten million. The difference is measured in system clock cycles, so the
resolution is one cycle: 10 ns at 100 MHz. The register `phase_tick_ps` gives it.

The first reference pulse after the loop is enabled resets the marker, so the two events start
aligned. Each following pair gives an error. A proportional and integral controller turns it
into a rate of phase steps, bounded by a limit, and the rate is given to the phase steering
module of [Phase steering of the 10 MHz clock](#timing-clk10).

The integral term learns the constant rate that the board needs, so no permanent phase error is
required to hold the frequency. When the PTP time is not locked, the loop keeps its last rate
if holdover is enabled and it has been locked before; otherwise the rate returns to zero.

### Parameters {#timing-ptp-rfic-parameters}

The registers belong to `ptp_clk10_discipline`. The loop is disabled at reset: the build only
measures until the software enables it. The utility commands are `ptp-clock10-status` and
`ptp-clock10-config`, whose fields are the register names with hyphens.

| Register | Reset | Meaning |
|--------------------------------|---------------------|-----------------------------------------------|
| `control` | 2 | Bits `enable`, `holdover`, `invert`, `align`, `clear_counters` |
| `update_cycles` | Clock / 5 MHz | Update interval of the phase steering |
| `p_gain` | `0x00040000` | Proportional gain |
| `i_gain` | `0x00010000` | Integral gain |
| `rate_limit` | `0x7fffffff` | Largest rate of phase steps |
| `lock_window_ticks` | 50 µs of cycles | Error below which the clock is locked |
| `half_period_ticks` | 0.5 s of cycles | Search window of the phase detector |

At the 100 MHz system clock of an Ethernet build, the update interval is 20 cycles (200 ns) and
the lock window is 5000 cycles. The window is wide to accept the jitter of a PTP master with
software timestamps; lower it with hardware timestamps, after measuring the link.

| Status field | Meaning |
|----------------------------------|------------------------------------------------------------------|
| `active` | The loop drives the phase steering |
| `reference_locked` | The board time is locked to PTP |
| `clock_locked` | The marker is within the lock window |
| `holdover` | Reference lost, last rate kept |
| `aligned` | The marker has been aligned since the loop was enabled |
| `rate_limited` | The last rate reached the limit |

The registers `last_error_ticks` and `last_error_ns` give the phase of the marker relative to the
reference, negative when the marker comes first. `last_rate` is the rate in use. Counters report
the samples, the pulses of each side, the missing pairs, the alignments, the losses of lock, the
rate updates and the saturations.

### Use {#timing-ptp-rfic-use}

1. Build with `--with-eth --with-eth-ptp --with-eth-ptp-rfic-clock` and wait for the PTP lock.
2. Read the status first; the loop only measures.
3. Enable the loop and watch it until `reference_locked` and `clock_locked` are both set.
4. Select the `fpga` clock source before the RFIC is initialised and calibrated.

```sh
m2sdr_util --ip 192.168.1.50 ptp-clock10-status
m2sdr_util --ip 192.168.1.50 ptp-clock10-config enable on
m2sdr_util --ip 192.168.1.50 --watch ptp-clock10-status
m2sdr_rf --sync fpga --refclk-freq 38400000
```

If the marker was started before the PTP lock, or after a change of reference, request a new
alignment with `ptp-clock10-config align`. The request is applied on the next reference pulse,
not when the register is written, so the delay of the host command does not set the phase.

If the error diverges after the loop is enabled, the field `invert` flips the sign of the
correction. During a long test, watch the missing pairs, the losses of lock and the
saturations: a clean run adds none. `m2sdr_ptp_check.py` includes this status with
`--with-clock10` and requires the lock with `--require-clock10-lock`.

> Keep the loop slow for RF work. A master with software timestamps has enough jitter to make a
> tight loop harmful, and large corrections while the RFIC is calibrated and streaming disturb
> the signal.

In the code: `PTPClock10Discipline` in `litex_m2sdr/gateware/clock_discipline.py`.

## White Rabbit {#timing-wr}

> White Rabbit on LiteX-M2SDR is Experimental (see the [status table](#intro-status)): the lock,
> the jitter and the accuracy of the PPS have not been measured on this board.

White Rabbit synchronises clocks over fibre to well below a nanosecond, as explained in
[Timing primer](#timing-primer). With `--with-white-rabbit` the gateware includes a White Rabbit
node on one SFP cage of the baseboard. The node comes from LiteX-WR-NIC, the LiteX integration
of the White Rabbit core.

### What the build adds {#timing-wr-gateware}

| Item | Value |
|--------------------------------|--------------------------------------------------------------------|
| Integration | `add_white_rabbit` of LiteX-WR-NIC |
| SFP cage | `--wr-sfp`; the first available cage when omitted |
| Transceiver clock | The Ethernet channel of the shared QPLL, 1.25 Gbps |
| Reference MMCM | 125 MHz, tunable by fine phase steps |
| DMTD MMCM | 62.5 MHz, tunable by fine phase steps |
| Phase word width | 16 bits (`--wr-dac-bits`) |
| 10 MHz clock | Made from the White Rabbit clock, 62.5 MHz |
| Host window | `0x00040000`, size `0x00040000`, on the SoC bus |
| Console | Shared UART with a receive FIFO of 4096 bytes |
| SFP I2C | SMBus pins of the M.2 connector |

A White Rabbit node normally tunes two oscillators with DACs. Here the two tunable clocks are
MMCMs, and the tuning words of the core are turned into fine phase steps. Each MMCM has its own
backend, which takes the commands in the clock domain of the core and waits for the end of each
step on the 200 MHz clock.

Fine phase shifting requires integer dividers, so both MMCMs are built without fractional
division. Their VCO runs at 1.5 GHz, which gives a step of about 11.9 ps. The backends
report busy, completed steps, superseded commands and a completion timeout; a timeout needs a
reset of the backend and of the MMCM, for example by reloading the FPGA.

A fractional setting of the DMTD MMCM, a VCO of 1.59375 GHz divided by 25.5, cannot make fine
phase steps. The step of the integer setting differs from it by 6.25 %; the coefficients of the
firmware are the same.

In a White Rabbit build the 10 MHz clock of the FPGA is made from the White Rabbit clock. On the
SI5351C version, selecting the `fpga` clock source therefore gives the RFIC a reference that
follows White Rabbit.

### Processor and firmware {#timing-wr-cpu}

The White Rabbit core runs its protocol in firmware on a small processor inside the FPGA. Four
options select the processor, its memory and the way the firmware is loaded.

| Option | Values | Default |
|------------------------------------|---------------------------------------|-------------------------|
| `--wr-cpu-type` | `urv`, `vexriscv` | `urv` |
| `--wr-cpu-variant` | `lite`, for VexRiscv only | Not set |
| `--wr-cpu-memory` | `private`, `integrated` | `private` |
| `--wr-cpu-boot` | `embedded`, `host` | `embedded` |

The default is the uRV processor with 128 KiB of private RAM and the firmware embedded in the
bitstream. The private memory requires uRV and embedded boot.

With the integrated memory, a RAM of 128 KiB is on the SoC bus, and with host boot the
processor is held until the host has loaded the firmware. uRV can also use the integrated
memory, with either boot. There is no external RAM and no boot from flash.

The build helper selects the firmware of the processor and builds it with the `acorn` firmware
target when it is missing. `--wr-firmware` gives another image; for an embedded boot in
integrated memory the matching `.bin` file must be next to the `.bram` file.

### Dependency {#timing-wr-dependency}

White Rabbit needs a checkout of
[LiteX-WR-NIC](https://github.com/enjoy-digital/litex_wr_nic) with the host management, the
processor memory and boot, and the application time interfaces. The commit that the continuous
integration tests is in `.github/workflows/ci.yml`; use that commit or a later one. An older
checkout may not have these interfaces.

The checkout is looked up in this order: `--wr-nic-dir`, the `LITEX_WR_NIC_DIR` environment
variable, then `litex_wr_nic` in the repository and next to it. `--wr-status` prints what was
found, including the state of the White Rabbit core sources, without building.

```sh
export LITEX_WR_NIC_DIR=/path/to/litex_wr_nic
export PYTHONPATH="$LITEX_WR_NIC_DIR${PYTHONPATH:+:$PYTHONPATH}"
./litex_m2sdr.py --variant=baseboard --wr-status
```

### Build {#timing-wr-build}

```sh
# uRV, private RAM, embedded firmware.
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit --build
# VexRiscv, integrated RAM, embedded firmware.
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit \
    --wr-cpu-type=vexriscv --wr-cpu-memory=integrated --build
# VexRiscv, integrated RAM, firmware loaded by the host.
./litex_m2sdr.py --variant=baseboard --with-pcie --with-white-rabbit \
    --wr-cpu-type=vexriscv --wr-cpu-memory=integrated --wr-cpu-boot=host --build
```

Without `--build` the command only elaborates the design, after building the firmware if needed.
With `--load` and the same options it loads the result. The name of the build directory includes
the processor, memory and boot choices that differ from the defaults.

> The file `scripts/csr.csv` is overwritten by each build. Keep it with its bitstream: the
> register addresses of the console and of the White Rabbit management depend on the
> configuration, and the host tools need the file of the loaded bitstream.

After a change of configuration, also rebuild the host software against the generated headers.

### Console and host loading {#timing-wr-console}

The White Rabbit console is reached through registers, so it works over any host bridge. Start a
LiteX server on the bridge, then use the host tool of LiteX-WR-NIC with the register file.

```sh
sudo litex_server --pcie --pcie-bar=04:00.0
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv status
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv console --command ver
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv console --command uptime
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv read-time
```

The PCIe address is the one of the board on the host. The shared UART also reports the
occupancy and the overflow of its receive FIFO. With host boot, load the firmware before
using the console; the tool verifies the whole RAM before it releases the processor.

```sh
python3 -m litex_wr_nic.wr --csr-csv scripts/csr.csv load-firmware \
    "$LITEX_WR_NIC_DIR/litex_wr_nic/firmware/spec_a7_wrc_vexriscv.boot"
```

### Limits {#timing-wr-limits}

- `read-time` returns the White Rabbit time. The board time is a separate counter: the gateware
  does not connect the White Rabbit time to the timestamps of the samples.
- A working processor, console and time snapshot do not prove that the servo is locked. The lock,
  the jitter and the accuracy of the PPS on this board have not been measured.
- White Rabbit and Ethernet PTP cannot be in the same bitstream.
- White Rabbit and Ethernet cannot be in the same bitstream either: the script refuses the
  pair, see [Shared QPLL](#links-qpll).
- The registers of the PPS input are absent, as described in [PPS input](#timing-pps-in).
- The simulations of LiteX-WR-NIC (`test/test_wr_clock.py` of that repository) cover the phase
  step backends. They do not measure the jitter of a real clock or the closed loop.

In the code: the White Rabbit section of `BaseSoC` and the White Rabbit MMCMs of `CRG` in
`litex_m2sdr.py`. `test/test_soc_white_rabbit.py` elaborates the processor, memory and boot
combinations and simulates the two phase step backends with the connections of the board.

## Combining the mechanisms {#timing-combos}

| Combination | Result |
|-------------------------------------------------|---------------------------------------------------|
| PCIe PTM without PCIe, or with x2 or x4 | Rejected |
| PCIe PTM with `--pcie-gen=1` | Accepted, not the supported use |
| Ethernet PTP without Ethernet | Rejected |
| PTP RFIC clock without Ethernet PTP | Rejected |
| Ethernet PTP and White Rabbit | Rejected |
| PTP RFIC clock and White Rabbit | Rejected |
| White Rabbit on the `m2` variant | Rejected |
| White Rabbit and Ethernet | Rejected |
| PCIe PTM and Ethernet PTP | Accepted; PTP owns the time while active |
| PPS input on TP1 or TP2 with GPIO | Rejected |

The transceiver side of these combinations is in [Shared QPLL](#links-qpll). The tests of this
chapter run with those of the links, as shown in
[Checking the links without hardware](#links-tests).

## Where to go next {#timing-next}

[Timing primer](#timing-primer) for the concepts; [Streaming](#streaming) for the time generator
and the timestamps; [Workflows](#workflows) for the recipes; [Debugging](#debugging) when a lock
does not come.
