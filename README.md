                            __   _ __      _  __    __  ______  _______  ___
                           / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                          / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                         /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                                  LiteX based M2 SDR FPGA board.
                                Copyright (c) 2024 Enjoy-Digital.

![License](https://img.shields.io/badge/License-BSD%202--Clause-orange.svg)

> [!WARNING]
>
> **LiteX-M2SDR** is still in the lab, engineering new features. 🧪 Expect things to change or break, but feel free to contribute! Hardware will be available on the webshop soon.

[> Intro
--------

We know what you'll first ask when discovering this new SDR project: what's the RFIC? 🤔 Let's answer straight away: Another **AD936X**-based SDR! 😄

Why yet another SDR based on this RFIC? Because we've been designing FPGA-based projects for clients with this chip for almost 10 years now and still think this RFIC has incredible capabilities and possibilities that haven't been fully tapped by open-source projects. We believe it can provide a fantastic and simple solution when paired with the [LiteX](https://github.com/enjoy-digital/litex) framework we're developing. 🚀

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/1a3f2d76-b406-4928-b3ed-2767d317757e" width="100%">
</div>

Imagine a minimalist AD9361-based SDR with:
- A compact form factor (M2 2280). 📏
- Minimal on-board RF frontend that could be specialized externally.
- 2T2R / 12-bit @ 61.44MSPS (and 2T2R / 12-bit @ 122.88MSPS for those wanting to use/explore Cellwizard/BladeRF [findings](https://www.nuand.com/2023-02-release-122-88mhz-bandwidth/)).
- PCIe Gen 2 X4 (~14Gbps of TX/RX bandwidth) with [LitePCIe](https://github.com/enjoy-digital/litepcie), providing MMAP and several possible DMAs (for direct I/Q samples transfer or processed I/Q samples). ⚡
- A large XC7A200T FPGA where the base infrastructure only uses a fraction of the available resources, allowing you to integrate large RF processing blocks. 💪
- The option to reuse some of the PCIe lanes of the M2 connector for 1Gbps or 2.5Gbps Ethernet through [LiteEth](https://github.com/enjoy-digital/liteeth). 🌐
- Or ... for SATA through [LiteSATA](https://github.com/enjoy-digital/litesata). 💾
- Or ... for inter-board SerDes-based communication through [LiteICLink](https://github.com/enjoy-digital/liteiclink). 🔗
- Powerful debug capabilities through LiteX [Host <-> FPGA bridges](https://github.com/enjoy-digital/litex/wiki/Use-Host-Bridge-to-control-debug-a-SoC) and [LiteScope](https://github.com/enjoy-digital/litescope) logic analyzer. 🛠️
- Multiboot support to allow secure remove update over PCIe (or Ethernet).
- ...and we hope a welcoming/friendly community as we strive to encourage in LiteX! 🤗

OK, you probably also realized this project is a showcase for LiteX capabilities, haha. 😅 Rest assured, we'll do our best to gather and implement your requests to make this SDR as flexible and versatile as possible!

This board is proudly developed in France 🇫🇷 by [Enjoy-Digital](http://enjoy-digital.fr/), managing the project and gateware/software development, and our partner [Lambdaconcept](https://shop.lambdaconcept.com/) designing the hardware. 🥖🍷

Ideal for SDR enthusiasts, this versatile board fits directly into an M2 slot or can team up with others in a PCIe M2 carrier for more complex projects, including coherent MIMO SDRs. 🔧

For Ethernet support with 1000BaseX/2500BaseX and SATA connectivity to directly record/play samples to/from an SSD, mount it on the LiteX Acorn Mini Baseboard! 💽

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/6ad09754-7aaf-4257-ba12-afbd93ebe75d" width="100%">
</div>

Unlock new possibilities in your SDR projects with this cutting-edge board—we'll try our best to meet your needs! 🎉

[> PCIe SoC Design
------------------

The PCIe design is the first variant developed for the board and does not require an additional baseboard. Just pop the M2SDR into a PCIe M2 slot, connect your antennas, and you're ready to go! 🚀

The SoC has the following architecture:

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/df5eb55e-16b2-4724-b4c1-28e06c45279c" width="100%">
</div>

- The SoC is built with the LiteX framework, allowing highly efficient HDL coding and integration. 💡
- You'll also find that most of the complexity is managed by LiteX and LitePCIe. The SoC itself only has an MMAP interface, DMA interface, and integrates the specific SDR/RFIC cores and features. ⚙️
- It provides debugging over PCIe or JTAG for MMAP peek & poke or LiteScope. 🛠️
- [LitePCIe](https://github.com/enjoy-digital/litepcie) and its Linux driver (sorry, we only provide Linux support for now 😅) have been battle-tested on several commercial projects. 🏆

The PCIe design has already been validated at the maximum AD9361 specified sample rate: 2T2R @ 61.44MSPS (and also seems to correctly handle the oversampling at 2T2R @ 122.88MSPS with 7.9 Gbps of bandwidth on the PCIe bus; this oversampling feature is already in place and more tests/experiments will be done with it in the future).

[> Ethernet SoC Design (1/2.5Gbps x 1 or 2).
--------------------------------------------

> [!WARNING]
>
> **WiP** 🧪 Still in the lab, all the cores required are already developped and interfaces have been validated but the SoC still need to be assembled/tested and software developped.


The Ethernet design variant will gives flexibility when deploying the SDR. The PCIe connector has 4 SerDes transceivers that are in most cases used for... PCIe :) But these are 4 classical GTP transceivers of the Artix7 FPGA that are connected to the PCIe Hardened PHY in the case of a PCIe application but that can be used for any other SerDes-based protocol: Ethernet 1000BaseX/2500BaseX, SATA, etc...

In this design, the PCIe core will then be replaced with [LiteEth](https://github.com/enjoy-digital/liteeth), providing the 1000BaseX or 2500BaseX PHY but also the UDP/IP hardware stack + Streaming/Etherbone front-end cores.

TODO: Add diagram and more info.

[> Getting Started
------------------

### For SDR Enthusiasts

If you are an SDR enthusiast looking to get started with the LiteX-M2SDR board, follow these simple steps to get up and running quickly:

1. **Connect the Board:**
   - Insert the LiteX-M2SDR board into an available M2 slot on your Linux computer and connect your antennas.

2. **Install Required Software:**
   - Ensure you have the necessary software installed on your Linux system. You can do this by running the following command in your terminal:
   ```
   sudo apt install gnuradio gnuradio-dev soapysdr-tools libsoapysdr0.8 libsoapysdr-dev libgnuradio-soapy3.10.1 gqrx
   ```

3. **Clone the Repository:**
   - Clone the LiteX-M2SDR repository using the following command:
   ```
   git clone https://github.com/enjoy-digital/litex_m2sdr
   ```

4. **Build and Install Software:**
    Software build use make and cmake for the C kernel driver and utilities, but since we also like Python 😅, we created a small script on top if it to simplify our developpment and installation:
   - Navigate to the software directory and run the build script:
   ```
   cd software
   ./build.py
   ```
   - This will build the necessary components including the kernel driver, user-space utilities, and the SoapySDR driver.

5. **Load the Kernel Driver:**
   - Load the kernel driver with the following commands:
   ```
   cd software/kernel
   sudo ./init.sh
   ```
   - 🚀 Ready for launch!

6. **Run Your SDR Software:**
   - Now, you can launch your preferred SDR software (like GQRX or GNU Radio) and select the LiteX-M2SDR board through SoapySDR. 📡

> [!WARNING]
>
> **WiP** 🧪 Content below is more our memo as developers than anything useful to read 😅. This will be reworked/integrated differently soon.

### Tutorials for your platform

For some platforms we created detailed tutorials. For everything else, please follow the earlier *Getting Started* tutorial.

- [Use LiteX-M2SDR on OrangePI 5 Max](doc/hosts/orangepi-5-max.md)

### For Software Developers

For those who want to dive deeper into development with the LiteX-M2SDR board, follow these additional steps after completing the SDR enthusiast steps:

1. **Run Software Tests:**
   - Test the kernel:
   ```
   cd software/kernel
   make clean all
   sudo ./init.sh
   ```
   - Test the user-space utilities:
   ```
   cd software/user
   make clean all
   ./m2sdr_util info
   ./m2sdr_rf init -samplerate=30720000
   ./tone_gen.py tone_tx.bin
   ./m2sdr_play tone_tx.bin 100000
   ```

2. **SoapySDR Detection/Probe:**
   - Detect the LiteX-M2SDR board:
   ```
   SoapySDRUtil --probe="driver=LiteXM2SDR"
   ```

3. **Run GNU Radio FM Test:**
   - Open and run the GNU Radio FM test:
   ```
   gnuradio-companion ../gnuradio/test_fm_rx.grc
   ```

4. **Enable Debugging in Kernel:**
    - Enable debugging:
    ```
    sudo sh -c "echo 'module litepcie +p' > /sys/kernel/debug/dynamic_debug/control"
    ```

### For Software & FPGA Developers

For those who want to explore the full potential of the LiteX-M2SDR board, including FPGA development, follow these additional steps after completing the software developer steps:

1. **Install LiteX:**
   - Follow the installation instructions from the LiteX Wiki: [LiteX Installation](https://github.com/enjoy-digital/litex/wiki/Installation). 📘


2. **Ethernet and PCIe Tests:**
   - For Ethernet tests, if the board is mounted in an Acorn Mini Baseboard:
   ```
   ./litex_m2sdr.py --with-ethernet --ethernet-sfp=0 --build --load
   ping 192.168.1.50
   ```
   - For PCIe tests, if the board is mounted directly in an M2 slot:
   ```
   ./litex_m2sdr.py --with-pcie --variant=m2 --build --load
   lspci
   ```

   - For PCIe tests, if the board is mounted directly in a LiteX Acorn Baseboard:
   ```
   ./litex_m2sdr.py --with-pcie --variant=baseboard --build --load
   lspci
   ```

4. **Use JTAGBone/PCIeBone:**
    - Start the LiteX server for JTAG or PCIe:
    ```
    litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg # JTAGBone
    sudo litex_server --pcie --pcie-bar=04:00.0              # PCIeBone (Adapt bar)
    ```

5. **Flash the Board Over PCIe:**
    - Flash the board:
    ```
    cd software
    ./flash.py ../build/litex_m2sdr_platform/gateware/litex_m2sdr_platform.bin
    ```

6. **Reboot or Rescan PCIe Bus:**
    - Rescan the PCIe bus:
    ```
    echo 1 | sudo tee /sys/bus/pci/devices/0000\:0X\:00.0/remove # Replace X with actual value
    echo 1 | sudo tee /sys/bus/pci/rescan
    ```

[> Contact
----------
E-mail: florent@enjoy-digital.fr

<div align="center">
  <img src="https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/0034fac5-d760-47ed-b93a-6ceaae47e978" width="100%">
</div>

