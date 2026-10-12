# Extending The Gateware With On-FPGA Processing

Back to the [documentation index](README.md).

Downstream projects can insert their own processing into the sample datapath — channelizers,
GNSS correlation, RX→TX repeating, DSP offload — **without forking `litex_m2sdr.py`**: import
`BaseSoC`, subclass it, and override the `add_rfic_datapath()` hook. The base class keeps
providing (and maintaining) the PCIe/Ethernet, clocking, SI5351, time and RF infrastructure.

This extension point distills two converging experiences: the hook-based approach proposed by
Soeren Schoenbrod for on-FPGA GNSS tracking ([#152](https://github.com/enjoy-digital/litex_m2sdr/pull/152)),
and the integration plane Enjoy-Digital has been using for on-FPGA DSP in private customer
projects on this board. The generic DSP building blocks themselves (DDS/NCO, mixers, CIC,
FIR, ...) are being gathered in [LiteDSP](https://github.com/enjoy-digital/litedsp), which this
hook is designed to host.

## The Hook

`add_rfic_datapath()` is called once during `BaseSoC.__init__` with the four stream Endpoints
(64-bit `dma_layout`, `sys` clock domain) surrounding the RFIC, at the **raw sample plane**: pure
I/Q words, no DMA-header beats (RX timestamping happens downstream in `header.rx`, so processed
samples are timestamped normally), on the RFIC side of the digital loopback (host loopback tests
keep running on the unprocessed path). The default implementation is a transparent passthrough,
so the stock design is unchanged.

```python
from litex_m2sdr import BaseSoC  # or import the script directly

class MySoC(BaseSoC):
    def add_rfic_datapath(self, rfic_sink, rfic_source, tx_source, rx_sink):
        # RX: RFIC -> DSP -> host (replace Buffer with your processing).
        self.dsp = stream.Buffer(dma_layout(64))
        self.comb += [
            tx_source.connect(rfic_sink),           # TX passthrough.
            rfic_source.connect(self.dsp.sink),     # RFIC RX -> DSP.
            self.dsp.source.connect(rx_sink),       # DSP -> host RX.
        ]

soc = MySoC(variant="m2", with_pcie=True, pcie_dmas=2)
```

Typical topologies:

- **Lossless tap / offload**: route `rfic_source` through (or tap it into) your block, return the
  samples to `rx_sink`; the host RX stream stays available alongside the processing.
- **RX→TX repeater**: feed `rfic_source` into the processing and connect its output to
  `rfic_sink`; `rx_sink` can carry an observation copy to the host.
- **Side-band results**: build with `pcie_dmas > 1` (`--pcie-dmas`) and drive `self.pcie_dma1`,
  ... from the hook (it runs after the comms sections) to stream correlator dumps or measurement
  records to the host without disturbing the DMA0 I/Q path.
