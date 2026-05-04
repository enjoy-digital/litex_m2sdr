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
