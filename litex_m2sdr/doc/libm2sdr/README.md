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

## Example program

See `doc/libm2sdr/example_sync_rx.c` for a minimal SC16 RX capture example.

## Device identifiers

- PCIe: `pcie:/dev/m2sdr0`
- Ethernet: `eth:192.168.1.50:1234`

If no identifier is provided, the library defaults to `/dev/m2sdr0` (PCIe) or `192.168.1.50:1234` (Ethernet).

## API overview

- Device: `m2sdr_open`, `m2sdr_close`, `m2sdr_get_device_info`
- RF: `m2sdr_config_init`, `m2sdr_apply_config`, `m2sdr_set_frequency`, `m2sdr_set_sample_rate`, `m2sdr_set_bandwidth`, `m2sdr_set_gain`
- Streaming: `m2sdr_sync_config`, `m2sdr_sync_rx`, `m2sdr_sync_tx`
- Time: `m2sdr_get_time`, `m2sdr_set_time`

## Notes

- Streaming currently supports SC16/Q11 only.
- `m2sdr_sync_config` requires buffer size to match `DMA_BUFFER_SIZE / 4` samples (SC16).
- RX/TX DMA headers can be enabled via `m2sdr_set_rx_header` / `m2sdr_set_tx_header`.
