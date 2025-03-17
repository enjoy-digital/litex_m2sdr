# LiteX M2SDR Linux Kernel Driver

> [!Note]
> This kernel driver provides a straightforward and flexible way to interface with the **LiteX-M2SDR** board over PCIe. It is part of the [LitePCIe](https://github.com/enjoy-digital/litepcie) suite, developed with a focus on simplicity for easy integration into your own applications or frameworks.

## Overview

By loading the **litepcie** kernel module, you gain access to:
- **DMA channels (TX/RX)** for high-speed data transfer.
- **Control/status registers** to configure and monitor hardware.
- **Interrupt handling** for a responsive data flow.

Its lightweight design aims to be simple to maintain and straightforward to customize for your specific needs. 🚀

---

## Building, Installing & Uninstalling

### 1. Build the driver
```
make
```
This compiles the `m2sdr.ko` module.

### 2. Install the driver
```
sudo make install
```
This performs the following steps:
- Installs the `m2sdr` module to the kernel directory.
- Enables auto-load on boot (`/etc/modules-load.d/m2sdr.conf`).
- Sets up a **udev rule** to allow non-root access (`/etc/udev/rules.d/99-m2sdr.rules`).
- Triggers **udev** to apply changes.

### 3. Load the driver manually (optional)
If you need to use the board without a reboot:
```
sudo insmod m2sdr.ko
```
### 4. Check driver status
```
dmesg | grep m2sdr
```
Look for messages indicating that the board is recognized and configured.

### 5. Uninstall the driver
```
sudo make uninstall
```
This removes:
- The kernel module.
- Auto-load configuration (`/etc/modules-load.d/m2sdr.conf`).
- The **udev rule** (`/etc/udev/rules.d/99-m2sdr.rules`).
- Updates the module dependencies.

---

## Usage & Notes

- **Multiple /dev entries**
  Each DMA channel appears as its own `/dev/m2sdrX` device (e.g., `/dev/m2sdr0`, `/dev/m2sdr1`, etc.).
- **User-Space Tools**
  You can use `m2sdr_util`, `m2sdr_play`, or `m2sdr_record` to test DMA, or create custom applications interfacing with `/dev/m2sdrX`.
- **Debug Logging**
  To enable detailed logs:
```
sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
```
  This helps diagnose data flow or interrupt issues. 🔎
- **IOMMU**
  If your system has an IOMMU, ensure it’s set to passthrough mode. Otherwise, DMA may be blocked or fail to work properly.

---

## File Structure

- **main.c**
  The core driver logic, including:
  - PCI enumeration (probe/remove)
  - BAR0 memory mapping
  - DMA buffer allocation
  - Interrupt registration
  - `/dev/m2sdrX` char device operations

---

## Contributing & More

The **LiteX-M2SDR** kernel driver is developed alongside [LiteX](https://github.com/enjoy-digital/litex) and [LitePCIe](https://github.com/enjoy-digital/litepcie). We encourage you to contribute back if you find any gaps or have enhancements to propose. 🤗

Happy hacking with your **LiteX M2SDR** board!
