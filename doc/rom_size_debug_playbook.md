# Integrated ROM Debug Playbook

This is the reusable debug flow from the integrated ROM size investigation. It is intended for future hardware bring-up agents that need to separate CPU, ROM contents, crossbar decode, timing, and external bus effects.

## Working Setup

Use a dedicated worktree/branch for hardware experiments and keep generated build products out of commits. The tested board path was:

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

Run the UDP bridge in a separate terminal/session and stop it when done:

```sh
litex_server --udp --udp-ip 192.168.1.50
```

## Minimum Hardware Checks

First prove that Etherbone CSR access works before touching ROM:

```python
from litex import RemoteClient
wb = RemoteClient(csr_csv="scripts/csr.csv")
wb.open()
print(hex(wb.regs.ctrl_scratch.read()))
wb.regs.ctrl_scratch.write(0x12345678)
print(hex(wb.regs.ctrl_scratch.read()))
wb.close()
```

Then read only one ROM word and immediately verify CSR access again:

```python
from litex import RemoteClient
wb = RemoteClient(csr_csv="scripts/csr.csv")
wb.open()
print("scratch_before", hex(wb.regs.ctrl_scratch.read()))
print("rom0", hex(wb.read(0x00000000)))
print("scratch_after", hex(wb.regs.ctrl_scratch.read()))
wb.close()
```

Expected first BIOS word is `0x0b00006f`, matching the ROM init file word `0b00006f`.

Check the crossover BIOS console with:

```sh
timeout 8s litex_term --csr-csv scripts/csr.csv crossover
```

A good image prints the LiteX banner, reports the ROM size, passes BIOS CRC, and reaches `litex>`.

## Isolation Sequence

Use this order before adding new probes:

1. Compare a known-good `0x8000` ROM with the failing larger size. Confirm the same BIOS image words are present in generated `.init` files.
2. Read ROM through Etherbone with the CPU held in reset via `ctrl_reset=0x00000002`. If the failure persists, it is not only CPU instruction fetch.
3. Lower `--sys-clk-freq` to 100 MHz and require post-route timing to pass. If the behavior persists with positive WNS/WHS, prioritize decode/arbitration over timing.
4. Try another CPU, such as `--cpu-type=serv`, keeping the ROM size and bus topology comparable. If it fails the same way, the bug is not VexRiscv-specific.
5. Try a banked 2 x 32 KiB ROM behind one 64 KiB public region. If it still fails, the single `16384x32` ROM inference is not the sole trigger.
6. Only then add LiteScope probes. Keep capture width below about 128 bits where possible; the ROM probe used 114 bits and was reliable to build.

## Verilog Checks

Inspect generated Verilog before instrumenting. Useful patterns:

```sh
rg -n "assign adapted_interface_adr|decoder[0-9]_master|si5351_bus_adr <=|Memory rom:" build -g "*.v"
```

For each main Wishbone master, confirm:

- its byte/word address adaptation
- which decoder slot selects ROM/SRAM/CSR
- which interface feeds the ROM arbiter
- whether the arbiter grant can stay on a master while its request remains asserted
- the local ROM address width and memory depth

The bug fixed here was visible statically:

- SI5351 was a main Wishbone master.
- Its sequencer drove `0x0000a014`, missing `CSR_BASE`.
- The Wishbone byte-to-word adapter produced word address `0x2805`.
- A 64 KiB ROM decodes word address `0x2805` as ROM, while a 32 KiB ROM does not.
- LiteScope later showed the ROM slave repeatedly acknowledging reads at `0x2805`, matching the bad SI5351 status address.

After the fix, the generated RTL should show SI5351 addresses in the `0xf000a000` CSR window, for example `0xf000a014`, and the SI5351 decoder should select CSR rather than ROM.

## LiteScope Probe Guidance

Use LiteScope after static decode checks or when the external read path cannot explain the behavior. Keep the capture narrow:

- `ctrl.cpu_rst`, CPU reset, sys reset
- Etherbone ROM access flag plus low address/data/control
- ROM slave low address/data/control
- optional suspect master low address/control
- optional arbiter grant if exposed

Avoid wide buses unless essential. A practical capture under 128 bits is preferred; up to 256 bits may work but can fail build/resource/timing more often.

For ROM read capture:

```sh
python3 -m litescope.software.litescope_cli \
    --csv test/analyzer.csv --csr-csv scripts/csr.csv \
    --rising-edge basesoc_rom_probe_eth_rom_access \
    --dump rom_read.vcd
```

If an external ROM read wedges Etherbone, arm on CPU reset release or another internal trigger so the capture can be drained before issuing the destructive external access.

## Commit Hygiene

Keep three kinds of commits separate when possible:

- diagnostic infrastructure, such as CPU selection, banked ROM, or probes
- measured results/documentation
- the functional fix

Do not commit generated timestamp-only header updates from build/load unless they are intentionally part of the change.
