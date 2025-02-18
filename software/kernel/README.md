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

## Building & Installing

1. **Build the driver**
   ~~~~
   make
   ~~~~
   This compiles the `litepcie.ko` module.

2. **Load the driver**
   ~~~~
   ./init.sh
   ~~~~
   This script:
   - Installs the driver (via `insmod litepcie.ko` or `modprobe litepcie`).
   - Creates `/dev/m2sdrN` device nodes, which user-space applications can open for DMA operations.

3. **Check dmesg output (optional)**
   ~~~~
   dmesg | grep litepcie
   ~~~~
   Look for messages indicating that the board is recognized and configured.

4. **Remove the driver**
   ~~~~
   sudo rmmod litepcie
   ~~~~
   This removes the device nodes and stops any active DMA transfers.

---

## Usage & Notes

- **Multiple /dev entries**
  Each DMA channel appears as its own `/dev/m2sdrX` device (e.g., `/dev/m2sdr0`, `/dev/m2sdr1`, etc.).
- **User-Space Tools**
  You can use `m2sdr_util`, `m2sdr_play`, or `m2sdr_record` to test DMA, or create custom applications interfacing with `/dev/m2sdrX`.
- **Debug Logging**
  To enable detailed logs:
  ~~~~
  sudo sh -c "echo 'module litepcie +p' > /sys/kernel/debug/dynamic_debug/control"
  ~~~~
  This helps diagnose data flow or interrupt issues. 🔎
- **IOMMU**
  If your system has an IOMMU, ensure it’s set to passthrough mode. Otherwise, DMA may be blocked or fail to work properly.

---

## File Structure

- **litepcie.c**
  The core driver logic, including:
  - PCI enumeration (probe/remove)
  - BAR0 memory mapping
  - DMA buffer allocation
  - Interrupt registration
  - `/dev/m2sdrX` char device operations

- **init.sh**
  A helper script that:
  - Loads the `litepcie` module
  - Automatically creates the device nodes

---

## Contributing & More

The **LiteX-M2SDR** kernel driver is developed alongside [LiteX](https://github.com/enjoy-digital/litex) and [LitePCIe](https://github.com/enjoy-digital/litepcie). We encourage you to contribute back if you find any gaps or have enhancements to propose. 🤗

Happy hacking with your **LiteX M2SDR** board!
