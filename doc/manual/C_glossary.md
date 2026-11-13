# Glossary {#glossary}

The terms below are used throughout the manual. The three primers explain them in context:
[SDR in a nutshell](#sdr-primer), [LiteX in a nutshell](#litex-primer) and
[Time in a nutshell](#timing-primer).

| Term | Meaning |
|----------------------|-------------------------------------------------------------------------|
| 1T1R, 2T2R | Channel configuration: one or two transmitters and receivers in use |
| ADC, DAC | Analog to digital and digital to analog converters; 12 bits in the AD9361 |
| AGC | Automatic gain control: the RFIC adjusts its receive gain to the signal level |
| Aliasing | Folding of signals from outside the sampled band into it |
| Analog bandwidth | Width of spectrum the analog filters of the RFIC let through |
| Baseband | The signal moved from its carrier down to 0 Hz |
| Baseboard | The LiteX Acorn Baseboard Mini: a carrier with SFP cages and a SATA connector |
| BBPLL | Baseband PLL of the AD9361: source of its converter and filter clocks |
| BFP8 | Block floating point transport format: 8-bit mantissas and one exponent per block |
| Bitstream | The configuration file of the FPGA |
| Board time | The 64-bit nanosecond counter of the gateware that stamps the samples |
| Carrier | What the board is plugged into: an M.2 slot, a carrier card or the baseboard |
| Carrier frequency | Centre frequency of a received or transmitted signal |
| Channel | One RF path of the RFIC: RX1, RX2, TX1 or TX2 |
| Crossbar | Stream switch that connects the SDR datapath to the selected link |
| CSR | Control and Status Register of a LiteX design, on the CSR bus |
| DATA_CLK, FB_CLK | Clocks of the data port of the AD9361: receive clock and its returned copy |
| DDR | Double data rate: data changes on both edges of the clock |
| Direct conversion | Radio architecture that mixes between carrier and baseband in one step |
| Discipline | Slow correction of a clock or of a time by a reference |
| DMA | Direct memory access: LitePCIe moves streams to and from host memory |
| Etherbone | Host bridge over UDP: access to the SoC bus through Ethernet |
| FDD, TDD | Frequency division and time division duplex |
| FIR | Finite impulse response filter; programmable, 128 taps, in the AD9361 |
| Gateware | The FPGA design |
| Grandmaster | The clock a PTP network follows |
| GTP | Transceiver of the Artix-7 |
| Half-band | Low-pass filter built to halve or double a sample rate |
| Holdover | State of a disciplined clock that has lost its reference |
| Host | The computer that controls the board |
| Host bridge | Link that makes the host a master of the SoC bus: PCIe, Etherbone, JTAGBone |
| Host link | Link that carries control and samples: PCIe, Ethernet or USB3 |
| I/Q | In-phase and quadrature: the two parts of a complex baseband sample |
| IOMMU | Unit of the host that translates the memory addresses a device uses for DMA |
| JTAG | Test and configuration port of the FPGA, also used by a host bridge |
| JTAGBone | Host bridge over the JTAG cable |
| LiteX, Migen | Python frameworks in which the gateware is written |
| LNA | Low-noise amplifier, first stage of a receiver |
| LO | Local oscillator: the carrier frequency generated inside the RFIC |
| LVDS | Low-voltage differential signalling, used by the data port of the AD9361 |
| M.2 | Form factor of the board: M.2 2280, Key M |
| MMAP | Memory-mapped access to the registers through PCIe |
| MSPS | Million samples per second |
| MSI | Message signalled interrupt: the interrupt of a PCIe device |
| Multiboot | Two bitstreams in the flash: a fallback and an operational one |
| Oversampling mode | The 122.88 MSPS mode of the `--with-rfic-oversampling` build; Experimental |
| PCIe | PCI Express: the serial bus of an M.2 Key M slot, and the main host link |
| PHC | PTP hardware clock: a device clock exposed by Linux to PTP tools |
| PHY | Physical layer of a link; also the gateware that captures the RFIC data port |
| PoE | Power over Ethernet: power carried by the Ethernet cable |
| ppm | Part per million: unit of frequency error |
| PPS | Pulse per second: a pulse on each second boundary; the board monitors a PPS input |
| PRBS | Pseudo-random binary sequence, used to test the RFIC data port |
| PTM | Precision Time Measurement: time transfer over a PCIe link |
| PTP | Precision Time Protocol, IEEE 1588: time transfer over a network |
| QPLL | Shared PLL of a transceiver quad |
| Quad | Group of four transceivers and their two PLLs |
| Reference clock | The clock from which the sample clock, the LO and the board time derive |
| Release image | A prebuilt bitstream published on the Releases page |
| RFIC | Radio frequency integrated circuit: the AD9361 |
| RX, TX | Reception from the air, transmission to the air |
| Sample | One complex I/Q value of one channel |
| Sample rate | Number of complex samples per second and per channel |
| SATA | Serial ATA: the bus of disks and SSDs; the board is the host of an SSD |
| SC16, SC8 | Transport formats: complex samples of two 16-bit or two 8-bit signed values |
| SDR | Software-defined radio |
| SFP | Small form-factor pluggable: the module cage of the baseboard |
| SigMF | Signal Metadata Format: an open format to describe recordings |
| SMBus | Two-wire bus of the M.2 connector, close to I2C |
| SoapySDR | Open-source library that gives SDR applications one device interface |
| SoC | System on chip: the LiteX design with its bus, registers and cores |
| SPI | Serial bus used to configure the AD9361 |
| Stream | LiteX data channel with a valid and ready handshake |
| Syntonised | Two clocks with the same frequency |
| Synchronised | Two clocks with the same frequency and the same time |
| Timed TX | Transmission of a buffer at the board time written in its timestamp |
| Timestamp | A board time value attached to a buffer of samples |
| Transceiver | High-speed serial block of the FPGA |
| uFL | Miniature coaxial connector, used for the RF ports and the 10 MHz reference |
| Variant | Value of the `--variant` build option: `m2` or `baseboard` |
| VCXO | Voltage-controlled crystal oscillator: a crystal oscillator with a tuning input |
| VRT | VITA 49 Radio Transport: a standard packet format for samples |
| White Rabbit | Extension of PTP for sub-nanosecond synchronisation over fibre |
| XO | Crystal oscillator; the board has a 100 MHz one for the FPGA |
| Zero-IF | Another name of direct conversion |
