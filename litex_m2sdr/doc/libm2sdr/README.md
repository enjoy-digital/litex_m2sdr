# LiteX-M2SDR C API (libm2sdr)

This directory documents the public C API for LiteX‑M2SDR. The API is intentionally close to BladeRF’s sync interface so C users can configure a device and stream samples without touching the CLI utilities.

## Build

From `litex_m2sdr/software/user`:

```
make
make install_dev PREFIX=/usr/local
```

This installs headers under `include/litex_m2sdr/` and the static library under `lib/`.

## Quick Start (SC16)

```c
#include <stdio.h>
#include <stdint.h>
#include "m2sdr.h"
#include "config.h" /* for DMA_BUFFER_SIZE */

int main(void)
{
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;

    /* Open device */
    if (m2sdr_open(&dev, "pcie:/dev/m2sdr0") != 0)
        return 1;

    /* Configure RF */
    m2sdr_config_init(&cfg);
    cfg.tx_freq = 2400000000LL;
    cfg.rx_freq = 2400000000LL;
    if (m2sdr_apply_config(dev, &cfg) != 0)
        return 1;

    /* Configure streaming */
    unsigned samples_per_buf = DMA_BUFFER_SIZE / 4; /* SC16 */
    if (m2sdr_sync_config(dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, samples_per_buf, 0, 1000) != 0)
        return 1;

    /* Receive one buffer */
    int16_t buf[DMA_BUFFER_SIZE / 2]; /* 2x int16 per sample */
    if (m2sdr_sync_rx(dev, buf, samples_per_buf, NULL, 1000) != 0)
        return 1;

    m2sdr_close(dev);
    return 0;
}
```

## Quick Start (SC8)

```c
#include <stdint.h>
#include "m2sdr.h"
#include "config.h"

int main(void)
{
    struct m2sdr_dev *dev = NULL;
    if (m2sdr_open(&dev, "pcie:/dev/m2sdr0") != 0)
        return 1;

    unsigned samples_per_buf = DMA_BUFFER_SIZE / m2sdr_format_size(M2SDR_FORMAT_SC8_Q7);
    if (m2sdr_sync_config(dev, M2SDR_TX, M2SDR_FORMAT_SC8_Q7,
                          0, samples_per_buf, 0, 1000) != 0)
        return 1;

    int8_t buf[DMA_BUFFER_SIZE];
    /* Fill buf with interleaved SC8 IQ data... */
    m2sdr_sync_tx(dev, buf, samples_per_buf, NULL, 1000);
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
- Capabilities: `m2sdr_get_capabilities`
- Control: `m2sdr_set_bitmode`, `m2sdr_set_dma_loopback`
- RF: `m2sdr_config_init`, `m2sdr_apply_config`, `m2sdr_set_frequency`, `m2sdr_set_sample_rate`, `m2sdr_set_bandwidth`, `m2sdr_set_gain`
- Streaming: `m2sdr_sync_config`, `m2sdr_sync_rx`, `m2sdr_sync_tx`
- Time: `m2sdr_get_time`, `m2sdr_set_time`
- Sensors: `m2sdr_get_fpga_dna`, `m2sdr_get_fpga_sensors`

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
- `m2sdr_sync_config` buffer size must match the DMA payload size for the chosen format.

## Troubleshooting

### Symbol collisions with other SDR drivers

If you use multiple SDR drivers that embed their own AD9361 stacks (for example, BladeRF),
symbol collisions can cause crashes or misbehavior. This build links AD9361 into libm2sdr
and binds symbols inside the Soapy module to avoid cross-driver resolution. If you build
custom modules, ensure they do not export or override AD9361 symbols used by LiteX‑M2SDR.

### SoapyRemote socket buffer warnings

When using SoapyRemote, you may see warnings about socket buffer size. Increase the system
limits to improve streaming stability, for example:

```
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
```
- RX/TX DMA headers can be enabled via `m2sdr_set_rx_header` / `m2sdr_set_tx_header`.
- Utilities now use `m2sdr_reg_read` / `m2sdr_reg_write` instead of direct CSR access.

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
