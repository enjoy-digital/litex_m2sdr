```{=latex}
\part{Software and use}
```

```{=html}
<h1 class="part">Part IV. Software and use</h1>
```

# Getting started {#getting-started}

This chapter takes you from a fresh Linux host to a board that receives and transmits samples. It
installs the software, checks the board, tunes the host, runs a first capture and opens the board
from a SoapySDR application. It ends with the guides of three single-board computers and with the
procedure to flash a release image.

The figure below shows the path. Each step gives the command, what you should see, and what to
check when you do not.

![The getting started path. The three cyan steps use the utilities of the
project.](start_path.svg)

The steps assume the default setup: a board in the M.2 slot of the host, with a PCIe image in its
flash. [Carriers and hosts](#carriers) describes the
other setups, and two sections below give the differences for a board on the baseboard, over
[Ethernet](#gs-ethernet) and over [USB3](#gs-usb3).

## Prerequisites {#gs-prerequisites}

You need a host with Linux, the board in an M-keyed M.2 slot, and antennas on the RF connectors
you use. The commands are those of Ubuntu; on another distribution, install the equivalent
packages with its package manager.

```sh
sudo apt install build-essential cmake git \
  pkg-config libsdl2-dev libgl1-mesa-dev \
  libsoapysdr-dev soapysdr-tools libsoapysdr0.8 \
  gnuradio gnuradio-dev libgnuradio-soapy3.10.9t64 gqrx-sdr \
  libsndfile1-dev libsamplerate0-dev libpng-dev
```

Not every package is needed for every use. The table below tells what each group is for.

| Packages | Needed for |
|-------------------------------------------|---------------------------------------------|
| `build-essential`, `cmake`, `git` | Kernel driver, library, utilities |
| `libsoapysdr-dev`, `soapysdr-tools` | SoapySDR driver and `SoapySDRUtil` |
| `gnuradio`, `gqrx-sdr` | The SDR applications of the last step |
| `libsndfile1-dev`, `libsamplerate0-dev` | `m2sdr_fm_tx` and `m2sdr_fm_rx` |
| `libsdl2-dev`, `libgl1-mesa-dev` | GUI tools `m2sdr_check` and `m2sdr_scan` |
| `libpng-dev` | `m2sdr_scan` |
| `libusb-1.0-0-dev`, not in the command above | USB3 host link |

The kernel driver builds against the headers of the running kernel, which it expects in
`/lib/modules/<version>/build`.

> **Warning**
> If the installation stops on an error about DKMS, run
> `sudo apt remove --purge xtrx-dkms dkms`, then run the installation again.

## Step 1: build and install the software {#gs-software}

The software has four parts: the kernel driver, the utilities, the library `libm2sdr` and the
SoapySDR driver. One script builds them all with `make` and CMake.

```sh
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr/litex_m2sdr/software
sudo ./build.py
```

Run as root, the script builds and installs. Run as a normal user, it only builds and says that
the install steps are skipped. The installation does four things:

- It installs the kernel module `m2sdr` and makes it load at boot.
- It adds a udev rule that gives every user access to the device nodes.
- It installs the utilities and the development files of the library under the prefix.
- It installs the SoapySDR driver under the prefix.

Then load the kernel driver, or reboot:

```sh
sudo modprobe m2sdr
```

**What you should see.** The script ends without error, and the device node `/dev/m2sdr0`
exists after the driver is loaded. `dmesg | grep m2sdr` shows the messages of the driver.

**If not.** A failure in the `kernel` directory usually means that the kernel headers are
missing. A missing `/dev/m2sdr0` with a loaded driver means that the board is not on the PCIe
bus: check `lspci`, the seating of the board and the image in its flash.

### Options of the build script {#gs-build-options}

| Option | Default | Effect |
|--------------------|-----------|--------------------------------------------------------|
| `--interface` | `runtime` | Default host link of the tools |
| `--prefix` | `/usr` | Install prefix |
| `--no-install` | off | Build only |
| `--no-sudo` | off | Skip the install steps even when run as root |
| `--skip-kernel` | off | Do not build or install the kernel driver |
| `--clean` | off | Run the clean targets first, for a full rebuild |

The default build opens PCIe and Ethernet boards with the same binaries: the device argument
chooses the host link at run time. USB3 boards are added when libusb-1.0 is installed. The
values `litepcie`, `liteeth` and `liteusb` only change which link the tools use when no device
is given; keep `runtime` unless you test that behaviour. Builds are incremental.

### Optional GUI tools {#gs-gui-tools}

`m2sdr_check` and `m2sdr_scan` are two optional tools with a graphical interface. They are built
only when the SDL2 and OpenGL development packages are installed and the pinned `cimgui`
submodule is present:

```sh
git submodule update --init --recursive litex_m2sdr/software/user/cimgui
```

Without these dependencies the build skips the two tools and nothing else. The same rule applies
to the FM tools without `libsndfile` and `libsamplerate`.

### Installing the parts by hand {#gs-manual-install}

When the script was run without root rights, install each part from its directory. The kernel
driver first:

```sh
cd litex_m2sdr/software/kernel
sudo make install
sudo insmod m2sdr.ko
```

The `insmod` line loads the driver without a reboot. Then the SoapySDR driver:

```sh
cd litex_m2sdr/software/soapysdr/build
sudo make install
```

An application that links `libm2sdr` needs its headers, its libraries and its pkg-config and
CMake files. The script installs them; this installs them by hand, here under another prefix:

```sh
cd litex_m2sdr/software/user
make
sudo make install_dev PREFIX=/usr/local
sudo ldconfig
```

`sudo make uninstall` in the `kernel` directory removes the module, its load-at-boot file and the
udev rule. [Host software](#software) describes each part and the library.

### Rebuilding during development {#gs-rebuild}

After a change in the driver or in the utilities, rebuild the part alone. For the kernel driver:

```sh
cd litex_m2sdr/software/kernel
make clean all
sudo make install
sudo insmod m2sdr.ko
```

For the utilities and the library:

```sh
cd litex_m2sdr/software/user
make clean all
```

The gateware tests of `test/` run without hardware, and the scripts of `scripts/` need a running
board. [Debugging](#debugging) lists the tests of each layer.

## Step 2: check the board {#gs-check}

Two commands tell whether the board and the software agree. The first one reads the registers of
the board through the kernel driver:

```sh
cd user
./m2sdr_util info
```

**What you should see.** The command prints five blocks, listed below. Every value must be
plausible.

| Block | Content | What to check |
|--------------|-------------------------------------------|---------------------------------------|
| SoC Info | Identifier, API version, variant, features | The variant and features of the image |
| FPGA Info | DNA, temperature, voltages, boot status | `Operational`, sane temperature |
| SI5351 Info | Presence, status bits, PLL sources | Present, PLL locked |
| AD9361 Info | Presence, product ID, temperature | Present |
| Board Time | Date and time of the board | A running time |

The second command opens the board through SoapySDR:

```sh
SoapySDRUtil --probe="driver=LiteXM2SDR"
```

It must list the capabilities and the configuration parameters of the device.

**If not.** An API version of `0.0`, shifted strings or impossible temperatures point to a host
link or driver problem, not to the RF part.

A boot status of `Fallback` means that the operational
image did not load: flash the board again. When `m2sdr_util info` works and SoapySDR finds no
device, the SoapySDR driver is not installed where SoapySDR looks for it.

Three more commands of `m2sdr_util` test the board without any RF signal:

```sh
./m2sdr_util scratch-test
./m2sdr_util clk-test
./m2sdr_util dma-test
```

They check a register write and read, the frequencies of the on-board clocks, and the sample path
between the host and the FPGA. [Debugging](#debugging) continues from here when one fails.

## Step 3: set up the host {#gs-host}

Four host settings decide whether streaming is reliable. Read this step when a stream does not
start, and before running at high sample rates.

### Kernel driver and IOMMU {#gs-iommu}

PCIe streaming works with the IOMMU enabled. Earlier builds of the kernel driver mapped the wrong
pages for zero-copy DMA behind an active IOMMU. The symptom was precise: SoapySDR received
buffers of zeros, while the tools that copy the buffers streamed correctly.

When an application receives no samples, first make sure that the loaded `m2sdr.ko` is built from
the same source tree as the rest of the software. Rebuild and reinstall it if in doubt.

With an older driver that cannot be updated, IOMMU passthrough is the workaround. On a PC, add
the option to the kernel command line in `/etc/default/grub`, then update GRUB and reboot:

```sh
GRUB_CMDLINE_LINUX="iommu=pt"
```

```sh
sudo update-grub
sudo reboot
```

On an ARM host, the option is `iommu.passthrough=1`; the [Jetson Orin](#gs-jetson) section gives
the file.

> **Warning**
> On an Intel CPU, a kernel panic with the message `Corrupted page table at address` calls for
> `intel_iommu=off` in `GRUB_CMDLINE_LINUX`. It was observed on an 11th generation Core i7.

### CPU frequency governor {#gs-governor}

On-demand frequency scaling can cause RX overflows and TX underflows at high sample rates. Set
the governor to `performance`:

```sh
sudo apt install linux-tools-$(uname -r) linux-tools-common
sudo cpupower frequency-set -g performance
```

The setting is lost at reboot. Make it persistent with the mechanism of the distribution, a
systemd unit or `cpufrequtils` for example. [Workflows](#workflows) covers the real-time setups:
small DMA rings, interrupt pinning and isolated cores.

### PCIe lanes {#gs-lanes}

PCIe Gen2 x1 is enough for 2T2R at 61.44 MSPS, with the small margin that
[M.2 slot of a host](#carriers-m2) computes. A higher rate needs an image built for two or four
lanes and a slot that provides them. This is the case of the oversampling mode, which is
Experimental (see [Status](#intro-status)).

### Selecting the device {#gs-device}

The same binaries open a PCIe or an Ethernet board. Name the device explicitly in scripts, so that
a command behaves the same on every host.

| Tool | Host link | Device argument |
|------------|-----------|-------------------------------------------------|
| Utilities | PCIe | `--device pcie:/dev/m2sdr0` |
| Utilities | Ethernet | `--device eth:192.168.1.50:1234` |
| SoapySDR | PCIe | `driver=LiteXM2SDR,path=/dev/m2sdr0` |
| SoapySDR | Ethernet | `driver=LiteXM2SDR,eth_ip=192.168.1.50` |

The utilities also accept two short forms: `-c N` for `/dev/m2sdrN`, and `-i` followed by the IP
address of an Ethernet board. With several boards in one PC, each board has its own device node.

### Driver debug messages {#gs-driver-debug}

The kernel driver has dynamic debug messages, off by default. This enables them:

```sh
sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
```

## Step 4: first RX and TX {#gs-first-rxtx}

`m2sdr_rf` initialises and configures the RFIC: sample rate, bandwidth, the two local
oscillators and the gains. Run it once before streaming with the utilities, and again to change
a setting.

```sh
./m2sdr_rf --sample-rate=30720000 --bandwidth=20000000 \
           --rx-freq=2400000000 --tx-freq=2400000000 \
           --rx-gain=10 --tx-att=10
```

The values accept the forms `30.72e6` and `20M`. The TX level is set as a positive attenuation in
dB. Without options the tool applies its defaults, a sample rate of 30.72 MSPS and both
frequencies at 2.4 GHz.

**What you should see.** The tool returns to the shell without error.

Then record. `m2sdr_record` writes the RX stream to a file, here 2 000 000 bytes of it:

```sh
./m2sdr_record rx_file.bin 2000000
```

The file holds raw I/Q samples, in the SC16 format unless `--format` says otherwise. The option
`--sigmf` writes a SigMF recording instead, with its metadata file:

```sh
./m2sdr_record --sigmf --sample-rate 30720000 --center-freq 2400000000 capture 2000000
```

For a quick look at a real signal, tune to a local FM broadcast station and record a bounded
capture:

```sh
./m2sdr_rf --sample-rate=5e6 --rx-freq=102.4e6 --rx-gain=30
./m2sdr_record /tmp/m2sdr-rx.iq 67108864
```

Transmission works the same way. `m2sdr_gen` generates a signal in real time, and `m2sdr_play`
plays a file, here ten times:

```sh
./m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1e6 --amplitude 0.5
./m2sdr_play rx_file.bin 10
```

> **Warning**
> A TX command radiates on the configured frequency. Transmit into a cable with an attenuator or
> a dummy load, or on a frequency you are allowed to use.

**If not.** A record that stays at zero bytes means that the RX stream does not flow: run
`./m2sdr_util dma-test`, then go back to [Step 3](#gs-host). The loopback tests of `m2sdr_util`
check the path with no antenna at all; [Debugging](#debugging) describes them.

[Host software](#software) is the reference of every utility, and [Workflows](#workflows) gives
the complete recipes, such as recording and replay or timed TX.

## Step 5: a first SoapySDR application {#gs-soapy}

Any SoapySDR application can use the board once the driver is installed. Check the discovery
first:

```sh
SoapySDRUtil --find="driver=LiteXM2SDR"
```

The command lists the PCIe boards, probes the default Ethernet address `192.168.1.50` and, with
the USB3 backend, lists the USB3 boards.

The repository has GNU Radio flowgraphs in `litex_m2sdr/software/gnuradio/`. The FM receiver is a
good end-to-end test: it tunes, demodulates and plays the audio.

```sh
gnuradio-companion litex_m2sdr/software/gnuradio/test_fm_rx.grc
```

In GQRX, choose the SoapySDR input and the driver `LiteXM2SDR`. In a GNU Radio flowgraph of your
own, use the Soapy source and sink blocks with the device arguments of
[Selecting the device](#gs-device).

**What you should see.** A spectrum that follows the tuned frequency, and audio on an FM
broadcast station.

**If not.** Buffers of zeros point to the kernel driver, as [Kernel driver and
IOMMU](#gs-iommu) explains. Overflows at high sample rates point to the
[CPU frequency governor](#gs-governor). [SDR frameworks](#frameworks) describes the driver, its
device arguments and the other applications.

### Writing your own application {#gs-libm2sdr}

The library `libm2sdr` is the common host interface of the utilities and of the SoapySDR driver.
Its examples are the starting point of a new application:

```sh
cd litex_m2sdr/software/user
make examples
../../doc/libm2sdr/example_sync_rx > /tmp/rx.iq
../../doc/libm2sdr/example_tone_tx
```

The development install provides the public headers, the generated register headers and the
pkg-config and CMake files. An installed library is found with
`pkg-config --cflags --libs m2sdr`, or from CMake:

```cmake
find_package(m2sdr REQUIRED)
target_link_libraries(app PRIVATE m2sdr::m2sdr)
```

[Host software](#software) summarises the API.

## A board on the baseboard over Ethernet {#gs-ethernet}

A board on the LiteX Acorn Baseboard Mini with an Ethernet image needs no kernel driver: build the
software with `./build.py --skip-kernel` if the host has no PCIe board. The default address of the
board is `192.168.1.50`. With the host on the same subnet, the first checks are:

```sh
ping 192.168.1.50
./m2sdr_util --device eth:192.168.1.50:1234 info
```

The steps 4 and 5 then apply with the Ethernet device arguments. The sample rate is bounded by
the rate of the Ethernet link. [Workflows](#workflows) gives the network setup and the streaming
recipe, and [Host links](#host-links) the Ethernet PHYs.

## A board on the baseboard over USB3 {#gs-usb3}

A board on the baseboard with a USB3 image reaches the host through an SFP to USB3 adapter and
a USB3 port. It needs no kernel driver either, but it needs libusb, a udev rule and a USB
memory setting, which [USB3 streaming](#flow-usb3) gives. The first checks are:

```sh
lsusb -d 1d50:605a
./m2sdr_util --device usb: info
```

The steps 4 and 5 then apply with the USB3 device arguments.

## Platform guides {#gs-platforms}

The three guides below give what differs from a PC. The steps 2 to 5 are the same on every host.

### Raspberry Pi 5 {#gs-rpi5}

The Raspberry Pi 5 takes the board on an M.2 HAT that accepts 2280 modules; the guide was
written with a Pineboards HatDrive BM1L. It uses Raspberry Pi OS (64-bit).

**Prepare the SD card.** Install and start the imager on another computer. Root rights are needed
to write the card.

```sh
sudo apt install rpi-imager
sudo rpi-imager
```

Choose the device "Raspberry Pi 5", the system "Raspberry Pi OS (64-bit)" and the SD card as
storage; `sudo fdisk -l` lists the drives. In the customisation settings, set the user name and
the password, and enable SSH if you work remotely. Check the selected drive once more before
writing.

**Change the boot configuration.** Mount the first partition of the card and edit two files at
its root. The running system mounts this partition as `/boot/firmware`. In `config.txt`, add to
the `[all]` section:

```sh
kernel=kernel8.img
dtparam=pciex1=on
dtoverlay=pcie-32bit-dma-pi5
dtoverlay=pciex1-compat-pi5,l1ss=off,no-l0s=on,no-mip=off
arm_boost=1
```

| Line | Purpose |
|------------------------------------------|---------------------------------------------------|
| `kernel=kernel8.img` | Kernel with 4 KiB pages instead of 16 KiB |
| `dtparam=pciex1=on` | Enables the PCIe port |
| `dtoverlay=pcie-32bit-dma-pi5` | 32-bit DMA addressing instead of 64-bit |
| `dtoverlay=pciex1-compat-pi5` | Compatibility settings of the PCIe port, see below |
| `arm_boost=1` | Runs the CPU as fast as the firmware allows |

The compatibility overlay takes three settings: `l1ss=off` disables the L1 power-saving
substates, `no-l0s=on` disables the L0s power state, and `no-mip=off` enables MIP, the
maximum payload size inference of the overlay.

In `cmdline.txt`, append to the single line:

```sh
pci=noaer pcie_aspm=off
```

`pci=noaer` disables PCIe advanced error reporting and its `AER: Corrected error received`
messages. `pcie_aspm=off` makes the kernel ignore active state power management and keep what the
firmware configured.

**Assemble and boot.** Insert the board in the HAT before powering the Raspberry Pi, and lock it
with an M2 screw of 3 mm. Connect the network and the SD card, then power the Raspberry Pi through
its USB-C port with a 5 V, 4 A supply. Log in and update the system:

```sh
sudo apt update
sudo apt dist-upgrade
sudo reboot
```

**Build the software.** The kernel driver is built for arm64 in its own directory:

```sh
sudo apt install -y cmake soapysdr-tools libsoapysdr0.8 libsoapysdr-dev
git clone https://github.com/enjoy-digital/litex_m2sdr
cd litex_m2sdr/litex_m2sdr/software
./build.py
cd kernel
make ARCH=arm64 clean all
sudo make ARCH=arm64 install
cd ..
```

The Python tools of the repository also need `python3-numpy`. The driver loads by itself at the
next boots.

**Check.** Run the commands of [Step 2](#gs-check) and [Step 4](#gs-first-rxtx) from the
`software` directory, with the `user/` prefix:

```sh
user/m2sdr_util info
user/m2sdr_rf
```

The FM receiver flowgraph of [Step 5](#gs-soapy) is a good end-to-end receive test on this host
too.

When the board enumerates and DMA streaming gives no samples, rebuild the kernel driver from the
same source tree first, as [Kernel driver and IOMMU](#gs-iommu) explains. With an older driver,
`iommu.passthrough=1` on the kernel command line is the workaround. Do not use
`arm-smmu.disable=1` as a default setting: it disables the SMMU for the whole system.

### Orange Pi 5 Max {#gs-opi5max}

The Orange Pi 5 Max has an M.2 slot on board. The guide uses an Ubuntu 24.04 server image for
this computer, from the community project `ubuntu-rockchip`. The file below is part of its
release v2.3.1; download it from the Releases page of that project.

**Prepare the SD card.** Use a fast card of 64 GB or more. Find its drive with `sudo fdisk -l`,
then write the image and flush the caches. Replace `/dev/sdX` with the drive of the card.

```sh
xzcat ubuntu-24.04-preinstalled-server-arm64-orangepi-5-max.img.xz | \
  sudo dd bs=128M of=/dev/sdX
sync
```

**Assemble and boot.** Insert the board before powering the computer, and lock it with an M2
screw of 3 mm. Connect the network, a monitor, a keyboard and the SD card, then power the computer
through its USB-C port with a 5 V, 5 A supply. The first login asks for a new password and shows
the IP address. Update the system:

```sh
sudo apt update
sudo apt upgrade
sudo reboot
```

**Build the software and check.** The commands are those of the
[Raspberry Pi 5](#gs-rpi5): the packages, `./build.py`, the arm64 build of the kernel driver, then
`user/m2sdr_util info` and `user/m2sdr_rf`. The driver loads by itself at the next boots.

### Jetson Orin {#gs-jetson}

These notes cover what is specific to an NVIDIA Jetson Orin with NVIDIA L4T.

**Kernel driver.** Install the kernel headers of the running L4T kernel, then build the driver
from its directory:

```sh
sudo apt install nvidia-l4t-kernel-headers
cd litex_m2sdr/software/kernel
make
sudo make install
sudo modprobe m2sdr
```

L4T installs its headers in a path that depends on the release, such as
`/usr/src/linux-headers-$(uname -r)-ubuntu20.04_aarch64`. When the build does not find them, give
the kernel build directory with the `KERNEL_PATH` variable of `make`.

**IOMMU.** The SMMU can stay fully enabled. Streaming with address translation active is verified
on JetPack 7, L4T R39, with the stock kernel command line. Never add `arm-smmu.disable=1` as a
default workaround: it disables the SMMU globally and can break other peripherals of the Jetson.

With an older driver that cannot be updated, the passthrough workaround goes on the `APPEND` line
of `/boot/extlinux/extlinux.conf`:

```sh
APPEND ... iommu.passthrough=1
```

Reboot after the change. Keep a known-good boot entry or a serial console when you change the
kernel command line.

**High-rate RX.** Five rules keep a sustained RX stream on a Jetson free of overflows:

- Configure the stream once, with `m2sdr_sync_config()`.
- Ask `m2sdr_sync_rx()` for large blocks, about 2 ms of samples, to spread the cost of the system
  calls.
- Run the RX loop in its own thread and never block it on downstream processing.
- Hand the filled buffers to a second thread or a queue when more work is needed.
- Watch the overflow counters and the ring high-water marks, with `m2sdr_get_stream_stats()`.

`m2sdr_sync_config()` takes the fixed size of a DMA buffer in samples. Batch in the application
with larger `m2sdr_sync_rx()` requests instead of changing this size.

## Flashing a release image {#gs-flash}

Prebuilt images of the common configurations are published on the Releases page of the
repository. `scripts/flash_release.py` downloads one and writes it to the flash of the board over
JTAG. It needs neither Vivado nor a working PCIe link, which also makes it the way to recover a
board.

### What you need {#gs-flash-needs}

- A USB JTAG probe with an FTDI chip on the [JTAG connector](#hw-jtag) of the board, or the
  baseboard and its USB-C JTAG port.
- A powered board: in its M.2 slot, or on the baseboard.
- openFPGALoader, with its `spiOverJtag_xc7a200tsbg484.bit.gz` bridge and its USB access rules.
- Python 3 and an Internet connection.

### Flashing one board {#gs-flash-one}

Without argument, the script flashes the latest stable release with the image for an M.2 slot and
one PCIe lane:

```sh
./scripts/flash_release.py
```

The script runs these steps and stops at the first failure:

1. It downloads the release archive and checks its SHA-256 and its manifest.
2. It identifies the FPGA and reads its DNA, then asks for a confirmation.
3. It backs up the first 16 MiB of the flash, which hold both multiboot slots.
4. It writes the fallback image at `0x00000000` and the operational image at `0x00800000`.
5. It reads both images back and compares them.
6. It checks that the FPGA starts from the flash and reads the multiboot status.

**What you should see.** The script reports `PASS`. `PASS` covers the content of the flash and
the start of the FPGA; it does not test PCIe or RF. Reboot the host, or rescan the PCIe bus, then
run [Step 2](#gs-check). To qualify PCIe and RF on a board, run the board check `autotest.py`
that [Host software](#software) describes.

### Choosing the image {#gs-flash-images}

`--image` selects the image. It must match the carrier of the board.

| `--image` | Carrier | Content |
|--------------------------------------------|-----------|---------------------------------|
| `m2_pcie_x1` | M.2 slot | PCIe, one lane; the default |
| `m2_pcie_x2` | M.2 slot | PCIe, two lanes |
| `baseboard_eth` | Baseboard | Ethernet |
| `baseboard_eth_ptp_rfic_clock` | Baseboard | Ethernet, PTP, PTP-disciplined RFIC clock |
| `baseboard_pcie_x1_eth` | Baseboard | PCIe, one lane, and Ethernet |

```sh
./scripts/flash_release.py --image m2_pcie_x2
```

A configuration outside this list is built from the sources;
[Building the gateware](#building-gateware) explains how, and how to flash over PCIe.

### Other operations {#gs-flash-options}

| Option | Effect |
|-------------------|------------------------------------------------------------------|
| `--release` | Release tag such as `2026_05_15`; `latest` by default |
| `--dry-run` | Downloads and validates the images, without access to the board |
| `--verify-only` | Reads the flash back and checks the boot, without writing |
| `--no-backup` | Skips the backup of the flash |
| `--yes`, `-y` | Skips the confirmation of a single board |
| `--batch` | Flashes successive boards with one release |
| `--cable` | Forces the openFPGALoader cable |
| `--busdev-num` | Selects one probe by its USB address, such as `001:016` |
| `--freq` | JTAG frequency in Hz, 20000000 by default |
| `--loader` | Path of the openFPGALoader executable |
| `--output-dir` | Directory of the downloads, logs and backups |

`--verify-only` reconfigures the FPGA for the time of the check. When several probes are
connected, list them and select one:

```sh
openFPGALoader --scan-usb
./scripts/flash_release.py --busdev-num 001:016
```

### Flashing a batch of boards {#gs-flash-batch}

The batch mode serves a production run. Press Enter after connecting each board, and enter `q` to
finish.

```sh
./scripts/flash_release.py --batch --release 2026_05_15
./scripts/flash_release.py --batch --release 2026_05_15 --no-backup
```

Pin the release for a repeatable run, and skip the backup only for blank boards. The release
stays the same for the whole batch. A board that already passed is skipped when its FPGA DNA is
seen again.

A board that failed can be tried again in the same batch. The batch mode always
waits for the operator between boards, and its exit status is not zero when an attempt failed.

### Records and failures {#gs-flash-records}

The script saves everything under `build/flash_release/`: the downloads, the backups, the
readback images, and for each board a `programmer.log` and a `result.json`. The record of a board
holds the release, the source revision, the FPGA DNA, the hashes of the images and the result.
Keep the records with the production batch.

When an operation fails, read the log of the board, check the power, the JTAG connection and the
USB permissions, then run the script again with the same `--release`. A lower JTAG frequency,
such as `--freq 10000000`, helps with an unreliable cable. After an interrupted write, run the
complete flashing again over JTAG to restore both slots.

Where to go next: [Workflows](#workflows) for the task recipes, [Host software](#software) and
[SDR frameworks](#frameworks) for the references, [Building the gateware](#building-gateware) to
build an image of your own.
