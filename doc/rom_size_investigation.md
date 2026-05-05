# Integrated ROM Size Investigation

## Baseline

Command:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --integrated-rom-size=0x8000 --no-integrated-rom-auto-size --build
```

Result on hardware:

- Integrated ROM region generated at `0x00000000`, size `0x00008000`.
- BIOS ROM usage: 22.80 KiB.
- Bitstream loaded over FT4232 with `openFPGALoader`.
- `litex_server --udp --udp-ip 192.168.1.50` plus `litex_term --csr-csv scripts/csr.csv crossover` reached an interactive `litex>` prompt.
- BIOS reported `ROM: 32.0KiB`, CRC passed, and the `help` command responded over the crossover UART.

Timing note:

- This local default-directive build completed bitstream generation but did not meet timing: final WNS was `-1.589 ns`, WHS was `+0.036 ns`.

## Larger ROM Test

Command:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size --build
```

Build observations:

- Integrated ROM region generated at `0x00000000`, size `0x00010000`.
- BIOS ROM usage stayed at 22.80 KiB.
- The generated Wishbone regions, CSR map, bus masters/slaves, and crossover UART placement matched the `0x8000` build except for the ROM region size.
- Vivado inferred `rom_dat0_reg` as `16384x32` Block RAM instead of `8192x32`.
- The ROM BRAM instance count doubled from 8 `rom_dat0_reg_*` instances to 16. Overall RAMB usage increased from 31 to 39 `RAMB36E1`; `RAMB18E1` usage stayed at 9.

Result on hardware:

- Bitstream loaded over FT4232 with `openFPGALoader`.
- `litex_term --csr-csv scripts/csr.csv crossover` produced no BIOS banner and did not respond to `help`.
- Etherbone CSR access remained functional: `ctrl_scratch` read back `0x12345678`.
- Before injection, `uart_xover_rxempty=1` and `uart_xover_txfull=0`.
- After injecting 24 bytes through `uart_xover_rxtx`, the CPU-facing UART RX FIFO was full (`uart_rxempty=0`, `uart_rxfull=1`) and the crossover sender was full (`uart_xover_txfull=1`), indicating that bytes were not being consumed by the BIOS.

Timing note:

- This local default-directive build completed bitstream generation but did not meet timing: final WNS was `-1.590 ns`, WHS was `+0.063 ns`.

## ROM Readback Probes

The generated ROM initialization files for both sizes start with the same BIOS words:

```text
0b00006f
00000013
00000013
00000013
```

The expected first 32-bit word on the Wishbone bus is therefore `0x0b00006f`.

`0x8000` result:

- `ctrl_scratch` read back `0x12345678`.
- Reading `0x00000000..0x0000003f` over Etherbone returned the expected BIOS words:
  `0x0b00006f`, `0x00000013`, ..., `0xfe112e23`, ...
- A subsequent `ctrl_scratch` read still returned `0x12345678`, so the ROM read did not disturb Etherbone access.

`0x10000` result:

- `ctrl_scratch` read back `0x12345678` before the ROM access.
- A single-word read at `0x00000000` returned `0x00000000` instead of `0x0b00006f` and caused `litex_server` to report a UDP read timeout.
- After that ROM access, subsequent CSR reads returned `0x00000000` and caused additional UDP read timeouts until the FPGA was reloaded.
- Repeating the test with the CPU held in reset through `ctrl_reset=0x00000002` did not change the result: reading `0x00000000` still returned `0x00000000` and timed out the Etherbone path.

Interpretation:

- The ROM initialization data generated for the larger image appears correct.
- The failing behavior is visible from an external Etherbone bus master, not only from the VexRiscv instruction path.
- Since the failure persists while the CPU reset bit is held, it is less likely to be only the CPU monopolizing the ROM slave after a bad fetch.
- The generated RTL for the ROM is structurally the same except for depth/address width: `8192` words with `basesoc_basesoc_adr[12:0]` versus `16384` words with `basesoc_basesoc_adr[13:0]`; the acknowledge logic remains the same.

## RTL and Vivado Log Review

The crossbar decoder change at the `0x8000` to `0x10000` boundary looks like the expected word-addressed decode change:

- `0x8000`: ROM select checks requester address bits `[29:13] == 0`.
- `0x10000`: ROM select checks requester address bits `[29:14] == 0`.

That matches `0x8000/4 = 8192` words and `0x10000/4 = 16384` words. The integrated ROM local address also grows as expected from 13 to 14 bits.

The generated ROM logic is otherwise unchanged:

- `0x8000`: `reg [31:0] rom[0:8191]`
- `0x10000`: `reg [31:0] rom[0:16383]`
- both use `$readmemh(...)`, one synchronous read data register, and the same Wishbone acknowledge equation.

The Vivado logs do not show a ROM-specific critical warning or error. The notable implementation change is the BRAM inference cliff:

- `0x8000`: `8192x32` Block RAM, 8 `rom_dat0_reg_*` instances.
- `0x10000`: `16384x32` Block RAM, 16 `rom_dat0_reg_*` instances.
- Overall RAMB36 usage increases by 8; RAMB18 usage stays unchanged.

Vivado emits the same class of optional output register merge warnings for the ROM BRAMs in both builds, with twice as many instances in the larger build. I do not see an obvious RTL decoder overlap, bad base address, or missing ROM initialization in the generated files.

## ROM Bus LiteScope Probe

`--with-rom-bus-probe` adds a LiteScope analyzer focused on the integrated ROM read path. Example debug build:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --with-rom-bus-probe --rom-bus-probe-depth=512 --build
```

The probe captures:

- Etherbone Wishbone address low 16 bits, read data, and `cyc/stb/ack/we/err`.
- Integrated ROM slave address low 16 bits, read data, and `cyc/stb/ack/we/err`.
- `ctrl.cpu_rst`, `ctrl.bus_error`, CPU reset, and sys reset mirror
- decoded ROM-access flags for Etherbone, ibus, and dbus
- decoded ROM slave read/ack flags

The probe is intentionally trimmed to stay below the usual LiteScope practical width limit. A generated probe was elaborated successfully without running Vivado. `test/analyzer.csv` reported a 114-bit capture group; the default depth is 512 samples and can be reduced with `--rom-bus-probe-depth`.

Example capture flow after loading a probe bitstream:

```sh
litex_server --udp --udp-ip 192.168.1.50
python3 -m litescope.software.litescope_cli \
    --csv test/analyzer.csv --csr-csv scripts/csr.csv \
    --rising-edge basesoc_rom_probe_eth_rom_access \
    --dump rom_read.vcd
```

Then issue a single external ROM read through the normal LiteX remote path. The key first check in the waveform is whether the Etherbone Wishbone read reaches `rom.bus`, whether `rom.bus.ack` pulses, and whether `rom.bus.dat_r` contains `0x0b00006f` at address zero.

### Probe Hardware Test

A `0x10000` ROM probe build was generated and loaded with:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --with-rom-bus-probe --rom-bus-probe-depth=512 --build
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --with-rom-bus-probe --rom-bus-probe-depth=512 --load
```

The generated analyzer remained below the practical LiteScope width limit: `data_width=114`, `depth=512`.
The probe build completed bitstream generation, but did not meet timing: post-route WNS was `-1.593 ns`, so the hardware traces are diagnostic rather than a timing-clean proof.

Hardware result:

- The bitstream loaded successfully over FT4232.
- `ctrl_scratch` read back `0x12345678`, confirming that Etherbone CSR access was alive after load.
- The crossover UART stayed silent: no BIOS banner and no response to `help`.
- With the CPU reset bit asserted through `ctrl_reset=0x00000002`, an external Etherbone read from `0x00000000` returned `0x00000000` instead of `0x0b00006f`.
- That external ROM read caused `litex_server` UDP read timeouts; after the read, `ctrl_scratch` also read back `0x00000000` until the FPGA was reloaded.
- A LiteScope capture was armed on the Etherbone ROM-access trigger, but the same external ROM read broke the remote path before the completed capture could be drained back over CSR.

A second capture was taken around CPU reset release, without doing an external ROM read. This capture could be drained successfully. It showed:

- `ctrl.cpu_rst` and `cpu.reset` asserted, then deasserted as expected.
- The integrated ROM slave port remained active with `cyc=1`, `stb=1`, `we=0`.
- `rom.bus.ack` pulsed repeatedly.
- `rom.bus.adr[15:0]` stayed at `0x2805`, and `rom.bus.dat_r` stayed at `0x00000000`.
- `0x2805` is in the zero-padded area of the generated ROM initialization file, so the observed read data at that word is consistent with the image contents.

This points away from missing ROM initialization and toward the bus/mux/arbiter side of the failure. The ROM slave is acknowledging a stale or unexpected read in the larger-ROM design, and an external ROM read can wedge the Etherbone path badly enough that LiteScope cannot be read back afterward.

### 100 MHz Timing Check

A timing-clean `0x10000` ROM build was generated and loaded with a reduced system clock:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --build
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --load
```

Build result:

- The SoC used a `100.000MHz` system clock.
- Etherbone remained on the normal 32-bit Wishbone path.
- The ROM was still inferred as `16384x32` Block RAM.
- Final post-route timing was clean: WNS `+0.493 ns`, TNS `0.000 ns`, WHS `+0.024 ns`, THS `0.000 ns`.

Hardware result:

- The timing-clean bitstream loaded successfully over FT4232.
- Etherbone CSR access was alive immediately after load: `ctrl_scratch` read back `0x12345678`.
- The crossover UART still stayed silent: no BIOS banner and no response to `help`.
- With the CPU reset bit asserted through `ctrl_reset=0x00000002`, an external Etherbone read from `0x00000000` still returned `0x00000000` instead of `0x0b00006f`.
- That external ROM read again caused `litex_server` UDP read timeouts; after the read, `ctrl_scratch` read back `0x00000000` until reload.

This makes a simple 125 MHz timing-closure explanation unlikely. The failing behavior survives with positive post-route setup and hold timing at 100 MHz, so the next investigation step should focus on the ROM slave arbitration/muxing and other bus masters.

Useful next checks:

- Sweep sizes just above the boundary, such as `0x9000`, `0xc000`, and `0x10000`, to see whether the issue is tied to inferred BRAM depth, power-of-two decode size, or a specific implementation layout.
- Use the ROM bus LiteScope probe to compare a known-good `0x8000` ROM read with a failing `0x10000` ROM read.
- Revise the probe to include the SI5351 sequencer Wishbone master and/or the ROM arbiter grant. The observed `0x2805` low address matches the SI5351 status CSR low word address (`0xf000a014 >> 2`), so this is worth ruling out explicitly.
- Try adding one explicit extra read latency cycle to the integrated ROM path. The larger BRAM tree may be returning invalid data when acknowledged with the current one-cycle SRAM wrapper.
- Build a reduced SoC with only CPU, integrated ROM/SRAM, crossover UART, and Etherbone to remove unrelated placement pressure before debugging the full design.

### SERV CPU Comparison

The target now accepts `--cpu-type`, defaulting to `vexriscv`. This was used to build a SERV comparison image while keeping the same 100 MHz system clock and 64 KiB integrated ROM:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --cpu-type=serv --cpu-variant=standard \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --build
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --cpu-type=serv --cpu-variant=standard \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --load
```

Build result:

- CPU changed to SERV with two Wishbone masters.
- ROM stayed at `0x00000000`, size `0x00010000`.
- SERV changed the default SRAM origin to `0x01000000` and CSR origin to `0x82000000`.
- The crossbar topology remained `4 <-> 3`: `cpu_bus0`, `cpu_bus1`, `si5351`, and `etherbone` masters against ROM, SRAM, and CSR slaves.
- Final post-route timing was clean: WNS `+0.404 ns`, TNS `0.000 ns`, WHS `+0.053 ns`, THS `0.000 ns`.

Hardware result:

- The bitstream loaded successfully over FT4232.
- Etherbone CSR access was alive immediately after load: `ctrl_scratch` read back `0x12345678` at the SERV CSR base.
- The crossover UART stayed silent: no BIOS banner and no response to `help`.
- With the CPU reset bit asserted through `ctrl_reset=0x00000002`, an external Etherbone read from `0x00000000` returned `0x00000000` instead of `0x0b00006f`.
- After that ROM read, `ctrl_scratch` read back `0x00000000` and `litex_server` reported UDP read timeouts.

This reproduces the failure with a different CPU and a timing-clean 100 MHz build. The issue is therefore unlikely to be specific to VexRiscv instruction fetch/cache behavior. The common failing surface remains the 64 KiB integrated ROM access through the full crossbar with the other bus masters present.

### Banked ROM Comparison

The target now accepts `--with-banked-integrated-rom`, a diagnostic option that keeps one public 64 KiB `rom` bus region at `0x00000000` but backs it with two 32 KiB memories. The low bank is selected by Wishbone word address bit 13 equal to zero, and the high bank by bit 13 equal to one.

Command:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --with-banked-integrated-rom \
    --build
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-cpu \
    --sys-clk-freq=100e6 \
    --integrated-rom-size=0x10000 --no-integrated-rom-auto-size \
    --with-banked-integrated-rom \
    --load
```

Build result:

- The public bus map stayed unchanged: ROM at `0x00000000`, size `0x00010000`; SRAM at `0x10000000`; CSR at `0xf0000000`.
- The SoC hierarchy showed `rom (BankedROM)` with `bank0` and `bank1`.
- Vivado inferred `rom_bank0_dat0_reg` as `8192x32` Block RAM. The high bank was mostly optimized because the BIOS image is below 32 KiB and the upper 32 KiB is zero-padded.
- The bank0 initialization file starts with the expected BIOS word sequence: `0b00006f`, `00000013`, ...
- Final post-route timing was clean: WNS `+0.349 ns`, TNS `0.000 ns`, WHS `+0.056 ns`, THS `0.000 ns`.

Hardware result:

- The bitstream loaded successfully over FT4232.
- Etherbone CSR access was alive immediately after load: `ctrl_scratch` read back `0x12345678`.
- The crossover UART stayed silent: no BIOS banner and no response to `help`.
- With the CPU reset bit asserted through `ctrl_reset=0x00000002`, an external Etherbone read from `0x00000000` returned `0x00000000` instead of `0x0b00006f`.
- After that ROM read, `ctrl_scratch` read back `0x00000000` and `litex_server` reported UDP read timeouts.

This rules out the single `16384x32` inferred ROM as the sole trigger for the first-word failure. Address zero is served by an `8192x32` low bank whose initialization file is correct, but the failing bus-visible behavior is unchanged. The next checks should focus on the 64 KiB ROM region decode/crossbar interaction or on adding visibility to the ROM arbiter and other masters.

## SI5351 Crossbar Address Decode

Static analysis of the generated RTL found that the SI5351 I2C sequencer was issuing Wishbone byte addresses without the main CSR bus base:

- The sequencer source used `i2c_base = self.csr.address_map("si5351", origin=True)`, which returns the CSR-bank offset `0xa000`, not the external Wishbone address `0xf000a000`.
- The generated RTL consequently drove addresses such as `si5351_bus_adr <= 16'd40980`, i.e. `0x0000a014`.
- The byte-to-word adapter then generated `adapted_interface_adr = si5351_bus_adr[31:2] = 0x00002805`.
- In the 64 KiB ROM build, the SI5351 master decoder checks `adapted_interface_adr[29:14] == 0`, so `0x2805` selects the ROM slave instead of CSR.
- In the 32 KiB ROM build, the ROM decoder checks one more address bit, `adapted_interface_adr[29:13] == 0`, so the same bad address falls outside the ROM window. This matches the observed failure boundary.

This also matches the LiteScope CPU-reset-release capture: the ROM slave was repeatedly acknowledging reads at low word address `0x2805` with zero data. That address is not a CPU reset fetch; it is the low word address of the SI5351 status CSR when the CSR base bits are missing.

The ROM arbiter only moves away from the current grant once that request drops. A persistent SI5351 request to an unintended ROM address can therefore explain why CPU and Etherbone ROM accesses see stale/zero data or become starved when the ROM region grows large enough to decode the bad address.

The target now passes the full Wishbone CSR address to the SI5351 sequencer:

```python
si5351_i2c_base = self.mem_map["csr"] + self.csr.address_map("si5351", origin=True)
```

Validation with a rebuilt 100 MHz, 64 KiB ROM image confirmed the generated RTL now emits `32'd4026572820` (`0xf000a014`) for the SI5351 status CSR, so the SI5351 master selects the CSR slave and no longer the ROM slave. The post-route timing report met constraints with WNS `+0.474 ns` and WHS `+0.060 ns`.

Hardware result after loading the fixed image:

- `ctrl_scratch` read/write/read stayed at `0x12345678`.
- An external Etherbone read from ROM address `0x00000000` returned `0x0b00006f`.
- A subsequent `ctrl_scratch` read still returned `0x12345678`, so the ROM read no longer wedges Etherbone/CSR access.
- `litex_term --csr-csv scripts/csr.csv crossover` showed the LiteX BIOS banner, `ROM: 64.0KiB`, CRC passed, and reached the `litex>` prompt.
