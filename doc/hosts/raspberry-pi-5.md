# Running LiteX-M2SDR on Raspberry Pi 5

Below you can see the [LiteX-M2SDR](../..) installed on an [Raspberry Pi 5](https://www.raspberrypi.com/products/raspberry-pi-5/).
Please follow this step-by-Step tutorial below to get your SDR running.

![Connecting LiteX-M2SDR to the Raspberry PI 5](https://github.com/user-attachments/assets/7205b4a2-814d-4db2-87c0-573993c92a3e)
![Raspberry Pi 5 Max from the CPU side](https://github.com/user-attachments/assets/4f821f79-f274-4270-8642-5e7e556e8577)

To connect the LiteX-M2SDR on the Raspberry Pi 5 board you needs an M.2 hat like the [Pineboards HatDrive! BM1L](https://pineboards.io/products/hatdrive-bottom-2230-2242-2280-for-rpi5)

## Raspberry Pi OS

### Create MicroSD card image

This step requires **rpi-imager**. On Debian or similar OS please use this command to install this tool:
```bash
sudo apt install rpi-imager
```

Once installed, launch the tool with this command (*root* user is required for write access to the mass storage):
```bash
sudo rpi-imager
```
![rpi-imager main page](https://github.com/user-attachments/assets/beb10486-4150-470f-8172-45ab108e9808)

1. click on **CHOOSE DEVICE** and select *Raspberry PI 5*</br>![choose_device](https://github.com/user-attachments/assets/54af5242-127c-4fe0-8d7b-01def2dc58a2)
2. click on **CHOOSE OS** and select *Raspberry Pi OS (64-bit)*</br>![choose_os](https://github.com/user-attachments/assets/c9afdcec-8a2b-4794-8363-033dc3a2c4be)
3. click on **CHOOSE STORAGE** and select the SD Card (please uses `sudo fdisk -l` to list mass storage devices)
4. click **NEXT**

On the page, *rpi-imager* allows OS customization:
![Image](https://github.com/user-attachments/assets/f81200fa-305a-4c13-b2a0-d0e958e3bca7)

if you plan to uses SSH initially, click on **EDIT SETTINGS**,
go to the **GENERAL** pane to set the username/password</br>![Image](https://github.com/user-attachments/assets/528a0460-79bf-48b9-ab02-f3cd50a2fce6)<br>

and enable SSH in the **SERVICES** pane.</br>
![Image](https://github.com/user-attachments/assets/9ed3a6b7-84dc-40a8-91b0-198ca10284c9)

After configuration, click **SAVE** and then **YES**.

Next page is the last point before erasing the device: double-check the selected mass storage.

Now, **rpi-imager** will download the image and write the SD Card.

### Tweaking the system

Once *rpi-imager* has written the SD-Card, some modifications must be applied to use a PCIe device. Mount the
first SD-Card partition and edit *config.txt* and *cmdline.txt*. Both files are in the *firmware* directory.

**config.txt**
In `[all]` section, please adds:

```sh
kernel=kernel8.img
dtparam=pciex1=on
dtoverlay=pcie-32bit-dma-pi5
dtoverlay=pciex1-compat-pi5,l1ss=off,no-l0s=on,no-mip=off
# Run as fast as firmware / board allows
arm_boost=1
```
- `kernel8.img` ensures a standard 4K PAGESIZE instead of 16K
- `dtparam=pciex1=on` enables PCIe support
- `dtoverlay=pcie-32bit-dma-pi5` enables 32-bit DMA addressing instead of 64-bit.
- `dtoverlay=pciex1-compat-pi5,l1ss=off,no-l0s=on,no-mip=off`: improves support for PCIe devices that may not work well with the default
  configuration, `l1ss=off` disables a power-saving feature, `no-l0s=on` disables L0s (Active State Power Management - L0s state) and `no-mip=off`
  enables MIP (Maximum Payload Size Inference)


**cmdline.txt**

Please append the line with `pci=noaer pcie_aspm=off`

- `pci=noaer`: disables PCIe advanced error reporting (`AER: Corrected error received`)
- `pcie_aspm=off`: ignore PCIe Active State Power Management. Leave any configuration done by firmware unchanged.

### Wiring up and running your Raspberry Pi board

Please plug in your Ethernet cable, HDMI monitor, USB keyboard and your MicroSD card generated earlier.
You can power your Raspberry Pi now over the USB-C port by using a 5V@4A power supply.

## First login and System Update

Log in over ssh using the assigned IP and your password:
```bash
ssh toto@XXX.YYY.Z.A

# Update the system
sudo apt update
sudo apt dist-upgrade
sudo reboot
```

### Installing LiteX-M2SDR on Raspberry Pi

Insert you LiteX-M2SDR module before powering your Raspberry Pi.
You require an M2x3mm screw for locking the SDR module in place.

```bash
# Login over ssh using your IP and password
ssh toto@XXX.YYY.Z.A

# On raspberry Pi, clone the LiteX-M2SDR source tree
mkdir Code
cd Code
git clone https://github.com/enjoy-digital/litex_m2sdr

# Compile the LiteX-M2SDR Linux kernel driver for arm64
cd litex_m2sdr/software/kernel
# Note that this line not only compiles the driver, but also activates it
sudo ARCH=arm64 ./init.sh 

# Now compile the command line tools and the SoapySDR driver for LiteX-M2SDR
cd ..
# Install missing dependencies ...
sudo apt install -y cmake soapysdr-tools libsoapysdr0.8 libsoapysdr-dev
# ... compile command line tools and SoapySDR driver
./build

# Verify list of compiled tools
git status

# If you want to play around later with LiteX-M2SDR Python tools
sudo apt install -y python3-numpy
```

### Playing around with LiteX-M2SDR on Raspberry PI 5
```bash
user/m2sdr_util info
user/m2sdr_rf
```

On subsequent boots manually insert the litexPCI driver and grant access to your user:
```bash
sudo modprobe ~/Code/litex_m2sdr/software/kernel/litepcie.ko
sudo chmod 666 /dev/m2sdr0
# Check whether it works
~/Code/litex_m2sdr/software/user/m2sdr_util info
```
Example LiteX session:

![Running LiteX-M2SDR Tools on Raspberry Pi 5](https://github.com/user-attachments/assets/19198ebe-3898-4209-a208-2ebb1a2417c2)

## Buildroot

TBD