# LiteX-M2SDR on Jetson Orin / NVIDIA L4T

These notes cover the host-side pieces that are specific to Jetson Orin
systems running NVIDIA L4T.

## Kernel Module Build

Install the NVIDIA kernel headers that match the running L4T kernel:

```bash
sudo apt install nvidia-l4t-kernel-headers
```

Then build the LitePCIe kernel module from the repository:

```bash
cd litex_m2sdr/software/kernel
make
sudo make install
sudo modprobe litepcie
```

L4T installs headers under `/usr/src/linux-headers-$(uname -r)-ubuntu20.04_aarch64`
or a nearby NVIDIA-specific path depending on the release. If the module build
cannot find them automatically, pass the matching kernel build directory through
the `KERNEL_PATH=...` make variable.

## IOMMU / SMMU

Keep the Tegra SMMU driver enabled. Do not add `arm-smmu.disable=1` as a
default workaround; it disables the SMMU globally and can break unrelated
Jetson peripherals.

If PCIe enumeration works but DMA streaming does not produce samples, first try
IOMMU passthrough:

```bash
# /boot/extlinux/extlinux.conf
APPEND ... iommu.passthrough=1
```

Reboot after changing `extlinux.conf`. Keep a known-good boot entry or serial
console access when changing kernel command-line options.

## High-Rate Streaming

For sustained RX on Jetson:

- Configure the stream once with `m2sdr_sync_config()`.
- Use larger `m2sdr_sync_rx()` requests, for example about 2 ms of samples, to
  amortize syscall and scheduling overhead.
- Run the RX pump in its own thread and avoid blocking it on downstream
  processing or other slow operations.
- Hand completed buffers to a second thread or queue when additional work is
  needed.
- Watch `m2sdr_get_stream_stats()` or the utility diagnostics for RX overflow
  counters and ring high-water marks.

`m2sdr_sync_config()` still takes the fixed DMA descriptor size in samples. Use
larger per-call `m2sdr_sync_rx()` sizes for application batching instead of
changing the descriptor size.

## Linking Applications

The development install exports the public libm2sdr headers, the generated
kernel CSR headers, `pkg-config` metadata, and CMake package files:

```bash
pkg-config --cflags --libs m2sdr
```

or:

```cmake
find_package(m2sdr REQUIRED)
target_link_libraries(app PRIVATE m2sdr::m2sdr)
```
