# Hardware Timed TX

A hardware gate in the TX datapath gives deterministic on-air transmit timing (e.g. for 5G
uplink): each TX buffer's DMA header carries an air-time, and the FPGA holds the buffer until
board time reaches it, so a written timestamp *is* the on-air time (10 ns grid). Untimed buffers
(timestamp `0`) transmit immediately — continuous streaming is unaffected and both can be mixed
freely. A buffer that reaches the gate after its air-time is dropped whole (the RFIC airs zeros
for its duration) and counted as a TX underflow, rather than airing late.

## Calibrating `tx_offset`

Stamp a timed burst's first buffer with its air-time; the rest of the burst streams contiguously
behind it. Calibrate the fixed pipeline delay once over a TX→RX loopback — `scripts/timed_tx_selftest`
prints the `tx_offset` to use. It is on the order of a microsecond and depends on the sample rate
and channel layout (e.g. 1T1R ≈ 1358 ns at 30.72 MSPS falling to ≈ 373 ns at 122.88; 2T2R ≈ 1215 ns
at 30.72), so measure it for your config.

## libm2sdr
```c
m2sdr_set_tx_header(dev, true);          /* REQUIRED: enable per-buffer air-time headers for the gate */
m2sdr_set_tx_offset(dev, 1212);          /* ns, from timed_tx_selftest */
struct m2sdr_metadata m = { .timestamp = air_time_ns, .flags = M2SDR_META_FLAG_HAS_TIME };
m2sdr_sync_tx(dev, buf, n, &m, timeout_ms);   /* returns M2SDR_ERR_STATE if the header is not enabled */
uint32_t uf; m2sdr_get_tx_underflow(dev, &uf);   /* frames that missed their air-time */
```

## SoapySDR

The standard SoapySDR timed-TX contract applies, no code change:

```
driver=LiteXM2SDR,tx_offset=1212         # ns; omit to auto-derive from sample rate/layout
```

then call `writeStream()` with `SOAPY_SDR_HAS_TIME` and `timeNs`. The gate engages automatically on
gateware that has it (`timed_tx=auto`, the default) and falls back to the software timeline on
gateware that doesn't; `timed_tx=software` forces the fallback.

## Timestamp Model And Compatibility

TX and RX timestamps share the same FPGA time counter and header format; `tx_offset` (and its RX
mirror `m2sdr_set_rx_offset()` / the `rx_offset` Soapy arg) refer both to a common reference
plane, so "received at T, transmit at T+D" is exact arithmetic on one clock. The gate's CSRs are
appended at the end of the HEADER block and its presence is advertised through the capability
features CSR, so all pre-existing register addresses are unchanged and older software keeps
working against this gateware (and vice versa).
