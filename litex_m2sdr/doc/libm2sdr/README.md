# LiteX-M2SDR C API (libm2sdr)

This directory documents the public C API for LiteX-M2SDR. The API is intentionally close to BladeRF's sync interface so C users can configure a device and stream samples without touching the CLI utilities.

`libm2sdr` is now the primary low-level host interface for the project. The CLI utilities and the SoapySDR module both build on top of it.

Note on TX control semantics: TX control uses positive attenuation values across
both native tools and the SoapySDR layer. Use the `tx_att` config field and
`m2sdr_set_tx_att()` helper.

## Build

From `litex_m2sdr/software/user`:

```
make
make install_dev PREFIX=/usr/local
```

The default `make` build uses `INTERFACE=USE_RUNTIME`, which compiles one
`libm2sdr` with both LitePCIe and LiteEth support. `INTERFACE=USE_LITEPCIE`
and `INTERFACE=USE_LITEETH` remain useful for legacy/default-target testing,
but they do not create separate public APIs for PCIe and Ethernet applications.
The shared and static libraries include the transport helper objects needed by
both backends, and the installed `pkg-config`/CMake metadata exports both the
public libm2sdr headers and generated kernel CSR headers.

This installs:

- public libm2sdr headers under `include/litex_m2sdr/libm2sdr/`
- generated kernel CSR headers under `include/litex_m2sdr/kernel/`
- `libm2sdr.a` for static linking
- `libm2sdr.so.1` plus the `libm2sdr.so` symlink for shared linking
- `m2sdr.pc` under `lib/pkgconfig/`
- `m2sdrConfig.cmake` under `lib/cmake/m2sdr/`

## Getting started

1) Build and install:

```
make
sudo make install_dev PREFIX=/usr
sudo ldconfig
```

2) Run a quick sanity check:

```
m2sdr_util info
```

Optional checks:

```
m2sdr_util scratch-test
m2sdr_util clk-test
m2sdr_util dma-test
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

For CMake projects, use:

```
find_package(m2sdr REQUIRED)
target_link_libraries(my_app PRIVATE m2sdr::m2sdr)
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
    cfg.rx_gain1       = 20;
    cfg.rx_gain2       = 20;
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

- PCIe explicit form: `pcie:/dev/m2sdr0`
- PCIe shorthand: `/dev/m2sdr0`
- Ethernet explicit form: `eth:192.168.1.50:1234`
- Ethernet shorthand: `192.168.1.50` or `192.168.1.50:1234`

If no identifier is provided, the runtime/default PCIe build opens
`/dev/m2sdr0`. A build made with `INTERFACE=USE_LITEETH` defaults to
`192.168.1.50:1234`, but explicit identifiers are preferred in applications
that may run with either transport.

## RF configuration lifecycle

`m2sdr_apply_config()` is the full RF bring-up path. It initializes the AD9361
state, programs the common RF clocking parameters, and applies the requested
channel layout. Treat it as a setup-time operation and call it before streaming
starts.

Use `m2sdr_apply_config_if_needed()` when startup code may be called more than
once with the same config. It returns success for an already-applied matching
config, but still rejects attempts to apply different settings to an active
RFIC.

For runtime retunes or small adjustments, prefer the per-field setters such as
`m2sdr_set_rx_frequency()`, `m2sdr_set_tx_frequency()`,
`m2sdr_set_sample_rate()`, `m2sdr_set_bandwidth()`, `m2sdr_set_rx_gain()`, and
`m2sdr_set_tx_att()`. These setters require an RFIC that was already brought up
with `m2sdr_apply_config()` or an advanced integration path such as SoapySDR.

## RX gain mode

`m2sdr_config_init()` defaults RX gain control to manual mode and programs
`rx_gain1` / `rx_gain2` during `m2sdr_apply_config()`. Applications should set
the gain values they want before applying the config:

```c
cfg.rx_gain1 = 20;
cfg.rx_gain2 = 20;
```

To use AD9361 AGC instead, clear `cfg.program_rx_gains` and set
`cfg.program_rx_gain_modes = true` with `cfg.rx_gain_mode1` /
`cfg.rx_gain_mode2`, or call `m2sdr_set_rx_gain_mode()` after RF bring-up.

## Stream buffer sizing

`m2sdr_sync_config()` configures the fixed backend DMA/UDP descriptor size. The
`buffer_size` argument is expressed in samples and must match the current
descriptor payload:

```c
unsigned samples_per_buf =
    m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
```

Use larger `m2sdr_sync_rx()` / `m2sdr_sync_tx()` requests when the application
wants batching. For example, at 30.72 MS/s, a 2 ms SC16 RX request is about
61440 samples even though each backend descriptor remains 8192 bytes.

## Channel layout and stream packing

`M2SDR_CHANNEL_LAYOUT_1T1R` selects the RF/PHY channel layout, but the current
host streaming path still uses the fixed transport descriptor layout. Do not
assume PCIe DMA bandwidth halves automatically in 1T1R mode. For applications
that need a stable host ABI today, use `M2SDR_CHANNEL_LAYOUT_2T2R` and demux the
active channel in software until a true packed 1T1R stream mode is added.

## API overview

- Device: `m2sdr_open`, `m2sdr_close`, `m2sdr_get_device_info`
- Backend selection/interop: `m2sdr_get_transport`, `m2sdr_get_fd`, `m2sdr_get_eb_handle`
- Capabilities: `m2sdr_get_capabilities`
- Control: `m2sdr_set_bitmode`, `m2sdr_set_dma_loopback`
- RF: `m2sdr_config_init`, `m2sdr_apply_config`, `m2sdr_apply_config_if_needed`, `m2sdr_set_rx_frequency`, `m2sdr_set_tx_frequency`, `m2sdr_set_sample_rate`, `m2sdr_set_bandwidth`, `m2sdr_set_rx_gain`, `m2sdr_set_rx_gain_mode`, `m2sdr_set_rx_gain_mode_all`, `m2sdr_set_agc_pin`, `m2sdr_set_tx_att`
  - `m2sdr_set_tx_att` uses positive-dB TX attenuation.
- AGC monitor: `m2sdr_configure_agc_counter`, `m2sdr_clear_agc_counter`, `m2sdr_get_agc_count`
- Streaming: `m2sdr_stream_config_init`, `m2sdr_stream_configure`, `m2sdr_sync_rx`, `m2sdr_sync_tx`, `m2sdr_get_buffer`, `m2sdr_try_get_buffer`, `m2sdr_submit_buffer`, `m2sdr_release_buffer`
- Stream diagnostics: `m2sdr_get_stream_stats`, `m2sdr_clear_stream_stats`
- Time: `m2sdr_get_time`, `m2sdr_set_time`, `m2sdr_get_ptp_status`, `m2sdr_get_ptp_discipline_config`, `m2sdr_set_ptp_discipline_config`, `m2sdr_clear_ptp_counters`
- Sensors: `m2sdr_get_fpga_dna`, `m2sdr_get_fpga_sensors`

## Error model

Library calls return `0` on success and negative status codes on failure.
In addition to generic `M2SDR_ERR_INVAL`/`M2SDR_ERR_IO` classes, the API now
uses:

- `M2SDR_ERR_PARSE` for malformed strings/identifiers.
- `M2SDR_ERR_RANGE` for out-of-range numeric values.
- `M2SDR_ERR_STATE` for invalid call sequencing (for example, streaming before configuration).
  On Ethernet PTP-disciplined builds, `m2sdr_set_time()` also returns this when PTP already owns the board clock.

Use `m2sdr_strerror()` for concise error text in logs.

## Library versioning

- `libm2sdr` public API version: `1.1.0`
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

- Streaming supports SC16/Q11, SC8/Q7, and encoded BFP8/Q11.
- `m2sdr_get_stream_stats()` reports backend stream counters. On PCIe it exposes
  DMA ring level/high-water and overflow/underflow counters; on LiteEth it
  mirrors UDP RX/TX counters and drop/recovery diagnostics.
- `m2sdr_get_ptp_status()` reports the LiteEth PTP lock state, current discipline mode, local/master port identity, tolerated lock-window misses, lock-loss counters, and protocol/debug counters when the FPGA bitstream was built with `--with-eth --with-eth-ptp`.
- `m2sdr_get_ptp_discipline_config()` / `m2sdr_set_ptp_discipline_config()` expose the runtime servo controls used by `m2sdr_util ptp-config`, including the consecutive `unlock_misses` threshold used to deglitch time-lock loss reporting and `coarse_confirm` used to allow confirmed runtime coarse realignments. The default `unlock_misses=64` avoids reporting short software-timestamp jitter bursts as lock drops. The default `coarse_confirm=0` disables runtime coarse rewrites after initial acquisition; near-one-second runtime coarse errors are always treated as TSU excursions and are not copied into the board clock.
- `m2sdr_clear_ptp_counters()` clears the board-side discipline and identity counters. LiteEth protocol counters remain read-only in this first implementation.
- Read-side time APIs continue to operate on the same logical board clock regardless of whether it is free-running, manually set, or disciplined from Ethernet PTP.
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
