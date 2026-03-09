# LiteX-M2SDR C API (libm2sdr)

This directory documents the public C API for LiteX-M2SDR. The API is intentionally close to BladeRF's sync interface so C users can configure a device and stream samples without touching the CLI utilities.

`libm2sdr` is now the primary low-level host interface for the project. The CLI utilities and the SoapySDR module both build on top of it.
The RF path is internally backend-based (`ad9361` today), so future RFIC
targets can reuse transport/stream/time code and swap only the RFIC module.

## Build

From `litex_m2sdr/software/user`:

```
make
make install_dev PREFIX=/usr/local
```

This installs:

- headers under `include/litex_m2sdr/`
- `libm2sdr.a` for static linking
- `libm2sdr.so.1` plus the `libm2sdr.so` symlink for shared linking
- `m2sdr.pc` under `lib/pkgconfig/`

## Getting started

1) Build and install:

```
make
sudo make install_dev PREFIX=/usr
sudo ldconfig
```

2) Run a quick sanity check:

```
m2sdr_selftest
```

Optional checks:

```
m2sdr_selftest --time
m2sdr_selftest --loopback
m2sdr_selftest --stream-loopback
```

3) If you use SoapySDR, (re)install the module after updating libm2sdr:

```
cd ../soapysdr/build
make
sudo make install
```

4) Check the installed C API metadata if needed:

```
pkg-config --modversion m2sdr
pkg-config --cflags --libs m2sdr
```

## Quick Start (SC16)

```c
#include <stdio.h>
#include <stdint.h>
#include "m2sdr.h"

int main(void)
{
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    int16_t *buf = m2sdr_alloc_buffer(format, samples_per_buf);

    /* Open device */
    if (m2sdr_open(&dev, "pcie:/dev/m2sdr0") != 0)
        return 1;

    /* Configure RF */
    m2sdr_config_init(&cfg);
    cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
    cfg.clock_source   = M2SDR_CLOCK_SOURCE_INTERNAL;
    cfg.tx_freq        = 2400000000LL;
    cfg.rx_freq        = 2400000000LL;
    if (m2sdr_apply_config(dev, &cfg) != 0)
        return 1;

    /* Configure streaming */
    if (m2sdr_sync_config(dev, M2SDR_RX, format,
                          0, samples_per_buf, 0, 1000) != 0)
        return 1;

    /* Receive one buffer */
    if (m2sdr_sync_rx(dev, buf, samples_per_buf, NULL, 1000) != 0)
        return 1;

    m2sdr_free_buffer(buf);
    m2sdr_close(dev);
    return 0;
}
```

## Quick Start (SC8)

```c
#include <stdint.h>
#include "m2sdr.h"

int main(void)
{
    struct m2sdr_dev *dev = NULL;
    enum m2sdr_format format = M2SDR_FORMAT_SC8_Q7;
    unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    int8_t *buf = m2sdr_alloc_buffer(format, samples_per_buf);
    if (m2sdr_open(&dev, "pcie:/dev/m2sdr0") != 0)
        return 1;

    if (m2sdr_sync_config(dev, M2SDR_TX, format,
                          0, samples_per_buf, 0, 1000) != 0)
        return 1;

    /* Fill buf with interleaved SC8 IQ data... */
    m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 1000);
    m2sdr_free_buffer(buf);
    m2sdr_close(dev);
    return 0;
}
```

## Example program

See `doc/libm2sdr/example_sync_rx.c` for a minimal SC16 RX capture example.
See `doc/libm2sdr/example_sync_tx.c` for a minimal SC16 TX example.
See `doc/libm2sdr/example_tone_tx.c` for a tone TX example (SC16/SC8).
See `doc/libm2sdr/example_rx_n.c` for RX N buffers with metadata.

## Device identifiers

- PCIe: `pcie:/dev/m2sdr0`
- Ethernet: `eth:192.168.1.50:1234`

If no identifier is provided, the library defaults to `/dev/m2sdr0` (PCIe) or `192.168.1.50:1234` (Ethernet).

## API overview

- Device: `m2sdr_open`, `m2sdr_close`, `m2sdr_get_device_info`
- Backend selection/interop: `m2sdr_get_transport`, `m2sdr_get_fd`, `m2sdr_get_eb_handle`
- RFIC discovery/extension: `m2sdr_get_rfic_name`, `m2sdr_get_rfic_caps`, `m2sdr_set_property`, `m2sdr_get_property`
- Capabilities: `m2sdr_get_capabilities`
- Control: `m2sdr_set_bitmode`, `m2sdr_set_dma_loopback`
- RF: `m2sdr_config_init`, `m2sdr_apply_config`, `m2sdr_set_rx_frequency`, `m2sdr_set_tx_frequency`, `m2sdr_set_sample_rate`, `m2sdr_set_bandwidth`, `m2sdr_set_rx_gain`, `m2sdr_set_tx_gain`
- Streaming: `m2sdr_stream_config_init`, `m2sdr_stream_configure`, `m2sdr_sync_rx`, `m2sdr_sync_tx`
- Time: `m2sdr_get_time`, `m2sdr_set_time`
- Sensors: `m2sdr_get_fpga_dna`, `m2sdr_get_fpga_sensors`

## Error model

Library calls return `0` on success and negative status codes on failure.
In addition to generic `M2SDR_ERR_INVAL`/`M2SDR_ERR_IO` classes, the API now
uses:

- `M2SDR_ERR_PARSE` for malformed strings/identifiers.
- `M2SDR_ERR_RANGE` for out-of-range numeric values.
- `M2SDR_ERR_STATE` for invalid call sequencing (for example, streaming before configuration).

Use `m2sdr_strerror()` for concise error text in logs.

## RFIC backends

- Active backend can be queried with `m2sdr_get_rfic_name()`.
- Backend ranges/features can be queried with `m2sdr_get_rfic_caps()`.
- Backend-specific controls use namespaced string properties via
  `m2sdr_set_property()` / `m2sdr_get_property()`.
- Environment override: set `M2SDR_RFIC=ad9361` to force backend selection.

## Library versioning

- `libm2sdr` public API version: `1.0.0`
- `libm2sdr` public ABI version: `1`
- installed shared-library SONAME: `libm2sdr.so.1`

This is the first intentionally versioned public C API for the project, so the library compatibility series starts at ABI 1.

## Capabilities example

```c
struct m2sdr_capabilities caps;
if (m2sdr_get_capabilities(dev, &caps) == 0) {
    int major = caps.api_version >> 16;
    int minor = caps.api_version & 0xffff;
    printf("API %d.%d, features=0x%08x\n", major, minor, caps.features);
}
```

## Notes

- Streaming supports SC16/Q11 and SC8/Q7.
- Use `m2sdr_bytes_to_samples()` / `m2sdr_samples_to_bytes()` instead of hard-coding sample sizes in applications.

## Troubleshooting

### Symbol collisions with other SDR drivers

If you use multiple SDR drivers that embed their own AD9361 stacks (for example, BladeRF),
symbol collisions can cause crashes or misbehavior. This build links AD9361 into libm2sdr
and binds symbols inside the Soapy module to avoid cross-driver resolution. If you build
custom modules, ensure they do not export or override AD9361 symbols used by LiteX-M2SDR.

### SoapyRemote socket buffer warnings

When using SoapyRemote, you may see warnings about socket buffer size. Increase the system
limits to improve streaming stability, for example:

```
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
```
- RX/TX DMA headers can be enabled via `m2sdr_set_rx_header` / `m2sdr_set_tx_header`.
- Utilities now use `m2sdr_reg_read` / `m2sdr_reg_write` instead of direct CSR access.
- RF helper logs are enabled by default; define `M2SDR_LOG_ENABLED=0` at build time to mute them.

## Migration (utilities to C API)

Replace direct CSR access:

```c
/* Before */
uint32_t v = m2sdr_readl(conn, CSR_XADC_TEMPERATURE_ADDR);

/* After */
struct m2sdr_fpga_sensors s;
m2sdr_get_fpga_sensors(dev, &s);
```

Replace bitmode/loopback:

```c
/* Before */
litepcie_writel(fd, CSR_AD9361_BITMODE_ADDR, 1);

/* After */
m2sdr_set_bitmode(dev, true);
```
