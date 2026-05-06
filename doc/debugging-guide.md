# LiteX-M2SDR Debugging Guide

This document is a practical starting point for developers or agents debugging
LiteX-M2SDR gateware, software, and hardware behavior. Keep it current as new
features and debug flows are added.

The goal is to avoid rediscovering the same workflow each time: inspect the
design, reproduce the issue on hardware, use CSR access over Etherbone or PCIe,
add small observability points, inspect generated Verilog, and turn the finding
into a focused regression test whenever possible.

## First Pass

Start with the current state, not assumptions:

```bash
git status --short --branch
git log --oneline -5
rg -n "name_or_signal_or_csr" .
```

Do not edit generated software headers directly. Files such as
`litex_m2sdr/software/kernel/csr.h`, `mem.h`, and `soc.h` are generated from the
SoC build. If they are dirty, understand which build produced them before
including them in a commit.

Useful entry points:

- `litex_m2sdr.py`: top-level SoC, build options, CSR map, LiteScope probes.
- `litex_m2sdr/gateware/`: board-specific gateware blocks.
- `litex_m2sdr/software/user/libm2sdr/`: common PCIe/Etherbone host API.
- `litex_m2sdr/software/user/m2sdr_util.c`: low-level board utility.
- `scripts/`: hardware scripts using LiteX `RemoteClient`.
- `test/`: CI-safe Python/gateware tests.
- `.github/workflows/`: reference CI commands.

## Build And Load

Default `sys_clk_freq` is 125 MHz. If a failure might be timing-sensitive,
repeat the test at a lower system clock before instrumenting heavily:

```bash
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --sys-clk-freq=100e6 --build --load
```

Common Ethernet/Etherbone build on the baseboard:

```bash
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build --load
ping 192.168.1.50
```

Common PCIe build:

```bash
./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=1 --build --load
lspci
```

Host software can be switched between the two transports from
`litex_m2sdr/software/`:

```bash
cd litex_m2sdr/software
sudo ./build.py --interface=liteeth
sudo ./build.py --interface=litepcie
```

For faster local rebuilds while iterating only on user-space code:

```bash
make -C litex_m2sdr/software/user INTERFACE=USE_LITEETH all
make -C litex_m2sdr/software/user INTERFACE=USE_LITEPCIE all
```

Use `--load` for volatile debug iterations. Use flash flows only when the
bitstream has already been validated or when persistent boot behavior is the
thing being tested.

Generated artifacts are under `build/<build_name>/`, for example:

- `build/<build_name>/gateware/<build_name>.v`
- `build/<build_name>/gateware/vivado.log`
- `build/<build_name>/gateware/<build_name>_timing.rpt`
- `scripts/csr.csv`

## Host Access

There are two normal ways to interact with hardware: project tools and
`litex_server`/`RemoteClient`.

Project tools are best when the feature already has host support:

```bash
cd litex_m2sdr/software/user
./m2sdr_util info
./m2sdr_util scratch-test
./m2sdr_util reg-read 0x0
./m2sdr_util --device eth:192.168.1.50:1234 info
./m2sdr_util --device pcie:/dev/m2sdr0 info
```

Use `litex_server` when writing small Python probes, using LiteScope, or working
with CSRs that do not yet have a host utility:

```bash
# Ethernet/Etherbone. The M2SDR Ethernet build uses a 32-bit Etherbone path.
litex_server --udp --udp-ip=192.168.1.50 --udp-port=1234

# PCIe. Replace the BDF with the board's BAR.
sudo litex_server --pcie --pcie-bar=04:00.0
```

Then use `RemoteClient` scripts in another terminal:

```bash
python3 scripts/test_led.py --csr-csv scripts/csr.csv
python3 scripts/test_time.py --csr-csv scripts/csr.csv
python3 scripts/test_pcie_ltssm.py --csr-csv scripts/csr.csv
```

If the design exposes a crossover UART, use it through LiteX:

```bash
litex_term crossover --csr-csv scripts/csr.csv
```

When debugging a hang, also check whether host writes accumulate but the target
logic does not consume them. That often separates host transport problems from
internal bus or state-machine problems.

## LiteEth And Etherbone

Use this sequence when debugging Ethernet access, LiteEth streaming, or
Soapy/Gqrx over Ethernet.

First verify the board and host path before starting RF or streaming:

```bash
ping 192.168.1.50
cd litex_m2sdr/software/user
./m2sdr_util --device eth:192.168.1.50:1234 info
./m2sdr_util info
```

The `info` output should be coherent: correct SoC identifier, API version,
board variant, Ethernet enabled, plausible FPGA temperature and voltages, and
AD9361 presence/product ID. If the output contains shifted strings, API version
`0.0`, impossible temperatures, reserved variants, or missing features on a
known-good bitstream, suspect corrupted Etherbone reads or a wedged board before
debugging RF state.

Use bounded commands while debugging failures so a slow hardware timeout does
not trap the terminal:

```bash
timeout 6s ./m2sdr_rf
timeout 12s ./m2sdr_record /dev/null 67108864
```

### Ethernet Loopback Diagnostics

Use the loopback tests before involving antennas, demodulators, or Gqrx. They
exercise increasingly complete pieces of the Ethernet/RF path and print compact
progress by default. Add `--verbose` to recover the detailed RF init logs and
counter table.

The tests reset the FPGA streaming datapath at startup and cleanup: LiteEth
streamers are stopped, crossbars are idled, FPGA loopbacks/PRBS are disabled,
stream headers are disabled, and 16-bit packing is restored. This makes the
checks suitable after a Gqrx/Soapy session or after another loopback test.

Build the utility for the Ethernet transport first:

```bash
cd litex_m2sdr/software/user
make m2sdr_util INTERFACE=USE_LITEETH
./m2sdr_util -i 192.168.1.50 info
```

Recommended quick checks:

```bash
# Host TX -> FPGA AD9361 PHY data loopback -> host RX.
./m2sdr_util -i 192.168.1.50 \
    --duration 4 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-phy-loopback-test

# Host TX -> AD9361 internal digital loopback -> host RX.
./m2sdr_util -i 192.168.1.50 \
    --duration 8 --pace=rx --sample-rate 1920000 --window 32 \
    ad9361-loopback-test
```

A healthy compact run looks like:

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

Use `--verbose` when a test fails or when you need the raw counters:

```bash
./m2sdr_util -i 192.168.1.50 --verbose \
    --duration 4 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-phy-loopback-test
```

The pure FPGA stream loopback is useful for stressing the Ethernet host/FPGA
stream path without the RFIC rate limit:

```bash
# Moderate paced run for a quick formatting/sanity check.
./m2sdr_util -i 192.168.1.50 \
    --duration 3 --pace=rate --sample-rate 30720000 --window 32 \
    fpga-loopback-test

# Near line-rate stress run. Drops or data errors here usually point to host
# socket/ring pressure or stream pacing, not RFIC configuration.
./m2sdr_util -i 192.168.1.50 \
    --duration 3 --pace=rx --sample-rate 1920000 --window 32 \
    fpga-loopback-test
```

Loopback command meanings:

- `fpga-loopback-test`: host TX to FPGA stream loopback to host RX; isolates
  the Ethernet stream path from the AD9361.
- `fpga-phy-loopback-test`: host TX through the FPGA AD9361 PHY data loopback
  to host RX; validates the host/Ethernet path plus RFIC-side stream packing.
- `ad9361-loopback-test`: host TX through AD9361 internal digital loopback to
  host RX; validates the RFIC digital loopback path. The initial warmup is
  expected and is ignored before data checking starts.

Typical LiteEth RF-init failures:

- `m2sdr_apply_config failed: timeout`: Etherbone did not complete a register
  transaction quickly enough.
- `m2sdr_apply_config failed: io`: a lower-level register/RF transaction
  failed; this can happen after repeated timeouts or while the board is wedged.
- `Unsupported PRODUCT_ID 0x1` or inconsistent AD9361 product IDs: do not
  assume a real RFIC ID change. First recheck `m2sdr_util info`, then reset or
  reload the board if the SoC fields are also incoherent.
- `ad9361_init : AD936x setup error -110`: RFIC initialization reached a Linux
  timeout-style error path. Keep the command bounded and check whether simple
  CSR reads still work afterward.

The SI5351 status should be interpreted with the selected clocking mode in
mind. For the internal-XO baseboard flow, the important quick check during RF
init is that the code can continue after SI5351 setup and the expected PLL path
is locked. A status warning alone is less useful than a coherent `info` dump
plus a successful AD9361 product-ID read.

When stream cleanup is suspected, verify the stream-selection CSR after the
program exits. On the current baseboard CSR map used during Ethernet debug,
`CSR_CROSSBAR_MUX_SEL_ADDR` was `0xc804`; prefer the generated CSR name when
available and treat the literal address as build-specific:

```bash
./m2sdr_util reg-read 0xc804
```

The value should be `0x00000000` after `m2sdr_record` exits or after a Soapy
stream is deactivated and closed. If it remains selected, the next process can
inherit an active stream path and symptoms can look like stale hardware state.

`m2sdr_record` prints LiteEth UDP counters on exit. A healthy short receive
test should have zero or near-zero drops:

```text
LiteEth UDP: rx_buffers=... rx_kernel_drops=0 rx_source_drops=0 rx_ring_full=0 rx_flushes=0 rx_recoveries=0 rx_recv_errors=0
LiteEth UDP SO_RCVBUF: requested=8388608 actual=...
```

Use the counters to separate failure classes:

- `rx_kernel_drops`: the Linux socket receive queue overflowed. Increase
  `net.core.rmem_max`, reduce sample rate, or reduce host load.
- `rx_source_drops`: packets arrived from an unexpected source IP. Check the
  configured board IP, local routing, and whether another sender is using the
  stream port.
- `rx_ring_full`: user-space did not drain the LiteEth UDP ring fast enough.
- `rx_recoveries`: the RX path timed out, then libm2sdr/Soapy stopped, flushed,
  reactivated, and retried the stream once.
- `rx_recv_errors`: socket-level receive errors. Check host permissions,
  interface state, and whether another process owns the needed port.

Linux often caps socket buffers below the requested value. If the tools report:

```text
LiteEth UDP SO_RCVBUF capped; increase net.core.rmem_max for more RX headroom.
```

increase the host limit before long high-rate captures:

```bash
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608
```

For Soapy/Gqrx Ethernet tests, force the intended plugin and device arguments
so an installed PCIe/default plugin does not hide what is being tested:

```bash
SOAPY_SDR_ROOT=/tmp/soapy-empty-root \
SOAPY_SDR_PLUGIN_PATH=/tmp/litex_m2sdr_soapysdr_liteeth \
LD_LIBRARY_PATH=$PWD/libm2sdr \
SoapySDRUtil --probe=driver=LiteXM2SDR,eth_ip=192.168.1.50
```

For a minimal RX sample check:

```bash
SOAPY_SDR_ROOT=/tmp/soapy-empty-root \
SOAPY_SDR_PLUGIN_PATH=/tmp/litex_m2sdr_soapysdr_liteeth \
LD_LIBRARY_PATH=$PWD/libm2sdr \
python3 -c 'import numpy as np, SoapySDR; from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32; s=SoapySDR.Device({"driver":"LiteXM2SDR","eth_ip":"192.168.1.50"}); s.setSampleRate(SOAPY_SDR_RX,0,2e6); st=s.setupStream(SOAPY_SDR_RX,SOAPY_SDR_CF32,[0]); s.activateStream(st); b=np.empty(4096,np.complex64); r=s.readStream(st,[b],4096); print("read", r.ret); print("source_drops", s.readSensor("liteeth_rx_source_drops")); print("recoveries", s.readSensor("liteeth_rx_timeout_recoveries")); s.deactivateStream(st); s.closeStream(st)'
```

Gqrx should use a device string that selects the LiteX-M2SDR Soapy driver and
the Ethernet board IP, for example `driver=LiteXM2SDR,eth_ip=192.168.1.50`.
If Gqrx appears slow during startup, compare with `SoapySDRUtil --probe` and
the Python sample check above. If those are fast, focus on Gqrx sample-rate,
gain, antenna, and demodulator settings rather than the Etherbone path.

## Adding CSRs

Prefer a small CSR or status counter before a wide trace when the question can
be answered by software polling. Typical useful CSRs:

- last address accepted by a bus master or slave;
- transaction counters;
- error/timeout counters;
- FSM state snapshots;
- sticky flags that can be cleared by software.

When adding a new gateware block with CSRs:

1. Add the block in `litex_m2sdr/gateware/` or the appropriate existing module.
2. Instantiate it from `litex_m2sdr.py`.
3. Reserve a stable name in `BaseSoC.csr_map` when the CSR location is part of
   the software ABI or useful for repeatable debug.
4. Rebuild so `scripts/csr.csv` and generated software headers match the SoC.
5. Add host-side access through `libm2sdr`, `m2sdr_util`, or a focused script
   when the CSR is expected to survive beyond one debug session.

For a gateware master that accesses CSRs through the SoC main bus, use the full
main-bus byte address:

```python
csr_base = self.get_csr_address("peripheral_name")
```

Do not pass `self.csr.address_map("name", origin=True)` directly to a main-bus
master. That value is a CSR-local offset. It is useful inside the CSR space, but
it is not the full SoC bus address.

## LiteScope

Use LiteScope after narrowing the question with CSRs and generated Verilog.
Keep captures small. As a rule, stay below 128 captured signal bits; up to 256
bits can work, but larger captures often fail or make builds hard to close.

Prefer:

- one bus channel rather than every bus in the design;
- address/control/FSM bits before wide data paths;
- a short depth with a precise trigger before a deep blind capture;
- one clock domain per analyzer.

List the available probe switches from the target script, then enable only the
probe that matches the current question:

```bash
./litex_m2sdr.py --help | rg probe
./litex_m2sdr.py --variant=m2 --with-pcie --with-pcie-probe --build --load
./litex_m2sdr.py --variant=m2 --with-pcie --with-pcie-dma-probe --build --load
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-tx-probe --build --load
./litex_m2sdr.py --with-ad9361-data-probe --build --load
```

Use `litescope_cli` through a running `litex_server`:

```bash
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv --list
litescope_cli --csv test/analyzer.csv --csr-csv scripts/csr.csv --rising-edge <signal> --dump /tmp/capture.vcd
```

If the predefined probes are too wide, add a temporary narrow probe and commit
only the reusable version. For a one-off analyzer, document the captured signals
and the conclusion in the commit or debug note.

## Verilog And Timing Analysis

Always inspect the generated RTL when the symptom depends on an address, size,
decoder boundary, bus width, reset, or clock domain. Useful searches:

```bash
rg -n "signal_or_module_name" build/<build_name>/gateware/<build_name>.v
rg -n "mem|sram|csr|wishbone|ack|cyc|stb|adr" build/<build_name>/gateware/<build_name>.v
rg -n "WNS|WHS|VIOLATED|Timing" build/<build_name>/gateware/vivado.log build/<build_name>/gateware/*timing*.rpt
```

For bus issues, check these explicitly:

- byte-addressed versus word-addressed Wishbone interfaces;
- address bits dropped by adapters, usually `adr[31:2]` for 32-bit word buses;
- region decoder boundaries when sizes cross powers of two;
- generated constants assigned to bus addresses;
- whether a master uses a CSR-local offset where a full main-bus address is
  required.

Positive timing is necessary but not proof of functional correctness. If timing
is suspected, compare:

- default `sys_clk_freq` versus a lower one, such as 100 MHz;
- timing directives and WNS/WHS;
- a minimal SoC versus the full SoC;
- another CPU only when the failure could plausibly be CPU-specific.

## Investigation Loop

Use this loop for difficult issues:

1. Reproduce the failure and write down the exact build command, bitstream,
   board transport, host command, and observed behavior.
2. Establish a known-good build and one minimal failing change.
3. Verify the host path with a simple CSR read/write or `m2sdr_util info`.
4. Inspect generated maps: `scripts/csr.csv`, `csr.h`, `mem.h`, and generated
   Verilog.
5. Add a CSR counter/status if polling can answer the next question.
6. Add a narrow LiteScope probe if the issue is temporal or bus-handshake
   related.
7. Rebuild, load, capture, and record the hardware result.
8. Fix the root cause in the smallest appropriate layer.
9. Add a regression test or host-side check that would have caught it.
10. Commit the fix separately from exploratory instrumentation unless the
    instrumentation is intentionally reusable.

Avoid jumping directly to broad instrumentation. The fastest path is often:
hardware symptom, generated Verilog, one narrow trace, then fix.

## Regression Tests

Match the test to the layer that failed:

```bash
# Python/gateware tests.
python3 -m pytest -v test

# Syntax smoke checks for edited Python entry points.
python3 -m py_compile litex_m2sdr.py

# User-space C tests, PCIe-oriented build.
make -C litex_m2sdr/software/user test-libm2sdr

# User-space C tests, Etherbone-oriented build.
make -C litex_m2sdr/software/user test-libm2sdr INTERFACE=USE_LITEETH

# Kernel build smoke check.
make -C litex_m2sdr/software/kernel clean all
```

For LiteX framework fixes, add tests in the LiteX repository near the affected
code. For example, a SoC builder/addressing bug belongs in LiteX's SoC tests,
not only in this project.

When a bug is discovered only on hardware, still try to capture a software or
unit-level invariant from it: parser behavior, address computation, CSR map
stability, generated constant, or host command behavior.

## Common Debug Patterns

The same few patterns explain many gateware/software mismatches. Before adding
large traces, check whether the failure fits one of these categories.

### Addressing And Decode

Symptoms:

- a host bridge still answers, but one internal master or slave appears stuck;
- changing a memory or CSR region size changes unrelated behavior;
- a bus transaction reaches the wrong slave;
- generated RTL contains an unexpected address constant.

Checks:

- confirm whether each bus interface is byte-addressed or word-addressed;
- inspect adapters for dropped low address bits;
- compare generated decoder masks against the intended region map;
- verify that any gateware bus master uses full SoC bus addresses, not local
  offsets from a sub-bus;
- read back a harmless register through the same transport before testing the
  failing path.

If a gateware master reaches a CSR through the main bus, use
`self.get_csr_address("name")` for the base address. Keep
`self.csr.address_map("name", origin=True)` for CSR-local reasoning only.

### Timing Versus Logic

Symptoms:

- the same bitstream behaves differently across builds or tool directives;
- timing reports are close to zero slack;
- lowering `sys_clk_freq` changes the symptom.

Checks:

- compare WNS/WHS for working and failing builds;
- retry with a lower `sys_clk_freq`;
- inspect clock-domain crossings and reset release paths;
- use LiteScope only after deciding which handshake or FSM should be observed.

Positive timing does not prove the logic is correct. Treat timing experiments as
a way to separate marginal implementation behavior from deterministic design
bugs.

### Host API And Software State

Symptoms:

- raw register access works, but a utility or library command fails;
- PCIe and Etherbone behave differently for the same user command;
- a feature works once and then fails after a timeout, reload, or stream stop.

Checks:

- reproduce the issue with the lowest-level available command, such as
  `m2sdr_util reg-read` or a short `RemoteClient` script;
- compare `--device pcie:/dev/m2sdr0` and `--device eth:192.168.1.50:1234`
  when the bitstream supports both transports;
- inspect host-side state machines, cached capability data, and cleanup paths;
- add a unit test around parsing, validation, state transition, or register
  sequence logic once the hardware behavior is understood.

### Capture Only What Answers The Question

A good capture has a narrow question: "does this master issue a read?", "does
the slave acknowledge?", "which FSM state holds?", or "which endpoint receives
the packet?" Capture the minimum signals needed to answer that question, then
iterate.

## Capturing Results

A useful debug commit or note should include:

- exact build command;
- transport used: PCIe, Etherbone, JTAGBone, or crossover UART;
- bitstream timing summary when relevant;
- host commands used to reproduce or validate;
- register values, LiteScope signal names, or Verilog snippets that prove the
  conclusion;
- tests run afterward.

Keep exploratory artifacts out of commits unless they are intentionally part of
the project. Keep reusable probes, scripts, and tests in the tree so the next
person can start one level higher.
