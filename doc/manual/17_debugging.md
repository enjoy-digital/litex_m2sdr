# Debugging {#debugging}

This chapter explains how to find why a board does not answer, does not stream or streams wrong
samples. It gives the method, the access paths to a running board, the checks for each layer,
then the tests of the repository. Read it before working on hardware.

The generic method for a LiteX design is on the LiteX wiki page
[Debug a SoC on hardware](https://github.com/enjoy-digital/litex/wiki/Debug-A-SoC-On-Hardware).
This chapter follows it and adds the commands of the LiteX-M2SDR.

## At a glance {#debug-glance}

| Question | First tool |
|---------------------------------|-------------------------------------------------------------------|
| Does the board answer? | `m2sdr_util info` |
| Is the register access reliable? | `m2sdr_util scratch-test` |
| Is the PCIe link up, at which rate and width? | `lspci`, register `pcie_phy_link_status` |
| Is the Ethernet link up? | `ping`, register `eth_phy_pcs_status` or `eth_phy_status` |
| Are the clocks present? | `scripts/test_clks.py` |
| Is the stream path correct? | Loopback tests of `m2sdr_util` |
| What does an internal signal do? | LiteScope probe and `litescope_cli` |
| Is the bitstream sound? | Timing report, generated Verilog |
| Did a change break something? | `python3 -m pytest -v test` |

## Method {#debug-method}

The approach is to observe before capturing. The checks are ordered from the cheapest to the
most expensive, as the figure below shows, and each step is taken only when the previous one
gave a coherent answer.

![Debug steps, from the cheapest check to the most expensive one.](debug_steps.svg)

1. Check the identity of the board: the bridge answers and the build is the expected one.
2. Read the registers of the block in doubt, and add a counter or a status register when the
   question can be answered by software polling. Useful ones are the last address accepted by a
   bus master or slave, transaction counters, error and timeout counters, state snapshots of
   state machines, and sticky flags cleared by software.
3. Run the loopback tests to split the path: host, host link, FPGA, RFIC.
4. Capture a LiteScope trace only when the question is narrow: one clock domain, a precise
   trigger, less than 128 captured bits.
5. Read the generated Verilog and the timing report when the symptom depends on an address, a
   width, a reset or a clock domain.

Two more rules close the loop. Turn the finding into a test, so that it cannot come back
([Tests](#debug-tests)). Write the result down with what makes it reproducible
([Reporting a result](#debug-report)).

Start from the state of the tree, not from assumptions:

```sh
git status --short --branch
git log --oneline -5
```

The generated software headers are the usual trap. `csr.h`, `soc.h` and `mem.h` in
`litex_m2sdr/software/kernel/` follow the last run of the build script, and software compiled
against the headers of another build reads the wrong registers
([Generated software headers](#build-headers)).

### Entry points in the repository {#debug-entry}

| Path | Content |
|-----------------------------------------------------------------------|-----------------------------|
| `litex_m2sdr.py` | SoC, build options, register map, LiteScope probes |
| `litex_m2sdr/gateware/` | Gateware blocks of the board |
| `litex_m2sdr/software/user/libm2sdr/` | Host library, common to PCIe and Ethernet |
| `litex_m2sdr/software/user/m2sdr_util.c` | Low-level board utility |
| `scripts/` | Hardware scripts for a running board |
| `test/` | Tests that need no board |
| `.github/workflows/` | Reference commands of the continuous integration |

## Access to a running board {#debug-access}

There are two ways to reach the registers of a running board, shown in the figure below. The
utilities of the project go through the library, over PCIe, Ethernet or USB3. The LiteX host
tools go through `litex_server`, which opens one of the host bridges: the three of the figure,
or the USB bridge of a USB3 build ([Host bridges](#build-bridges)).

![Debug access: the utilities use the library; the LiteX tools use litex_server over PCIe, Etherbone or JTAGBone.](debug_access.svg)

### Project utilities {#debug-util}

The utilities are the right tool when the feature already has host support. `m2sdr_util` reads
the identity of the board, tests the register access and reads or writes one register
([Host software](#software)).

```sh
cd litex_m2sdr/software/user
./m2sdr_util info
./m2sdr_util scratch-test
./m2sdr_util reg-read 0x0
./m2sdr_util --device eth:192.168.1.50:1234 info
./m2sdr_util --device pcie:/dev/m2sdr0 info
```

The output of `info` must be coherent: the right SoC identifier, API version and variant, the
expected host links, a plausible FPGA temperature and voltages, the AD9361 present with its
product identifier.

Shifted strings, an API version `0.0`, an impossible temperature, a reserved variant or a
missing feature on a known-good bitstream point to corrupted register reads or a wedged board.
Fix that before looking at the RF state.

The software builds the PCIe and Ethernet host links by default, and USB3 when libusb is
installed. The link is selected at run time with the device identifier. For a fast rebuild of the utilities only:

```sh
make -C litex_m2sdr/software/user all
```

### LiteX host tools {#debug-litex-tools}

`litex_server` is the tool for small Python probes, for LiteScope and for registers that have no
utility yet. Start the server on the bridge that is available, then run the tools in another
terminal.

```sh
# Etherbone. The Ethernet build uses a 32-bit Etherbone path.
litex_server --udp --udp-ip=192.168.1.50 --udp-port=1234

# PCIe. Replace the address with the one of the board.
sudo litex_server --pcie --pcie-bar=04:00.0

# JTAG.
litex_server --jtag --jtag-config=openocd_xc7_ft4232.cfg
```

```sh
litex_cli --csr-csv=scripts/csr.csv --ident
litex_cli --csr-csv=scripts/csr.csv --regs --filter=crossbar
litex_cli --csr-csv=scripts/csr.csv --read=eth_phy_status
```

`scripts/csr.csv` is the register map of the last run of the build script. It must be the one of
the bitstream in the board, otherwise the names point to the wrong addresses. The locations of
several register banks move between builds with different options.

If the design exposes a crossover UART, as the White Rabbit builds do for the console of the
core, it is reached through the same server:

```sh
litex_term crossover --csr-csv scripts/csr.csv
```

When a transfer hangs, check whether the writes of the host accumulate without being consumed by
the target logic. This separates a problem of the host link from a problem of the internal bus
or of a state machine.

### Scripts and agents {#debug-agents}

The LiteX tools also give structured output, for a script or for an AI agent. `litex_cli` and
`litescope_cli` take `--json`. `litex_mcp` exposes the board as tools of the Model Context
Protocol, through a running `litex_server`.

```sh
litex_mcp --csr-csv=scripts/csr.csv --port=1234
litex_cli --csr-csv=scripts/csr.csv --ident --regs --filter=crossbar --json
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv -r <signal> --timeout 10 --json
```

`litex_mcp` needs the `mcp` Python package (`pip3 install mcp`). It starts with a check of the
link and of the SoC identity, then gives access to the registers by name, to the memory, to
LiteScope captures returned as data, to the console, to builds with their parsed timing and to
simulations. Writes, loads and flashes need its `--allow-write` option. The
LiteX wiki page
[LiteX for AI agents](https://github.com/enjoy-digital/litex/wiki/LiteX-for-AI-Agents)
describes it.

## Builds for debugging {#debug-builds}

Use `--load` for the debug iterations: the bitstream is volatile and the flash keeps a known
image. Flash only a validated bitstream ([Loading and flashing](#build-load)).

When a failure may be sensitive to timing, repeat the test at a lower system clock before
instrumenting the design. The Ethernet builds already default to 100 MHz; a PCIe build is
lowered with `--sys-clk-freq`.

```sh
./litex_m2sdr.py --variant=m2 --with-pcie --sys-clk-freq=100e6 --build --load
```

The files to look at after a build are the generated Verilog, the Vivado log and the timing
report of the build directory, and `scripts/csr.csv` ([The build flow](#build-flow)).

## PCIe link {#debug-pcie}

A board that does not appear in `lspci` has no link, or a link the host did not enumerate. The
link state is readable over JTAGBone, which does not depend on PCIe.

```sh
litex_server --jtag --jtag-config=openocd_xc7_ft4232.cfg
litex_cli --csr-csv=scripts/csr.csv --read=pcie_phy_link_status
```

| Field of `link_status` | Bits | Meaning |
|----------------------------------------|----------|--------------------------------------------------|
| `status` | 0 | 1: link up |
| `rate` | 1 | 0: 2.5 Gbps per lane, 1: 5 Gbps per lane |
| `width` | 3:2 | 0: one lane, 1: two lanes, 2: four lanes |
| `ltssm` | 9:4 | State of the link training state machine |

A build with `--with-pcie-probe` adds a tracer that records the transitions of the link training
state machine, and `scripts/test_pcie_ltssm.py` prints them with the names of the states.

```sh
./litex_m2sdr.py --variant=m2 --with-pcie --with-pcie-probe --build --load
python3 scripts/test_pcie_ltssm.py --csr-csv scripts/csr.csv
```

The script and the dashboard `test_dashboard.py` find the register of the tracer under its
name, `pcie_phy_ltssm_tracer_history`, and under the name that older bitstreams gave it. On a
build without the probe the script stops and names the missing option. `litex_cli` reads the
raw register.

```sh
litex_cli --csr-csv=scripts/csr.csv --read=pcie_phy_ltssm_tracer_history
```

Some hosts need several training attempts. `--with-pcie-reset-workaround` builds a block that
toggles the PCIe reset until the link comes up ([Host links](#host-links)).

After a load, the host must rescan the bus before it sees the board again
([After a load: PCIe rescan](#build-rescan)). The kernel driver has debug messages that can be
enabled at run time ([Driver debug messages](#gs-driver-debug)).

## Ethernet and Etherbone {#debug-eth}

Use this sequence for the register access over Ethernet, for the Ethernet streaming and for a
SoapySDR application over Ethernet. Check the board and the host path before starting the RF or
the streaming.

```sh
ping 192.168.1.50
cd litex_m2sdr/software/user
./m2sdr_util --device eth:192.168.1.50:1234 info
```

Bound the commands while debugging, so that a slow hardware timeout does not block the terminal:

```sh
timeout 6s ./m2sdr_rf
timeout 12s ./m2sdr_record /dev/null 67108864
```

### Loopback tests {#debug-loopback}

Run the loopback tests before involving antennas, demodulators or an SDR application. Each test
sends a known stream from the host and checks what comes back, and each one turns the stream
around at a deeper point of the path. [Loopbacks](#streaming-loopbacks) shows these points.

| Command | Turn-around point | Validates |
|--------------------------------------------|--------------------------|------------------------------|
| `fpga-loopback-test` | FPGA stream loopback | Host and host link, without the RFIC |
| `fpga-phy-loopback-test` | Data loopback of the FPGA RFIC PHY | The above, plus the sample packing of the RFIC side |
| `ad9361-loopback-test` | Internal digital loopback of the AD9361 | The above, plus the RFIC digital interface |

The tests reset the streaming path of the FPGA when they start and when they end. They stop the
Ethernet streamers, idle the crossbar, disable the loopbacks and the PRBS, disable the stream
headers and restore the 16-bit packing. They can therefore run right after an SDR application
or after another loopback test.

```sh
cd litex_m2sdr/software/user
make m2sdr_util
./m2sdr_util -i 192.168.1.50 info

# Host TX, FPGA RFIC PHY data loopback, host RX.
./m2sdr_util -i 192.168.1.50 \
    --duration 4 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-phy-loopback-test

# Host TX, AD9361 internal digital loopback, host RX.
./m2sdr_util -i 192.168.1.50 \
    --duration 8 --pace=rx --sample-rate 1920000 --window 32 \
    ad9361-loopback-test
```

A healthy run prints a compact progress line and ends with `PASS`. The output below is a
reference run, measured by the maintainers.

```text
[> FPGA PHY loopback test:
Device      : eth:192.168.1.50:1234
Sample rate : 1920000 S/s
Duration    : 4 s
Mode        : pace=rx, window=32 buffers
Path        : host TX -> FPGA RFIC data loopback -> host RX
RX 0.12 Gbps | checked 1811 buffers | errors 0
PASS: checked 7434 buffers, 0 errors, RX 0.12 Gbps
Note: synchronized after skipping ... startup lanes; discarded 1 stale startup buffer.
```

Add `--verbose` when a test fails or when the raw counters are needed: it prints the RF
initialisation log and the counter table.

```sh
./m2sdr_util -i 192.168.1.50 --verbose \
    --duration 4 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-phy-loopback-test
```

The warm-up at the start of `ad9361-loopback-test` is
expected, and it is ignored before the data check starts.

The pure FPGA loopback stresses the stream path of the host link without the rate limit of the
RFIC:

```sh
# Moderate paced run, as a sanity check.
./m2sdr_util -i 192.168.1.50 \
    --duration 3 --pace=rate --sample-rate 30720000 --window 32 \
    fpga-loopback-test

# Run close to the line rate.
./m2sdr_util -i 192.168.1.50 \
    --duration 3 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-loopback-test
```

Drops or data errors in the second run usually point to the pressure on the socket or on the
ring of the host, or to the pacing of the stream, not to the RFIC configuration. `--pace` takes
`rx`, `rate` or `none`; `--window` is the number of TX buffers allowed ahead of RX.

### RF initialisation failures over Ethernet {#debug-eth-rf}

| Message | Meaning |
|------------------------------------------------------------------|----------------------------------|
| `m2sdr_apply_config failed: timeout` | Etherbone did not complete a register access in time |
| `m2sdr_apply_config failed: io` | A register or RF access failed, often after repeated timeouts or on a wedged board |
| `Unsupported PRODUCT_ID 0x1` | Corrupted read of the AD9361 identifier |
| `ad9361_init : AD936x setup error -110` | The RFIC initialisation ended on a timeout |

An inconsistent AD9361 product identifier is not a change of chip. Check `m2sdr_util info`
again, then reload or reset the board if the SoC fields are incoherent too. After a timeout of
the RFIC initialisation, check that simple register reads work.

Read the status of the clock generator with the selected clocking mode in mind. With the
internal oscillator, the quick check is that the RF initialisation continues after the setup of
the clock generator and that the expected PLL is locked. A coherent `info` output and a
successful read of the AD9361 identifier tell more than one status warning.

### Stream state after a program exits {#debug-eth-cleanup}

A program that streams selects a source and a destination in the crossbar, and must release
them when it exits. When a cleanup is in doubt, read the two selection registers of the
crossbar.

```sh
./m2sdr_util reg-read 0xc800
./m2sdr_util reg-read 0xc804
```

`0xc800` is `crossbar_mux_sel`, the selected TX source, and `0xc804` is `crossbar_demux_sel`,
the selected RX destination ([Register reference](#registers)). These two addresses are fixed
in every build, since the crossbar has a fixed location in the register map. Both read 0 after
`m2sdr_record` exits or after a SoapySDR stream is deactivated and closed. A value that stays
selected lets the next process inherit an active stream path, and the symptoms look like stale
hardware state.

### UDP counters {#debug-eth-counters}

`m2sdr_record` prints the counters of the UDP receive path when it exits. A healthy short
capture has zero or near-zero drops.

```text
LiteEth UDP: rx_buffers=... rx_kernel_drops=0 rx_source_drops=0 rx_ring_full=0 rx_flushes=0 rx_recoveries=0 rx_recv_errors=0
LiteEth UDP SO_RCVBUF: requested=8388608 actual=...
```

| Counter | Meaning | Action |
|------------------------------|----------------------------|------------------------------------------|
| `rx_kernel_drops` | The receive queue of the socket overflowed | Raise `net.core.rmem_max`, lower the sample rate or the host load |
| `rx_source_drops` | Packets came from an unexpected source address | Check the board address, the routing, another sender on the port |
| `rx_ring_full` | The program did not drain the receive ring fast enough | Lower the rate or the host load |
| `rx_recoveries` | The RX path timed out, then was stopped, flushed and restarted once | Look for the cause of the timeout |
| `rx_recv_errors` | Receive errors of the socket | Check the permissions, the interface, another owner of the port |

Linux often caps the socket buffers below the requested size, and the tools then print
`LiteEth UDP SO_RCVBUF capped; increase net.core.rmem_max for more RX headroom.` Raise the limit
of the host before long captures at a high rate:

```sh
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608
```

### SoapySDR over Ethernet {#debug-eth-soapy}

Force the module and the device arguments, so that another installed module does not hide the
one under test. From `litex_m2sdr/software/user`, the SoapySDR build directory next to it can be
used directly. The first variable points SoapySDR to an empty directory, so that it loads only
the module of the second one.

```sh
export SOAPY_SDR_ROOT=/tmp/soapy-empty-root
export SOAPY_SDR_PLUGIN_PATH=../soapysdr/build
export LD_LIBRARY_PATH=$PWD/libm2sdr
SoapySDRUtil --probe=driver=LiteXM2SDR,eth_ip=192.168.1.50
```

A minimal RX check in Python, with the same environment, reads one buffer and prints two
sensors of the driver:

```python
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

sdr = SoapySDR.Device({"driver": "LiteXM2SDR", "eth_ip": "192.168.1.50"})
sdr.setSampleRate(SOAPY_SDR_RX, 0, 2e6)
stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
sdr.activateStream(stream)
buf = np.empty(4096, np.complex64)
r = sdr.readStream(stream, [buf], 4096)
print("read", r.ret)
print("source_drops", sdr.readSensor("liteeth_rx_source_drops"))
print("recoveries", sdr.readSensor("liteeth_rx_timeout_recoveries"))
sdr.deactivateStream(stream)
sdr.closeStream(stream)
```

GQRX takes the same device string, `driver=LiteXM2SDR,eth_ip=192.168.1.50`. When GQRX is slow to
start, compare with the probe and with the Python check. If those are fast, look at the sample
rate, the gain, the antenna and the demodulator of GQRX, not at the Etherbone path
([SDR frameworks](#frameworks)).

### 5GBASE-R PHY diagnostics {#debug-5g}

The 5GBASE-R mode needs an SFP module that presents a 64b/66b host interface at 5.15625 Gbaud,
such as the H!Fiber ASF-10G-T80 once switched to this host mode ([Host links](#host-links)). The
mode is Experimental ([Status](#intro-status)). The PHY has the
diagnostics registers of the LiteEth BASE-R PHY, readable over JTAGBone when the link is down.

```sh
litex_server --jtag --jtag-config=openocd_xc7_ft4232.cfg
litex_cli --csr-csv=scripts/csr.csv --read=eth_phy_status
litex_cli --csr-csv=scripts/csr.csv --read=eth_phy_rx_prbs_errors
```

`eth_phy_status` reports the 64b/66b block lock, the high bit error rate flag and the qualified
link state. A healthy link has the block lock set and the high bit error rate flag clear. Its
`link_up` bit is the status of the receive side only: it does not prove that the peer receives.

`eth_phy_control` selects the loopback of the transceiver and controls the PRBS31 generator and
checker; pulse `eth_phy_reset` after a change of loopback. On Artix-7, the near-end PMA loopback
can suppress the data valid signal of the receive gearbox, so a missing block lock in that mode
is not, alone, evidence of a failure of the PCS.

When the resets and the QPLL are healthy but the external path never reaches block lock, check
the protocol of the module on its host side. A module that advertises 2.5G, 5G or 10GBASE-T may
keep a fixed 10.3125 Gbaud host interface, or a rate adaptation mode of its vendor, until its
host mode is configured.

The fields of these registers are in the [Register reference](#reg-eth-phy-5g).

## LiteScope {#debug-litescope}

Use LiteScope once the registers and the generated Verilog have narrowed the question. Keep a
capture under 128 bits, in one clock domain, with a precise trigger: larger captures often fail
on this design, or make timing hard to close.

A build holds one probe. [Probes](#soc-probes) lists the probes, their signals and their clock
domains; choose the one that matches the question.

```sh
./litex_m2sdr.py --help | grep probe
./litex_m2sdr.py --variant=m2 --with-pcie --with-pcie-probe --build --load
./litex_m2sdr.py --variant=m2 --with-pcie --with-pcie-dma-probe --build --load
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-tx-probe --build --load
./litex_m2sdr.py --variant=baseboard --with-eth --eth-phy=2500basex \
    --with-eth-phy-rx-probe --build --load
./litex_m2sdr.py --with-ad9361-data-probe --build --load
```

The last command builds no host link: the capture is then read over JTAGBone. `litescope_cli`
runs through a `litex_server`, with the signal list `test/analyzer.csv` the build wrote.

```sh
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv --list
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv \
    --rising-edge <signal> --dump /tmp/capture.vcd
```

### Ethernet PHY RX probe {#debug-litescope-eth}

The Ethernet PHY RX probe captures the two raw 10-bit symbols the transceiver delivers per
cycle. In 2500BASE-X mode it samples them at 156.25 MHz, which keeps the analyzer out of the
312.5 MHz clock of the PCS.

Through JTAGBone, a capture without trigger on a quiet link shows the repeating idle ordered set
that starts with K28.5. To catch the start of a packet, trigger on the start symbol K27.7 on one
of the two symbol lanes. Its encoding is `0x05b` or `0x3a4`, depending on the running disparity.

```sh
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv \
    --value-trigger eth_phy_rx_symbol0 0x05b --offset 0x100 --dump /tmp/eth-rx.vcd
```

If one lane and one disparity do not trigger, try `eth_phy_rx_symbol1` and the other encoding.
Before a capture, read `eth_phy_pcs_status`: it gives the link state, the SGMII detection and
the ability word received from the link partner.

When the predefined probes are too wide, add a temporary narrow probe and commit only a version
that can be reused. For a one-off probe, record the captured signals and the conclusion in the
commit message or in a debug note.

## Verilog and timing {#debug-verilog}

Read the generated Verilog when the symptom depends on an address, a size, a decoder boundary, a
bus width, a reset or a clock domain. The file is flat and searchable.

```sh
grep -n "signal_or_module_name" build/<build>/gateware/<build>.v
grep -n -E "mem|sram|csr|wishbone|ack|cyc|stb|adr" build/<build>/gateware/<build>.v
grep -n -E "WNS|WHS|VIOLATED" build/<build>/gateware/vivado.log
grep -n -A 6 "Design Timing Summary" build/<build>/gateware/<build>_timing.rpt
```

The bus checks are on the wiki page cited at the top of this chapter: byte or word addressing,
dropped address bits, decoder boundaries, offsets inside the register space taken for bus
addresses. [Adding registers](#build-extend-registers) gives the rule for the last one.

To separate timing from logic, rebuild at a lower system clock and compare
([Builds for debugging](#debug-builds)). The `litex_build_report` tool of `litex_mcp` returns
the slacks, the failing paths and the utilisation of a build as data.

## Host library and software state {#debug-software}

Some failures are in the software state, not in the gateware. The symptoms are typical:

- a raw register access works, but a command of a utility or of the library fails;
- PCIe and Etherbone behave differently for the same command;
- a feature works once, then fails after a timeout, a reload or a stream stop.

The checks follow the same idea as for the gateware, from the lowest level up:

- reproduce with the lowest-level command available, such as `m2sdr_util reg-read` or a short
  `RemoteClient` script;
- compare `--device pcie:/dev/m2sdr0` and `--device eth:192.168.1.50:1234` when the bitstream
  has both host links;
- inspect the state machines of the host side, the cached capability data and the cleanup paths;
- add a unit test around the parsing, the validation, the state transition or the register
  sequence once the hardware behaviour is understood.

## Hardware scripts {#debug-scripts}

The Python scripts of `scripts/` read and drive a running board through `litex_server`. They
take the register map from `scripts/csr.csv`; the scripts without a `--csr-csv` option look for
`csr.csv` in the working directory, so run them from `scripts/`.

| Script | Task | Main options |
|------------------------------------|---------------------------------|-------------------------------|
| `test_clks.py` | Measure the six clocks of the clock measurement block | `--num`, `--delay` |
| `test_xadc.py` | Read the temperature and the voltages of the FPGA | `--csr-csv`, `--host` |
| `test_time.py` | Set and read the board time | `--set`, `--interval`, `--duration` |
| `test_header.py` | Read the last TX and RX headers and timestamps | `--frame-size`, `--loops` |
| `test_agc.py` | Read the saturation counters of the RFIC interface | `--agc`, `--threshold`, `--clear` |
| `test_led.py` | Drive the status LED by hand, or release it | `--preset`, `--pulse`, `--release` |
| `test_pcie_ltssm.py` | Print the link training transitions; see [PCIe link](#debug-pcie) | `--csr-csv`, `--port` |
| `test_dashboard.py` | Graphical dashboard of clocks, monitoring, time and headers | `--csr-csv`, `--host` |

```sh
litex_server --udp --udp-ip=192.168.1.50 --udp-port=1234
cd scripts
python3 test_clks.py
python3 test_time.py --csr-csv csr.csv --set now
python3 test_led.py --csr-csv csr.csv --list-presets
```

`test_clks.py` latches each counter of the clock measurement block twice and prints the
frequencies in MHz: system clock, PCIe clock, AD9361 reference clock, AD9361 data clock, board
time reference clock and FPGA 10 MHz clock. A clock at 0 MHz is missing. `test_dashboard.py`
needs the Dear PyGui Python package.

The other tools of `scripts/` test complete functions on hardware. They are listed here and
described with the function they test.

| Tool | Tests | Described in |
|--------------------------------------------------|------------------|--------------------------------|
| `loopback_selftest.sh` | Samples, timestamps and restarts over a TX to RX cable | [Scripts](#sw-scripts) |
| `timed_tx_selftest.c` | Timed TX latency and its calibration | [Streaming](#streaming) |
| `m2sdr_ptp_check.py` | Ethernet PTP lock | [Time synchronisation](#timing) |
| `m2sdr_pcie_time_sync.py` | Board time following the host over PCIe | [Time synchronisation](#timing) |
| `m2sdr_sata_transport_check.py` | SATA access over the selected host link | [Workflows](#workflows) |
| `evaluate_sample_formats.py` | Model of the loss of the transport formats, no board needed | [The RFIC interface](#rfic) |

`loopback_selftest_all_images.sh` runs the same sweep on two images. It exists because one
bitstream cannot cover every TX mode: the oversampling image transmits only at 122.88 MSPS in
2T2R ([Oversampling mode](#rfic-oversampling)). The script flashes each of the two images in
turn, rebuilds the software against its headers and runs the sweep for the configurations the
image supports. Both images must be built first.

`litex_m2sdr/software/autotest.py` is the board check of a PCIe board: PCIe device and link
speed, `m2sdr_util info` with the supply voltages, oscillator test, RF initialisation, DMA
loopback and RFIC loopback. Each step can be disabled with a `--disable-*` option.

## Tests {#debug-tests}

The repository has three levels of tests that need no board. Match the test to the layer that
changed.

```sh
# Gateware and script tests.
python3 -m pytest -v test

# Syntax check of the target script.
python3 -m py_compile litex_m2sdr.py

# Library unit tests, with both host links.
make -C litex_m2sdr/software/user test-libm2sdr

# The same with another default host link.
make -C litex_m2sdr/software/user test-libm2sdr INTERFACE=USE_LITEPCIE
make -C litex_m2sdr/software/user test-libm2sdr INTERFACE=USE_LITEETH
make -C litex_m2sdr/software/user test-libm2sdr INTERFACE=USE_LITEUSB

# Kernel driver build.
make -C litex_m2sdr/software/kernel clean all
```

### The pytest suite {#debug-tests-pytest}

`pytest.ini` sets `test/` as the test directory. The suite needs LiteX and its cores, no
simulator other than the Migen one and no toolchain. The tests fall into four groups.

| Group | Files in `test/` | What they check |
|----------------|-------------------------------------------------------------|-----------------------|
| Gateware blocks | `test_ad9361.py`, `test_agc.py`, `test_spi.py`, `test_rfic.py`, `test_header.py`, `test_loopback.py`, `test_time_core.py`, `test_pps.py`, `test_cdc.py`, `test_gpio.py`, `test_led.py`, `test_measurement.py`, `test_capability.py`, `test_si5351.py`, `test_vrt.py` | Each block in simulation, with stalls and corner cases |
| Clocking and links | `test_qpll.py`, `test_clock_discipline.py`, `test_ptp_discipline.py`, `test_ptp_identity.py`, `test_pcie_workaround.py`, `test_shared_etherbone.py`, `test_sata_streamers.py`, `test_sata_stream_tap.py`, `test_usb3_streamers.py` | QPLL channel allocation, discipline loops, Etherbone sharing, SATA and USB3 streamers |
| SoC elaboration | `test_cli_args.py`, `test_soc_eth_ptp.py`, `test_soc_eth_sata.py`, `test_soc_ptm.py`, `test_soc_clk10_discipline.py`, `test_soc_white_rabbit.py`, `test_soc_usb3.py`, `test_soc_pcie_dmas.py`, `test_csr_map.py` | Option rules, defaults, build names, registers of whole configurations, agreement with the committed `csr.h` |
| Scripts | `test_release.py`, `test_github_release.py`, `test_flash_release.py`, `test_ptp_check_script.py`, `test_pcie_time_sync_script.py`, `test_pcie_ltssm_script.py`, `test_sample_format_loss.py` | Release, flash and check scripts, without hardware |

The SoC elaboration tests build `BaseSoC` with a set of options and export its register map,
without Vivado. They are the tests to extend when an option or a register is added.
`test_csr_map.py` compares two of these maps with the header of the software
([The committed register header](#soc-csr-header)). The USB3 tests are skipped when USB3-PIPE
is not installed.

The White Rabbit tests need the `litex_wr_nic` package. They are skipped when it is absent, and
they expect the revision the continuous integration pins in `.github/workflows/ci.yml`. The run
below used a checkout of that revision; `<litex_wr_nic>` is its directory.

```console
$ PYTHONPATH=<litex_wr_nic> python3 -m pytest -v test
[...]
test/test_vrt.py::test_vrt_packet_structural_invariants_under_stalls PASSED [100%]

======================= 273 passed in 105.69s (0:01:45) ========================
```

### What the continuous integration runs {#debug-tests-ci}

| Workflow, job | Runs |
|----------------------------|------------------------------------------------------------------------|
| `ci.yml`, `build` | LiteX install, `./release.py --nobuild`, White Rabbit elaborations, `python3 -m pytest -v test` |
| `ci.yml`, `software-build` | Kernel driver, utilities and SoapySDR driver builds, with a check of their shared libraries |
| `build.yml`, `build` | Utilities and SoapySDR driver with each default host link and without the USB3 backend, library unit tests, the same under the address and undefined behaviour sanitizers, `cppcheck` on the library, examples |
| `doc.yml`, `check` | Figures and sources of this manual |

The `build` job of `ci.yml` also checks that the register appendix of this manual matches the
gateware.

The White Rabbit step of `ci.yml` also checks the error paths: White Rabbit refused on the `m2`
variant, a stale local `wr-cores` tree, and a `wr-cores` tree at the wrong revision.

### Where a new test goes {#debug-tests-new}

A fix of the LiteX framework or of a LiteX core is tested in its own repository, near the code:
a bug of the SoC builder or of the addressing belongs to the tests of LiteX, not only to this
project.

When a bug is found only on hardware, capture an invariant of it in a test that needs no
board: the behaviour of a parser, the computation of an address, the stability of the register
map, a generated constant or the behaviour of a host command.

## Reporting a result {#debug-report}

A result is useful to others when it can be reproduced. Record:

- the exact `./litex_m2sdr.py` command and the variant;
- the access path: PCIe, Etherbone, JTAGBone or crossover UART;
- the timing summary of the bitstream;
- the `m2sdr_util` or script commands, with their complete output.

Keep the reusable probes, scripts and tests in the tree.

## Troubleshooting {#debug-table}

| Symptom | Check first |
|---------------------------------------|-------------------------------------------------------------|
| Board absent from `lspci` after a load | Rescan the bus; `pcie_phy_link_status` over JTAGBone |
| Link up on one lane only | Slot width; `width` field of `pcie_phy_link_status` |
| `info` prints incoherent fields | Headers of the software against the bitstream; reload the board |
| RFIC looks dead on a new build | Timing report of the build |
| Oversampling build: one data lane always in error | Rebuild; see [Oversampling PHY](#rfic-oversampling-phy) |
| `--with-eth-vrt` build sends no VRT packet | Route not selected; see [VRT streaming](#flow-eth-vrt) |
| GPIO bits 2 and 3 have no effect | Only TP1 and TP2 are connected; see [Hardware](#hardware) |
| Register names read wrong values | `scripts/csr.csv` against the bitstream in the board |
| No answer to `ping` | SFP cage against `--eth-sfp`; PHY status registers over JTAGBone |
| RF initialisation times out over Ethernet | `info` first; bound the commands; reload if wedged |
| Next program starts with a stale stream | `crossbar_mux_sel` and `crossbar_demux_sel` |
| UDP drops at high rate | `net.core.rmem_max`; UDP counters of `m2sdr_record` |
| 5GBASE-R never locks | Host interface of the SFP module; `eth_phy_status` |
| LiteScope capture fails or build misses timing | Narrower probe, one clock domain |

## Where to go next {#debug-next}

[Building the gateware](#building-gateware) covers the builds and the host bridges,
[Register reference](#registers) the registers named here, [Host software](#software) the
utilities, and [Workflows](#workflows) the recipes to run once the board answers.
