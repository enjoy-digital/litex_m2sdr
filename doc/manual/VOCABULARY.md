# Vocabulary of the LiteX-M2SDR manual

Project-specific writing rules, completing the generic `STYLE.md` of the manual kit.

## Names of the parts

- *LiteX-M2SDR* (with the hyphen, never "M2SDR" alone or "LiteX M2 SDR") is both the board and
  the project. *The board* is the M.2 module; *the project* is the repository: gateware, software
  and documentation. The hardware design is not public: never write "open hardware",
  "open-source hardware" or "fully open source". Gateware and software are open source
  (BSD-2-Clause).
- *the RFIC* is the AD9361 (write "AD9361", not "AD936x", except when citing the vendor family).
  *the FPGA* is the XC7A200T. *the clock generator* is the SI5351.
- *gateware* is the FPGA design (`litex_m2sdr.py`, `litex_m2sdr/gateware/`); *software* is
  everything that runs on the host (`litex_m2sdr/software/`): *the kernel driver*, *the library*
  (`libm2sdr`), *the utilities* (`m2sdr_util`, `m2sdr_rf` and the others), *the SoapySDR driver*.
  "Firmware" is used only for the White Rabbit core software, and for the boot firmware of a
  single-board computer (the Raspberry Pi firmware and its `/boot/firmware` directory). The
  setup program of a PC is "the BIOS setup".
- *the host* is the computer that controls the board and exchanges samples with it.
- *host link* is the link that carries control and samples between the board and the host: PCIe,
  Ethernet or USB3. SATA is not a host link: the board is the SATA host of an SSD. Write
  "SATA storage" or "the SATA link".
- *carrier* is whatever the board is plugged into: the M.2 slot of a PC or of a single-board
  computer, an M.2 carrier card, or the baseboard. *the baseboard* is the LiteX Acorn Baseboard
  Mini (full name once per chapter, then "the baseboard").
- *variant* is the value of the `--variant` build option, `m2` or `baseboard`, and nothing else.
  The two clock generator fittings are *the SI5351C version* and *the SI5351B version* of the
  board.
- *bitstream* is the FPGA file; *release image* is a prebuilt bitstream published on the
  Releases page.
- *host bridge* is a LiteX bridge that gives the host access to the registers of the SoC: over
  PCIe (the kernel driver), over Ethernet (Etherbone), over USB3 (endpoint 0) or over JTAG
  (JTAGBone).
- *register* in prose (never "CSR", except for "CSR bus" and the file `csr.h`).
- *stream* is a LiteX stream (valid, ready, data); *the RX stream* goes from the RFIC to the
  host, *the TX stream* from the host to the RFIC. RX and TX are always seen from the antenna:
  RX is reception from the air.
- *sample* is one complex I/Q value of one channel; *I/Q* with a slash. *channel* is one RF
  path of the RFIC (RX1, RX2, TX1, TX2). *2T2R* and *1T1R* for the channel configurations;
  "MIMO" only for multi-antenna uses.
- *sample rate* in MSPS (`61.44 MSPS`, `122.88 MSPS`); *bandwidth* (in MHz) is the analog or RF
  bandwidth, never a synonym of the sample rate.
- *oversampling mode* is the 122.88 MSPS mode of the `--with-rfic-oversampling` build. It is
  Experimental: flag it at its first mention in a chapter, and never put it in a summary, an
  opening or a list of headline features (the headline rate is 2T2R, 12-bit, up to
  61.44 MSPS).
- *board time* is the 64-bit nanosecond counter of the gateware (`TimeGenerator`); *timestamp*
  is a board time value attached to a buffer of samples; *timed TX* (or "hardware timed TX" in a
  title) is the transmission of a buffer at its timestamp.
- *reference clock* is the clock the RFIC and the board time derive from; *10 MHz reference*
  and *PPS* (pulse per second, in capitals) are the two external timing inputs. The PPS input
  is monitored only: never write that the board time uses it, and never mention a PPS output
  except to say that there is none.
- *the crystal of the clock generator* is the 25 MHz crystal of the SI5351; *the 100 MHz
  oscillator* is the oscillator of the FPGA. Never "on-board oscillator" or "local oscillator"
  for either of them: *local oscillator* (LO) is the carrier synthesizer of the RFIC.
- *time synchronisation* in British spelling in prose ("synchronisation", "synchronise");
  American spelling stays in code identifiers.
- *discipline* (verb and noun) is the slow correction of a clock or of the board time by a
  reference: "the PTP discipline", "the board time is disciplined by PTP".

## Status words

Exactly five, defined once under the status table of the introduction (`#intro-status`); other
chapters link to it and never restate a status:

- *Validated on hardware*, *Experimental*, *In development*, *Build-tested*, *Simulation-tested*.
- The oversampling mode and White Rabbit are always flagged "Experimental" with a link to the
  status table. The USB3 host link is Validated on hardware: no flag, and no measured rate
  without a record in the repository.
- Link texts are the titles of the chapters: "The SoC", "The RFIC interface", "Host software",
  "Time synchronisation", "Carriers and hosts".

## Protocol names

- PCIe: "PCIe Gen2 x1", "x2", "x4" (as the README and the tools print them); *PTM* for Precision
  Time Measurement; *MMAP* for the memory-mapped register access and *DMA* for the sample
  streaming of LitePCIe.
- Ethernet PHYs: *1000BASE-X*, *2500BASE-X*, *5GBASE-R* in prose; `1000basex`, `2500basex`,
  `5000baser` are the values of `--eth-phy`. Rates: 1 Gbps, 2.5 Gbps, 5 Gbps.
- *PTP* is IEEE 1588 as LiteEth and linuxptp implement it; *White Rabbit* in two words,
  capitalised, "WR" only in identifiers.
- *USB3* (no space) for the host link and its options; *USB3-PIPE* for the LiteX core; cite the
  standard as "USB 3.2". *the USB3 helper* is `libliteusb`; "LiteUSB" only in identifiers.
  Endpoints as "endpoint 0", "Bulk IN", "Bulk OUT"; "EP0" only in identifiers and figures.
- *SATA*, *SFP*, *M.2* (with the dot), "M.2 2280 Key M".
- Sample formats: *SC16* and *SC8* (transport formats), *VRT* for VITA 49 packets, *SigMF* for
  the recording format.
- LiteX cores with their exact names: LiteX, LitePCIe, LiteEth, LiteSATA, LiteICLink, LiteScope,
  LiteI2C, LiteDSP, USB3-PIPE.
- Vendor documents are cited by title ("AD9361 Reference Manual, UG-570"), never by page.

## Placeholders

- `<variant>` is `m2` or `baseboard`.
- `<device>` is a device identifier of the utilities, such as `pcie:/dev/m2sdr0`,
  `eth:192.168.1.50:1234` or `usb:`.
- `<release>` is a release tag of the repository; `<image>` is a release image name.
- All are defined once in the Conventions of the front matter.

## Hardware names

- Board: LiteX-M2SDR. Baseboard: LiteX Acorn Baseboard Mini. M.2 carrier card for several
  boards: ASUS Hyper M.2 (a bifurcated carrier card).
- Chips: AD9361 (Analog Devices), XC7A200T (AMD/Xilinx Artix-7; "Artix-7" for the family),
  SI5351B and SI5351C (Skyworks, formerly Silicon Labs).
- Hosts: PC, Raspberry Pi 5, Orange Pi 5 Max, NVIDIA Jetson Orin. M.2 HAT of the Raspberry Pi
  guide: Pineboards HatDrive BM1L.
- SFP modules used for the validation: LianGuo LG 2.5GE (2500BASE-X), H!Fiber ASF-10G-T80
  (5GBASE-R). PHY families of multi-rate copper modules: Broadcom BCM84891L, Marvell Alaska
  88X3310 and 88E2110, Marvell AQR113C (formerly Aquantia). SFP to USB3 adapter: Xillybus
  SFP2USB.
- SSDs of the SATA measurements: the models named in `D_performance.md`.
- Transceiver: *transceiver* (or GTP for the Artix-7 block); "SerDes" only in identifiers and
  when quoting an option. *QPLL* is the shared PLL of the GTP quad (`SharedQPLL`).
- Connectors: SFP cage (`sfp0`, `sfp1` in options), uFL connector.
- Companies: only Enjoy-Digital, LambdaConcept (once, in the introduction), and the chip, board,
  host, module and adapter vendors above (Analog Devices, AMD/Xilinx, Skyworks/Silicon Labs,
  ASUS, Raspberry Pi, Orange Pi, NVIDIA, Pineboards, LianGuo, H!Fiber, Samsung, WDC, Broadcom, Marvell,
  Aquantia, Xillybus, and the SSD vendors of the measurements). Open-source projects by their
  names: SoapySDR, GNU Radio, GQRX, srsRAN, SigMF, White Rabbit, linuxptp, `ubuntu-rockchip`.
