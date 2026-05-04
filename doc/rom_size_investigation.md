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
