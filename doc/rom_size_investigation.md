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

Useful next checks:

- Sweep sizes just above the boundary, such as `0x9000`, `0xc000`, and `0x10000`, to see whether the issue is tied to inferred BRAM depth, power-of-two decode size, or a specific implementation layout.
- Use the ROM bus LiteScope probe to compare a known-good `0x8000` ROM read with a failing `0x10000` ROM read.
- Try a deliberately banked ROM implementation made from two 32 KiB memories selected by address bit 13. If that boots, the issue is probably in the single `16384x32` inferred memory implementation or its physical placement/timing.
- Try adding one explicit extra read latency cycle to the integrated ROM path. The larger BRAM tree may be returning invalid data when acknowledged with the current one-cycle SRAM wrapper.
- Build a reduced SoC with only CPU, integrated ROM/SRAM, crossover UART, and Etherbone to remove unrelated placement pressure before debugging the full design.
