```{=latex}
\frontmatter
```

# About this document {.unnumbered}

This manual is the reference of the LiteX-M2SDR project: a software-defined radio board in the
M.2 form factor, built around an AD9361 RFIC and an Artix-7 FPGA, with its gateware and its host
software. It is written for the people who use the board and for the FPGA and software engineers
who extend it.

No prior knowledge of radio, of LiteX or of time distribution is assumed. Part I explains the
approach of the project, then gives three short primers: software-defined radio and the AD9361,
the LiteX infrastructure the gateware reuses, and the role of time in a radio.

Part II describes the board and what it plugs into. Part III describes the gateware: the SoC,
the RFIC interface, the streaming path, the host links and the timing mechanisms.

Part IV is about software and use. It opens with the first steps, which take a new user from the
box to the first samples, then covers the host software, the SDR frameworks, the gateware build,
the task recipes and the debugging. The appendices hold the register reference, the build
options, the glossary and the measured results.

| If you want to | Read |
|------------------------------------------|----------------------------------------------|
| Know what the project is and why | [Introduction](#intro) |
| Know what works today | [Status](#intro-status) |
| Learn the radio vocabulary | [SDR in a nutshell](#sdr-primer) |
| Understand the LiteX cores behind the board | [LiteX in a nutshell](#litex-primer) |
| Understand 10 MHz, PPS, PTM, PTP and White Rabbit | [Time in a nutshell](#timing-primer) |
| Know the board, its pins and its clocks | [Hardware](#hardware) |
| Choose where to plug the board | [Carriers and hosts](#carriers) |
| Receive the first samples | [Getting started](#getting-started) |
| Know the library and the utilities | [Host software](#software) |
| Use GNU Radio, GQRX or another application | [SDR frameworks](#frameworks) |
| Build or modify the gateware | [Building the gateware](#building-gateware) |
| Follow a recipe for a given task | [Workflows](#workflows) |
| Understand a board that does not answer | [Debugging](#debugging) |

The manual follows the repository: every name, number and default is taken from the source code
and from the documents of the repository, and the manual is generated from them on the date
printed on the title page. When the code and the manual disagree, the code is right and the
manual has a bug.

## Conventions {.unnumbered}

- Numbers: `0x` prefix for hexadecimal, `0b` for binary.
- Rates: sample rates in MSPS (10^6 samples per second), line rates in Gbps, throughputs in MB/s
  (10^6 bytes per second) unless a tool prints another unit. Sizes in KiB and MiB.
- *The board* is the LiteX-M2SDR module; *the project* is the repository. *The RFIC* is the
  AD9361; *the FPGA* is the XC7A200T; *the clock generator* is the SI5351.
- *The host* is the computer that controls the board. A *host link* carries the control and the
  samples between the board and the host: PCIe, Ethernet or USB3.
- A *carrier* is what the board is plugged into. *The baseboard* is the LiteX Acorn Baseboard
  Mini.
- RX and TX are seen from the antenna: RX is what the board receives from the air.
- `<variant>` stands for a value of the `--variant` build option: `m2` or `baseboard`.
- `<device>` stands for a device identifier of the utilities, such as `pcie:/dev/m2sdr0` or
  `eth:192.168.1.50:1234`.
- `<release>` stands for a release tag of the repository and `<image>` for the name of a release
  image.
- A feature flagged *in development* is not part of a release, and a feature flagged
  *Experimental* runs in a restricted setup; the [status table](#intro-status) defines each
  status word.
- Paths are relative to the repository root. Shell commands are given for a Linux host.
- When the text names a source file, it is the file to open to check or extend the fact.
