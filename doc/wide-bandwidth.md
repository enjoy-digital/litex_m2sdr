# Wide Analog Bandwidth (RFIC Overclock)

> [!NOTE]
> 2T2R at 122.88 MSPS needs gateware built with `--with-rfic-oversampling` and PCIe Gen2 x2/x4
> bandwidth; 1T1R works with the stock gateware.

The AD9361's analog baseband filters are normally limited to ~56 MHz. Requesting a sample rate above 61.44 MSPS (up to 122.88) selects the wide-bandwidth mode automatically:

```bash
m2sdr_rf --channel-layout 1t1r --sample-rate 122.88e6 ...
```

Converter half-band stages are bypassed so the data port runs at 2x rate, and the TX/RX baseband filters are re-tuned by the chip's own BBF calibration against a ~54/62 MHz corner target (programming the tune dividers past the driver's bandwidth clamp), opening a true ~100 MHz analog passband at 122.88 MSPS while keeping the calibrated noise behavior. The interface DATA_CLK follows the channel layout: 245.76 MHz in 1T1R (stock gateware) and 491.52 MHz in 2T2R (gateware built with `--with-rfic-oversampling`). The doubled-rate interface framing can come up misaligned, so the configuration PRBS-verifies the interface and retries the clock programming in place until it is aligned (typically 1-2 attempts).

## Measured Performance

Measured at 3.6 GHz on the internal reference (RX side: clean external transmitter -> M2SDR RX, 50 MHz NR-FR1-TM3.1 64QAM, MATLAB 5G Toolbox EVM; in-band SNR: notch/NPR method):

| Metric (wide mode, 122.88 MSPS)            | Measured                          |
|--------------------------------------------|-----------------------------------|
| Usable analog bandwidth                    | ~100 MHz (full quality to +/-40)  |
| RX EVM, 50 MHz TM3.1 centered              | ~6.0-6.6% RMS                     |
| RX EVM, 50 MHz TM3.1 shifted +/-25 MHz     | ~8.7-9.2% RMS (touches band edge) |
| RX in-band SNR (NPR), +/-10..31 MHz        | 26-29.5 dB                        |
| RX in-band SNR (NPR), +/-41 / +/-45 MHz    | ~21 / ~17 dB                      |
| RX passband flatness (with EQ FIR)         | +/-1.2 dB to +/-44 MHz            |
| No-signal RX floor (rx-gain 55)            | -35.6 dBFS integrated             |
| RX image rejection (quadrature tracking)   | ~40-43 dBc                        |
| TX EVM, 50 MHz TM3.1 (flat over tx-att 0-18) | ~5.2-6% RMS                     |
| TX image rejection (tx-att 0)              | ~44 dBc                           |
| TX passband flatness (chip TX FIR EQ)      | +/-1.3 dB to +/-42 MHz (from -8 dB) |

The RX EVM bottoms out with the ADC filled to ~-12 dBFS RMS at the lowest rx-gain that gets there (the band-edge noise floor is gain-independent). TX EVM is constant-dBc (drive-independent over tx-att 0-18). The remaining limit on both sides is LO phase noise from the reference chain (Si5351 multiplication x94 into the RFPLLs); the RX band-edge limit (beyond ~+/-40 MHz) is the converter's own shaped noise at this oversampling ratio - both are silicon/architecture limits, not configuration. TX passband flatness over the full 100 MHz is achieved in-chip with a 16-tap TX FIR equalizer (`M2SDR_OC_TX_FIR_FILE`; the taps must be designed for the FIR's 245.76 MHz processing rate in this mode), so real-time transmitters need no host-side pre-emphasis; waveform pre-emphasis remains available as an alternative. Best EVM is obtained on the internal clock reference; an external 10 MHz reference costs ~6% EVM through the Si5351's higher multiplication ratio (N=84 vs N=34).
## Tuning And Diagnostics

Tuning/diagnostic environment variables (defaults are the measured optimum): `M2SDR_OC_BBF_TUNE` (BBF corner target), `M2SDR_OC_BBF_FORCE` (legacy register force-widening), `M2SDR_OC_RX_FIR_FILE` / `M2SDR_OC_TX_FIR_FILE` (passband flatness EQ FIR taps, max 16; TX taps designed for 245.76 MHz), `M2SDR_QEC_KEXP` (RX quadrature tracking loop gain), `M2SDR_RFPLL_CP_PERCENT` (RX RFPLL charge pump scale), `M2SDR_OC_RX_DELAY`/`M2SDR_OC_TX_DELAY` (interface delay overrides).
