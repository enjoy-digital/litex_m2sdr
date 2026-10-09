# Low-Latency Streaming

Optional, opt-in knobs for real-time transmit/receive loops (e.g. 5G uplink). Defaults are
unchanged: stock builds keep the full-throughput behavior.

## 1. Shallow DMA Ring (opt-in at module load)

The default 256-buffer ring is tuned for
full-throughput streaming; a small ring lowers the TX pipeline-latency floor, which is the ring
drain time, `dma_buffer_count x 8192 B / (rate x bytes_per_sample)` -- e.g. at 30.72 MSPS 2T2R
(8 B/sample) that is ~8.5 ms for 256 buffers and ~0.27 ms for 8 (double these for 1T1R, which is
4 B/sample). A small ring buffers less host jitter, so the consumer must be real-time.

```bash
sudo insmod m2sdr.ko dma_buffer_count=8 dma_buffer_per_irq=2
sudo scripts/pin_m2sdr_irq.sh          # keep the DMA IRQ on your radio-loop core (re-run after each insmod)
```

## 2. RX Low-Latency Wake (opt-in)

With `M2SDR_RX_WAIT=mwaitx` on CPUs with MONITORX/MWAITX
(AMD), the zero-copy RX read sleeps on the next ring slot's cache line and wakes the instant the
FPGA's DMA write lands (sub-microsecond, no spinning, no CSR traffic). The default is `poll()`
everywhere. Independently, the RX wait consults the live DMA cursor just before blocking, so a
freshly captured buffer is delivered without waiting for the next coalesced interrupt.

## 3. TX Fill Lead (opt-in, zero-copy API)

`m2sdr_set_tx_lead_buffers(dev, 3)` holds the host a
tight lead ahead of the free-running DMA reader instead of filling the whole ring, trimming the TX
pipeline latency to ~lead x buffer air-time. The lead must stay strictly above the kernel's
`dma_buffer_per_irq` (see the API doc in `m2sdr.h`); `0` keeps the legacy full-ring fill.

## Real-Time Host Setup

With a shallow ring, run the radio thread on an isolated core (`isolcpus=`, `SCHED_FIFO`,
`mlockall`) on the same core as the pinned IRQ, and confirm **0 overflow / 0 underflow** under
load.
