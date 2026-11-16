# AGENTS.md: LiteX-M2SDR

Gateware and software for the LiteX-M2SDR board (M.2 2280, Artix-7 + AD9361 RFIC), over PCIe,
Ethernet (1G/2.5G/5G) or USB3, with SoapySDR support, time sync (PTP/PTM, White Rabbit) and the
Acorn/PoE baseboards.

## Where things are

- `litex_m2sdr.py` (SoC/target: `--variant=m2|baseboard`, `--with-pcie`, `--with-eth`, probe switches), `litex_m2sdr_platform.py`.
- `litex_m2sdr/gateware/`: board gateware (AD9361 PHY/SPI, clocking, time, headers, loopbacks...).
- `litex_m2sdr/software/`: kernel driver, `user/` (libm2sdr, `m2sdr_util`, `m2sdr_rf`, `m2sdr_record`...), SoapySDR; `./build.py`.
- `scripts/` (RemoteClient scripts), `test/` (CI-safe tests), `doc/manual/` (the manual: Markdown chapters, `build.py`).

## Verify a change

```sh
python3 -m pytest -v test                                           # what CI runs
./litex_m2sdr.py --help                                             # variants, interfaces and probe switches
./litex_m2sdr.py --variant=m2 --with-pcie --build                   # gateware build (Vivado)
make -C litex_m2sdr/software/user all                               # user-space tools
```

- Read the Debugging chapter of the manual (`doc/manual/17_debugging.md`) before hardware work: transports, loopback diagnostics, probes.
- Keep the CSR map and `csr.h` a superset of the previous ones: the host software depends on them.
- AD9361: sample-rate/filter changes need a check at several rates (FIR bypassed between 2.5 and 61.44 MSPS).
- The full test suite needs a LiteX-WR-NIC checkout at the pinned commit.

## LiteX

This project is built with LiteX: read https://github.com/enjoy-digital/litex/wiki/LiteX-for-AI-Agents (framework map, conventions,
how to verify a change, contribute back the generic fixes to LiteX and its cores).

## Debug on hardware

Method: https://github.com/enjoy-digital/litex/wiki/Debug-A-SoC-On-Hardware (check the link and the SoC identity first, registers
before traces, narrow LiteScope captures, read the generated Verilog, turn findings into tests).
`litex_mcp` gives an agent the same loop as MCP tools through `litex_server` (CSRs by name, LiteScope
captures as data, console, builds with parsed timing, simulation; writes and `--load`/`--flash`
only with `--allow-write`): https://github.com/enjoy-digital/litex/wiki/LiteX-for-AI-Agents#drive-a-soc-through-mcp.

## Rules

- LiteX style: https://github.com/enjoy-digital/litex/blob/master/doc/coding_style.md.
- Never edit generated files (`build/`, `csr.h`, `csr.csv`, generated Verilog): change the Python.
- Never flash a board or write its SPI flash unless the user asked; `--load` (volatile) for experiments.
- Never commit flash dumps, stock firmware or vendor bitstreams.
- Pull requests: one topic each, with what was tested and on which hardware; say that an agent wrote
  it and who supervised.
