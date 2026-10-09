# Hardware Reference

Board-level reference for LiteX-M2SDR: M.2 keying, I/O voltage levels, sideband signal routing,
PPS input selection and the user LED. For the AD9361 and SI5351 datasheets, see
[`doc/ad9361/`](ad9361/) and [`doc/si5351/`](si5351/).

## M.2 Keying / GPIO Voltage Levels

LiteX-M2SDR is an **M.2 2280 Key M** module. Use it with M-keyed PCIe M.2 slots, carriers, or compatible adapters.

LiteX-M2SDR does **not** use a single M.2 I/O voltage:
- FPGA banks **13/14/15/16** on the SDR are powered at **3.3V**.
- FPGA banks **34/35** on the SDR are powered at **1.8V**.
- The general-purpose sideband signals routed directly from the M.2 connector to the FPGA on LiteX-M2SDR (`PPS`, `Synchro_GPIO`, `PERST#`, optional `PEWAKE#`, `SUSCLK`, `PEDET`) sit on **3.3V FPGA banks on the SDR side**.
- The M.2 `SMB_CLK` / `SMB_DATA` pins are a special case: on LiteX-M2SDR r02 they reach the FPGA bank-16 pins through optional resistors `R82` / `R83`, which are **not mounted by default**.
- PCIe lanes and the PCIe reference clock are transceiver signals, not single-ended 1.8V/3.3V GPIOs.

Additional notes:
- M.2 pin **44** (`ALERT#` / `SMB_ALERT#`) is currently **not routed to the FPGA** on LiteX-M2SDR r02.
- M.2 pin **52** (`CLKREQ#`) is pulled up to `3V3_PCIe` and is **not** routed to the FPGA.
- M.2 pin **10** (`LED#`) is **not connected** on the FPGA side.
- The dedicated FPGA JTAG/config pins and the Acorn JTAG header are separate **3.3V** JTAG paths.
- When discussing M.2 sideband voltages, distinguish the **FPGA bank voltage on the SDR** from the **connector-side voltage expected by a host/baseboard**. For example, the Acorn baseboard implements the M.2 SMBus pins as a **1.8V SMBus domain** with translation to **3.3V** for the SFP modules.

| Signal | Connector Location | FPGA Pin | Bank | Voltage On SDR Side | Notes |
|--------|--------------------|----------|------|---------------------|-------|
| `GPIO0` | `TP1` | `E22` | 16 | 3.3V | General-purpose test point (`FPGA_GPIO0`). |
| `GPIO1` | `TP2` | `D22` | 16 | 3.3V | General-purpose test point (`FPGA_GPIO1`). |
| `PPS_IN` | M.2 pin 22 (`NC22`) | `K18` | 15 | 3.3V | Routed to the FPGA. |
| `PPS_OUT` | M.2 pin 24 (`NC24`) | `Y18` | 14 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO1` | M.2 pin 28 (`NC28`) | `A19` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO2` | M.2 pin 30 (`NC30`) | `A18` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO3` | M.2 pin 32 (`NC32`) | `A21` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO4` | M.2 pin 34 (`NC34`) | `A20` | 16 | 3.3V | Routed to the FPGA. |
| `Synchro_GPIO5` | M.2 pin 36 (`NC36`) | `B20` | 16 | 3.3V | Routed to the FPGA. |
| `SMB_CLK` | M.2 pin 40 | `A13` | 16 | 3.3V FPGA bank on SDR | Optional path through `R82`, not mounted by default; connector-level SMBus compatibility depends on the host/baseboard. |
| `SMB_DATA` | M.2 pin 42 | `A14` | 16 | 3.3V FPGA bank on SDR | Optional path through `R83`, not mounted by default; connector-level SMBus compatibility depends on the host/baseboard. |
| `ALERT#` / `SMB_ALERT#` | M.2 pin 44 | - | - | Host-defined sideband | Not routed to the FPGA on LiteX-M2SDR r02. |
| `PERST#` | M.2 pin 50 | `A15` | 16 | 3.3V | Routed to the FPGA. |
| `CLKREQ#` | M.2 pin 52 | - | - | 3.3V | Pulled up to `3V3_PCIe` with `R59`; not routed to the FPGA. |
| `PEWAKE#` | M.2 pin 54 | `B16` | 16 | 3.3V | Optional path through `R88`, not mounted by default. |
| `SUSCLK` | M.2 pin 68 | `B17` | 16 | 3.3V | Routed through `R84` (0R). |
| `PEDET` / `PRESENT` | M.2 pin 69 | `A16` | 16 | 3.3V | Routed through `R85` (0R). |
| `LED#` | M.2 pin 10 | - | - | Host-defined sideband | Not connected on LiteX-M2SDR. |

## External PPS Input

The default PPS input is M.2 pin 22 (`PPS_IN`). For easier wiring, the input can instead be selected from test point TP1 or TP2 with `--pps-input=tp1` or `--pps-input=tp2`; the default is `--pps-input=m2`. For example:

```sh
./litex_m2sdr.py --pps-input=tp1 --build
```

TP1 and TP2 are general-purpose test points on FPGA pins E22 and D22. The selected input is synchronized to the system clock, with rising edges reported through the `pps_in` CSRs (`status.level`, `status.pulse`, and `count`). A TP1/TP2 PPS selection uses those pins instead of the regular GPIO feature, so it cannot be combined with `--with-gpio`. All three inputs require a ground-referenced 3.3V logic PPS signal; do not apply 5V. For M.2 pin 22, use a carrier or adapter that actually routes the otherwise NC pin.

## User LED Behavior

The board exposes a single monochrome `user_led`, so the gateware uses it as a layered status indicator rather than a simple on/off flag:

- **Not ready yet**: double-heartbeat while time is still invalid or while an enabled PCIe/Ethernet transport is not ready.
  PCIe becomes ready when the link is up and DMA/PPS synchronization is established; Ethernet becomes ready when the link is up.
- **Idle / ready state**: gentle low-amplitude breathing.
- **PPS event**: short bright accent pulse over the base animation.
- **RF or Ethernet RX/TX activity**: bright accent pulse.

When PCIe is not enabled in the build, the PCIe-specific states are naturally skipped and the LED falls back to the generic timing/activity behavior.
