# Host Setup

Host-side settings that matter for reliable streaming with LiteX-M2SDR. Read this after the
[Quick Start](../README.md#quick-start) if streams do not start, or before running at high sample
rates. Platform-specific guides:

- [Raspberry Pi 5](hosts/raspberry-pi-5.md)
- [Orange Pi 5 Max](hosts/orangepi-5-max.md)
- [Jetson Orin / NVIDIA L4T](hosts/jetson-orin.md)

## IOMMU Passthrough

For PCIe streaming, set the IOMMU to passthrough mode. If you don't see I/Q data streams in your
SDR application, this is the first thing to check.

**x86/PC**:

```bash
# Add to GRUB config (/etc/default/grub):
GRUB_CMDLINE_LINUX="iommu=pt"
sudo update-grub && sudo reboot
```

**ARM (e.g. NVIDIA Jetson/Orin)**:

```bash
# Add to extlinux.conf (/boot/extlinux/extlinux.conf):
APPEND ... iommu.passthrough=1
sudo reboot
```

Keep the Tegra SMMU driver enabled. `iommu.passthrough=1` is the safer first test knob for current
L4T kernels; disabling `arm-smmu` globally can break other Jetson devices. See
[Jetson Orin host notes](hosts/jetson-orin.md).

> [!WARNING]
> For Intel CPUs: if a *kernel panic* occurs with the message **Corrupted page table at address**,
> add `intel_iommu=off` to `GRUB_CMDLINE_LINUX`. (This has been observed on an
> *11th Gen Intel(R) Core(TM) i7-11700B @ 3.20GHz*.)

## CPU Frequency Governor

For sustained high sample rates, set the CPU frequency governor to `performance`; on-demand
frequency scaling can cause RX overflows/TX underflows:

```bash
sudo apt install linux-tools-$(uname -r) linux-tools-common
sudo cpupower frequency-set -g performance
```

The setting resets at reboot; make it persistent with your distribution's preferred mechanism
(e.g. a systemd unit or `cpufrequtils`). For real-time loops (small DMA rings, IRQ pinning,
isolated cores), see [Low-Latency Streaming](low-latency.md).

## PCIe Generation And Lanes

Gen2 x1 is enough for the standard 61.44 MSPS 2T2R rate. Oversampling (122.88 MSPS, see
[Wide Analog Bandwidth](wide-bandwidth.md)) requires PCIe Gen2 x2/x4 bandwidth.

## Selecting The Transport At Runtime

The installed user tools, `libm2sdr` and SoapySDR module support both PCIe and Ethernet in one
build. Select the device explicitly:

| Tool | PCIe | Ethernet |
|------|------|----------|
| CLI utilities | `--device pcie:/dev/m2sdr0` | `--device eth:192.168.1.50:1234` |
| SoapySDR | `driver=LiteXM2SDR,path=/dev/m2sdr0` | `driver=LiteXM2SDR,eth_ip=192.168.1.50` |

Ethernet requires the board to be mounted on the LiteX Acorn Baseboard Mini with an Ethernet
gateware image; see [Ethernet](ethernet.md).

## Kernel Driver Debug Output

Enable the driver's dynamic debug messages:

```bash
sudo sh -c "echo 'module m2sdr +p' > /sys/kernel/debug/dynamic_debug/control"
```

See also the [kernel driver README](../litex_m2sdr/software/kernel/README.md) and the
[Debugging Guide](debugging-guide.md).
