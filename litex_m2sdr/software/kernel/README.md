# LiteX M2SDR Linux Kernel Driver

> [!Note]
> This kernel driver provides a straightforward and flexible way to interface with the **LiteX-M2SDR** board over PCIe. It is part of the [LitePCIe](https://github.com/enjoy-digital/litepcie) suite, developed with a focus on simplicity for easy integration into your own applications or frameworks.

## Overview

By loading the **m2sdr** kernel module, you gain access to:
- **DMA channels (TX/RX)** for high-speed data transfer.
- **Control/status registers** to configure and monitor hardware.
- **Interrupt handling** for a responsive data flow.
- **SATA userspace DMA ioctl** when the loaded bitstream exposes the M2SDR SATA
  CSR block.

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
- **SATA**
  SATA host access is exposed through the M2SDR userspace utilities, such as
  `m2sdr_sata`.
- **Per-channel DMA geometry**
  Each DMA channel has its own buffer size and MSI divisor, so a bulk I/Q channel
  and a low-rate side channel no longer have to share one ring geometry: buffer
  completion (and therefore the latency the host sees) scales with
  `buffer_size / byte_rate`, and an MSI is only raised every
  `dma_buffer_per_irq` buffers.

  Both are module parameters, one value per DMA channel, defaulting to
  `DMA_BUFFER_SIZE` (8192) and `DMA_BUFFER_PER_IRQ` (8) for every channel:
```
# DMA0: 8 KiB buffers, one MSI every 8 buffers (throughput, deep ring)
# DMA1: 512 B buffers, one MSI per buffer      (a record is visible in ~1 ms)
sudo insmod m2sdr.ko dma_buffer_size=8192,512 dma_buffer_per_irq=8,1

# ... or persistently:
echo "options m2sdr dma_buffer_size=8192,512 dma_buffer_per_irq=8,1" | \
    sudo tee /etc/modprobe.d/m2sdr.conf
```
  The chosen geometry is reported at probe time (`dmesg | grep DMA`) and, per
  channel, through `LITEPCIE_IOCTL_MMAP_DMA_INFO` and `LITEPCIE_IOCTL_DMA_STATS`
  — applications should read the buffer size from those ioctls rather than assume
  the compile-time default.

  Constraints, enforced when the module loads:
  - a multiple of 64 bytes, up to 16 MiB (the descriptor length field is 24-bit);
  - either a multiple of the page size, or an exact divisor of it (sub-page
    buffers are packed several per page so the ring stays dense, both for
    `read()`/`write()` and for the `mmap()` layout);
  - `dma_buffer_per_irq` must divide `DMA_BUFFER_COUNT` (256), which keeps the
    MSI spacing uniform across ring wraps.

  Size a buffer for the rate its channel carries: a buffer completes in
  `buffer_size / byte_rate`, and with `dma_buffer_per_irq=1` the channel raises
  `byte_rate / buffer_size` interrupts per second. Very small buffers on a
  high-rate channel are therefore an interrupt storm (a 64-byte buffer on an
  8 MB/s stream is 125k IRQ/s), and the ring also gets shallower:
  `DMA_BUFFER_COUNT x buffer_size` is the headroom against a host stall.

  Note that the DMA header/timestamp framing (`CSR_HEADER_*_FRAME_CYCLES`) is
  sized for 8192-byte buffers by default. When enabling header mode on a channel
  with a different buffer size, program `frame_cycles = buf_size / 8 - 2`.
- **Debug Logging**
  To enable detailed logs:
```
sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
```
  This helps diagnose data flow or interrupt issues. 🔎
- **Host Requirements**
  For IOMMU/DMA settings and PCIe expectations, see the top-level README.

---

## File Structure

- **main.c**
  The core driver logic, including:
  - PCI enumeration (probe/remove)
  - BAR0 memory mapping
  - DMA buffer allocation
  - Interrupt registration
  - `/dev/m2sdrX` char device operations
  - SATA userspace DMA ioctl handling

---

## Contributing & More

The **LiteX-M2SDR** kernel driver is developed alongside [LiteX](https://github.com/enjoy-digital/litex) and [LitePCIe](https://github.com/enjoy-digital/litepcie). We encourage you to contribute back if you find any gaps or have enhancements to propose. 🤗

Happy hacking with your **LiteX M2SDR** board!
