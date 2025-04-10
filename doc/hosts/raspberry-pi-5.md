# Running LiteX-M2SDR on Raspberry Pi 5

Below is the [LiteX-M2SDR](../..) installed on a [Raspberry Pi 5](https://www.raspberrypi.com/products/raspberry-pi-5/).
Please follow the step-by-step tutorial below to get your SDR running.

![Connecting LiteX-M2SDR to the Raspberry Pi 5](https://github.com/user-attachments/assets/7205b4a2-814d-4db2-87c0-573993c92a3e)
![Raspberry Pi 5 Max from the CPU side](https://github.com/user-attachments/assets/4f821f79-f274-4270-8642-5e7e556e8577)

To connect the LiteX-M2SDR on the Raspberry Pi 5 board, you need an M.2 hat like the [Pineboards HatDrive BM1L](https://pineboards.io/products/hatdrive-bottom-2230-2242-2280-for-rpi5).

## Raspberry Pi OS

### Create MicroSD Card Image

This step requires **rpi-imager**. On Debian or similar OS, use this command to install the tool:
```bash
sudo apt install rpi-imager
```

Once installed, launch the tool with the following command (*root* user is required for write access to mass storage):
```bash
sudo rpi-imager
```

![rpi-imager main page](https://github.com/user-attachments/assets/beb10486-4150-470f-8172-45ab108e9808)

1. Click on **CHOOSE DEVICE** and select *Raspberry Pi 5*.
   ![Choose device](https://github.com/user-attachments/assets/54af5242-127c-4fe0-8d7b-01def2dc58a2)
2. Click on **CHOOSE OS** and select *Raspberry Pi OS (64-bit)*.
   ![Choose OS](https://github.com/user-attachments/assets/c9afdcec-8a2b-4794-8363-033dc3a2c4be)
3. Click on **CHOOSE STORAGE** and select the SD card (use `sudo fdisk -l` to list mass storage devices).
4. Click **NEXT**.

On the next page, *rpi-imager* allows OS customization:

![Image](https://github.com/user-attachments/assets/f81200fa-305a-4c13-b2a0-d0e958e3bca7)

If you plan to use SSH initially, click on **EDIT SETTINGS**,
go to the **GENERAL** pane to set the username and password:
![Image](https://github.com/user-attachments/assets/528a0460-79bf-48b9-ab02-f3cd50a2fce6)
and enable SSH in the **SERVICES** pane:
![Image](https://github.com/user-attachments/assets/9ed3a6b7-84dc-40a8-91b0-198ca10284c9)

After configuration, click **SAVE** and then **YES**.

The next page is the last point before erasing the device: double-check the selected mass storage.

Now, **rpi-imager** will download the image and write it to the SD card.

### Tweaking the System

Once *rpi-imager* has written the SD card, some modifications must be applied to use a PCIe device. Mount the
first SD card partition and edit *config.txt* and *cmdline.txt*. Both files are in the *firmware* directory.

**config.txt**
In the `[all]` section, please add:

```bash
kernel=kernel8.img
dtparam=pciex1=on
dtoverlay=pcie-32bit-dma-pi5
dtoverlay=pciex1-compat-pi5,l1ss=off,no-l0s=on,no-mip=off
# Run as fast as firmware/board allows
arm_boost=1
```

- `kernel8.img`: ensures a standard 4K PAGE SIZE instead of 16K.
- `dtparam=pciex1=on`: enables PCIe support.
- `dtoverlay=pcie-32bit-dma-pi5`: enables 32-bit DMA addressing instead of 64-bit.
- `dtoverlay=pciex1-compat-pi5,l1ss=off,no-l0s=on,no-mip=off`: improves support for PCIe devices that may not work well with the default
  configuration; `l1ss=off` disables a power-saving feature, `no-l0s=on` disables L0s (Active State Power Management - L0s state), and `no-mip=off`
  enables MIP (Maximum Payload Size Inference).

**cmdline.txt**
Please append the line with `pci=noaer pcie_aspm=off`:

- `pci=noaer`: disables PCIe advanced error reporting (`AER: Corrected error received`).
- `pcie_aspm=off`: ignores PCIe Active State Power Management. Leaves any configuration done by firmware unchanged.

### Wiring Up and Running Your Raspberry Pi Board

Please plug in your Ethernet cable, HDMI monitor, USB keyboard, and the MicroSD card generated earlier.
You can power your Raspberry Pi now over the USB-C port using a 5V@4A power supply.

## First Login and System Update

Log in over SSH using the assigned IP and your password:
```bash
ssh toto@XXX.YYY.Z.A

# Update the system
sudo apt update
sudo apt dist-upgrade
sudo reboot
```

### Installing LiteX-M2SDR on Raspberry Pi

Insert the LiteX-M2SDR module before powering your Raspberry Pi.
You’ll need an M2x3mm screw to lock the SDR module in place.

```bash
# Log in over SSH using your IP and password
ssh toto@XXX.YYY.Z.A

# Install missing dependencies
sudo apt install -y cmake soapysdr-tools libsoapysdr0.8 libsoapysdr-dev

# On the Raspberry Pi, clone the LiteX-M2SDR source tree
mkdir Code
cd Code
git clone https://github.com/enjoy-digital/litex_m2sdr

# Move to the software directory
cd litex_m2sdr/litex_m2sdr/software

# Compile the command line tools and the SoapySDR module for Litex-M2SDR
./build.py

# Build and Install the LiteX-M2SDR Linux kernel driver for arm64
cd kernel
make ARCH=arm64 clean all
sudo make ARCH=arm64 install
cd ..

# Verify the list of compiled tools
git status

# If you want to use LiteX-M2SDR Python tools later
sudo apt install -y python3-numpy
```

### Using LiteX-M2SDR on Raspberry Pi 5

```bash
user/m2sdr_util info
user/m2sdr_rf
```

On subsequent boots, the LiteX-M2SDR driver is automatically loaded:
```bash
# Check whether it works
~/Code/litex_m2sdr/litex_m2sdr/software/user/m2sdr_util info
```

Example LiteX session:
![Running LiteX-M2SDR Tools on Raspberry Pi 5](https://github.com/user-attachments/assets/19198ebe-3898-4209-a208-2ebb1a2417c2)

## Buildroot

*TBD*