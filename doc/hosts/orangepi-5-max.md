# Running LiteX-M2SDR on OrangePI 5 Max Linux

Below is the [LiteX-M2SDR](../..) installed on an [OrangePi 5 Max](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-Max.html).
Please follow the step-by-step tutorial below to get your SDR running.

![Connecting LiteX-M2SDR to the OrangePI 5 Max](https://github.com/user-attachments/assets/5c25ed5b-a32e-493e-9ab1-b2280946ccf7)
![OrangePI 5 Max from the CPU side](https://github.com/user-attachments/assets/6d50b785-181d-4e82-a856-159ced4389f4)

## Create MicroSD Card Image

Please make sure to use a [fast MicroSD card of 64GB or larger](https://www.amazon.com/dp/B09X7BYSFG). We are using the [Rockchip community project maintained by Joshua Riek](https://github.com/Joshua-Riek/ubuntu-rockchip/).
You can download the latest [Ubuntu 24.04 base image for OrangePI 5 Max here](https://joshua-riek.github.io/ubuntu-rockchip-download/boards/orangepi-5-max.html).

```bash
# Download base image ...
curl -L -O https://github.com/Joshua-Riek/ubuntu-rockchip/releases/download/v2.3.1/ubuntu-24.04-preinstalled-server-arm64-orangepi-5-max.img.xz

# ... and write to MicroSD card plugged into the /dev/sdg drive.
# Please replace /dev/sdg with your MicroSD disk drive (see below)
xzcat ubuntu-24.04-preinstalled-server-arm64-orangepi-5-max.img.xz | sudo dd bs=128M of=/dev/sdg

# Ensure that all data is written by flushing disk caches.
sync

# You can unplug the MicroSD card now
```

Use `fdisk` to list all disk drives and identify where your MicroSD card is plugged in:
```bash
sudo fdisk -l
```

## Wiring Up and Running Your OrangePI Board

Connect your Ethernet cable, HDMI monitor, USB keyboard, and the MicroSD card you prepared earlier.
Power your OrangePI via the USB-C port using a 5V@5A power supply.

For the first login, your OrangePI system will prompt you to change the default user password on the HDMI monitor. It will then display its DHCP IP address.

## First Login into OrangePI and Update the Base Image

You can now log in over SSH using the IP shown earlier and the new user password:
```bash
ssh ubuntu@10.254.1.21

# Update the freshly installed system
sudo apt update
sudo apt upgrade
sudo reboot
```

## Installing LiteX-M2SDR on OrangePI

Insert the LiteX-M2SDR module before powering your updated OrangePI.
You’ll need an M2x3mm screw to secure the SDR module in place.

```bash
# Log in over SSH using your IP and password
ssh ubuntu@10.254.1.21

# On OrangePI, clone the LiteX-M2SDR source tree
mkdir Code
cd Code
git clone https://github.com/enjoy-digital/litex_m2sdr

# Compile the LiteX-M2SDR Linux kernel driver for arm64 ...
cd litex_m2sdr/software/kernel
# This line compiles and activates the driver
sudo ARCH=arm64 ./init.sh

# Compile the command-line tools and the SoapySDR driver for LiteX-M2SDR
cd ..
# Install missing dependencies ...
sudo apt install -y cmake soapysdr-tools libsoapysdr0.8 libsoapysdr-dev
# Compile command-line tools and SoapySDR driver
./build

# Verify the list of compiled tools
git status

# If you want to use LiteX-M2SDR Python tools later
sudo apt install -y python3-numpy
```

## Using LiteX-M2SDR on OrangePI

```bash
user/m2sdr_util info
user/m2sdr_rf
```

On subsequent boots, you’ll need to manually insert the LitePCI driver and grant access to your user to avoid using `sudo`:
```bash
sudo modprobe ~/Code/litex_m2sdr/software/kernel/litepcie.ko
sudo chmod 666 /dev/m2sdr0
# Check if it works
~/Code/litex_m2sdr/software/user/m2sdr_util info
```

Example LiteX session:
![Running LiteX-M2SDR on OrangePI 5 Max Linux via SSH](https://github.com/user-attachments/assets/84074b43-6239-42b9-a0b4-38b0ff84f206)

---
> Ubuntu is a trademark of Canonical Ltd. Rockchip is a trademark of Fuzhou Rockchip Electronics Co., Ltd. The Ubuntu Rockchip project is not affiliated with Canonical Ltd or Fuzhou Rockchip Electronics Co., Ltd. All other product names, logos, and brands are property of their respective owners. The Ubuntu name is owned by [Canonical Limited](https://ubuntu.com/).