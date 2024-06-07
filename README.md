                            __   _ __      _  __    __  ______  _______  ___
                           / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                          / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                         /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                                  LiteX based M2 SDR FPGA board.
                                Copyright (c) 2024 Enjoy-Digital.

> [!WARNING]
>
> **LiteX-M2SDR** is still in the lab, engineering new features. 🧪 Expect things to change or break, but feel free to contribute! Hardware will be available on the webshop soon.

[> Intro
--------

We know what you'll first ask when discovering this new SDR project: what's the RFIC? 🤔 Let's answer straight away: Another **AD936X**-based SDR! 😄

Why yet another SDR based on this RFIC? Because we've been designing FPGA-based projects for clients with this chip for almost 10 years now and still think this RFIC has incredible capabilities and possibilities that haven't been fully tapped by open-source projects. We believe it can provide a fantastic and simple solution when paired with the [LiteX](https://github.com/enjoy-digital/litex) framework we're developing. 🚀

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/1a3f2d76-b406-4928-b3ed-2767d317757e" width="80%">
</div>

Imagine a minimalist AD936X-based SDR with:
- A compact form factor (M2 2280). 📏
- PCIe Gen 2 X4 (~14Gbps of TX/RX bandwidth) with [LitePCIe](https://github.com/enjoy-digital/litepcie), providing MMAP and several possible DMAs (for direct I/Q samples transfer or processed I/Q samples). ⚡
- A large XC7A200T FPGA where the base infrastructure only uses a fraction of the available resources, allowing you to integrate large RF processing blocks. 💪
- The option to reuse some of the PCIe lanes of the M2 connector for 1Gbps or 2.5Gbps Ethernet through [LiteEth](https://github.com/enjoy-digital/liteeth). 🌐
- Or ... for SATA through [LiteSATA](https://github.com/enjoy-digital/litesata). 💾
- Or ... for inter-board SerDes-based communication through [LiteICLink](https://github.com/enjoy-digital/liteiclink). 🔗
- Powerful debug capabilities through LiteX [Host <-> FPGA bridges](https://github.com/enjoy-digital/litex/wiki/Use-Host-Bridge-to-control-debug-a-SoC) and [LiteScope](https://github.com/enjoy-digital/litescope) logic analyzer. 🛠️
- ...and we hope a welcoming/friendly community as we strive to encourage in LiteX! 🤗

OK, you probably also realized this project is a showcase for LiteX capabilities, haha. 😅 Rest assured, we'll do our best to gather and implement your requests to make this SDR as flexible and versatile as possible!

This board is proudly developed in France 🇫🇷 with [Enjoy-Digital](http://enjoy-digital.fr/) managing the project and developing the gateware/software and our partner [Lambdaconcept](https://lambdaconcept.com/) designing the hardware. 🥖🍷

Perfect for SDR enthusiasts, this versatile board fits directly in an M2 slot or can team up with others in a PCIe M2 carrier for more complex SDR projects like coherent MIMO SDRs. 🔧

Mount it on the LiteX Acorn Mini Baseboard for Ethernet support with 1000BaseX/2500BaseX and SATA connectivity to directly record/play samples to/from an SSD! 💽

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/6ad09754-7aaf-4257-ba12-afbd93ebe75d" width="80%">
</div>

Dive in and start enjoying your SDR projects like never before! 🎉

[> PCIe SoC Architecture
------------------------

The PCIe SoC is the first SoC variant developed for the board and does not require an additional baseboard. Just pop the M2SDR into a PCIe M2 slot, connect your antennas, and you're ready to go! 🚀

The SoC has the following architecture:

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/fa688df9-071e-40b9-b846-c0532f5e95eb" width="80%">
</div>

- The SoC is built with the LiteX framework, allowing highly efficient HDL coding and integration. 💡
- You'll also find that most of the complexity is managed by LiteX and LitePCIe. The SoC itself only has an MMAP interface, DMA interface, and integrates the specific SDR/RFIC cores and features. ⚙️
- It provides debugging over PCIe or JTAG for MMAP peek & poke or LiteScope. 🛠️
- LitePCIe and its Linux driver (sorry, we only provide Linux support for now 😅) have been battle-tested on several commercial projects. 🏆


[> Getting started
------------------
#### [> Installing LiteX:

LiteX can be installed by following the installation instructions from the LiteX Wiki: https://github.com/enjoy-digital/litex/wiki/Installation

#### [> Installing the RISC-V toolchain for the Soft-CPU:

To get and install a RISC-V toolchain, please install it manually of follow the
LiteX's wiki: https://github.com/enjoy-digital/litex/wiki/Installation:
```bash
./litex_setup.py --gcc=riscv
```

#### [> Clone repository:

```bash
git clone --recursive https://github.com/enjoy-digital/litex_m2sdr
```

#### [> Software Prerequisites

```bash
apt install gnuradio gnuradio-dev soapysdr-tools libsoapysdr0.8 libsoapysdr-dev libgnuradio-soapy3.10.1 gqrx
```

[> Ethernet Tests
-----------------

Board mounted in Acorn Mini Baseboard:

```bash
./litex_m2sdr.py --with-ethernet --ethernet-sfp=0 --build --load
./litex_m2sdr.py --with-ethernet --ethernet-sfp=0 --build --load
ping 192.168.1.50
```

[> PCIe Tests
-------------

Board mounted in Acorn Mini Baseboard:

```bash
./litex_m2sdr.py --with-pcie --variant=baseboard --pcie-lanes=1 --build --load
lspci
```

Board mounted in directly in M2 slot:

```bash
./litex_m2sdr.py --with-pcie --variant=m2 --pcie-lanes=N_LANES --build --load
lspci
```

where `N_LANES` may be 1, 2, 4 for `m2` variant or 1 for `baseboard`

[> Software Tests
------------------

Kernel
```bash
cd software/kernel
make clean all
sudo ./init.sh
```

User
```bash
cd software/user
make clean all
./m2sdr_util info
./m2sdr_rf init -samplerate=30720000
./tone_gen.py tone_tx.bin
./m2sdr_play tone_tx.bin 100000
```

[> SoapySDR detection/probe
--------------------------------

```bash
SoapySDRUtil --probe="driver=LiteXM2SDR"
######################################################
##     Soapy SDR -- the SDR abstraction library     ##
######################################################

Probe device driver=LiteXM2SDR
[INFO] SoapyLiteXM2SDR initializing...
[INFO] Opened devnode /dev/m2sdr0, serial 8550c7af9e854
ad9361_init : AD936x Rev 2 successfully initialized
[INFO] SoapyLiteXM2SDR initialization complete

----------------------------------------------------
-- Device identification
----------------------------------------------------
  driver=LiteX-M2SDR
  hardware=R01

----------------------------------------------------
-- Peripheral summary
----------------------------------------------------
  Channels: 2 Rx, 2 Tx
  Timestamps: NO
  Sensors: fpga_temp, fpga_vccint, fpga_vccaux, fpga_vccbram, ad9361_temp
     * fpga_temp: 63.120428 °C
        FPGA temperature
     * fpga_vccint: 1.002686 V
        FPGA internal supply voltage
     * fpga_vccaux: 1.787842 V
        FPGA auxiliary supply voltage
     * fpga_vccbram: 1.002686 V
        FPGA block RAM supply voltage
     * ad9361_temp: 32 °C
        AD9361 temperature

----------------------------------------------------
-- RX Channel 0
----------------------------------------------------
  Full-duplex: YES
  Supports AGC: YES
  Stream formats: CF32
  Native format: CF32 [full-scale=1]
  Antennas: A_BALANCED
  Full gain range: [0, 73] dB
    PGA gain range: [0, 73] dB
  Full freq range: [70, 6000] MHz
    RF freq range: [70, 6000] MHz
  Sample rates: [0.260417, 61.44] MSps
  Filter bandwidths: [0.2, 56] MHz

----------------------------------------------------
-- RX Channel 1
----------------------------------------------------
  Full-duplex: YES
  Supports AGC: YES
  Stream formats: CF32
  Native format: CF32 [full-scale=1]
  Antennas: A_BALANCED
  Full gain range: [0, 73] dB
    PGA gain range: [0, 73] dB
  Full freq range: [70, 6000] MHz
    RF freq range: [70, 6000] MHz
  Sample rates: [0.260417, 61.44] MSps
  Filter bandwidths: [0.2, 56] MHz

----------------------------------------------------
-- TX Channel 0
----------------------------------------------------
  Full-duplex: YES
  Supports AGC: NO
  Stream formats: CF32
  Native format: CF32 [full-scale=1]
  Antennas: A
  Full gain range: [-89, 0] dB
    PGA gain range: [-89, 0] dB
  Full freq range: [47, 6000] MHz
    RF freq range: [47, 6000] MHz
  Sample rates: [0.260417, 61.44] MSps
  Filter bandwidths: [0.2, 56] MHz

----------------------------------------------------
-- TX Channel 1
----------------------------------------------------
  Full-duplex: YES
  Supports AGC: NO
  Stream formats: CF32
  Native format: CF32 [full-scale=1]
  Antennas: A
  Full gain range: [-89, 0] dB
    PGA gain range: [-89, 0] dB
  Full freq range: [47, 6000] MHz
    RF freq range: [47, 6000] MHz
  Sample rates: [0.260417, 61.44] MSps
  Filter bandwidths: [0.2, 56] MHz

[INFO] Power down and cleanup
```

[> GNU Radio FM Test
--------------------
```bash
gnuradio gnuradio-companion ../gnuradio/test_fm_rx.grc
```

[> Enable Debug in Kernel
-------------------------

```bash
sudo sh -c "echo 'module litepcie +p' > /sys/kernel/debug/dynamic_debug/control"
```

[> Use JTAGBone
---------------

```bash
litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg
litex_cli --regs
litescope_cli
./test_clks.py
```

[> Flash board over PCIe
------------------------
```bash
cd software
./flash.py ../build/litex_m2sdr_platform/gateware/litex_m2sdr_platform.bin
```


[> Reboot or Rescan PCIe Bus
----------------------------
```bash
echo 1 | sudo tee /sys/bus/pci/devices/0000\:0X\:00.0/remove (replace X with actual value)
echo 1 | sudo tee /sys/bus/pci/rescan
```

[> Contact
----------
E-mail: florent@enjoy-digital.fr

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/0034fac5-d760-47ed-b93a-6ceaae47e978" width="50%">
</div>

