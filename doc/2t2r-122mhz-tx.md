# Full-Duplex 2T2R at 122.88 MSPS

LiteX-M2SDR can run the AD9361 in full 2T2R (two RX + two TX channels) at 122.88 MSPS.
RX at this rate was already supported (wide-bandwidth / oversampling mode); this note covers
the **TX** side, which needs the OSERDESE2 serializer and a bring-up reliability step.

## The problem

At 2T2R the LVDS interface carries both channels interleaved, so the doubled `DATA_CLK` is
**491.52 MHz** and each TX data lane runs at **983 Mbps** (DDR). Driving the lanes from
fabric-clocked `ODDR`s produces an output eye that is too poor at 983 Mbps for the AD9361 to
de-interleave the 8-way 2R2T frame: a clean single-channel tone splatters to `fs/2`.

This is *not* a chip limit (1T1R@122.88 TX works, but its LVDS runs at 245.76 MHz / 491 Mbps),
and it is *not* lane-to-lane skew (per-lane clock phasing does not recover it). It is the ODDR
output eye quality at 983 Mbps.

## The fix: OSERDESE2 TX serializer

When `--with-rfic-oversampling` builds set `M2SDR_TX_OSERDES`, the six TX data lanes and TX_FRAME
are driven by **OSERDESE2** (8:1 DDR) instead of ODDRs:

- `CLK = 491.52 MHz` (DDR bit clock → 983 Mbps), `CLKDIV = 122.88 MHz` (8-bit parallel load),
  both from a dedicated MMCM off the recovered `DATA_CLK`.
- Per `CLKDIV` cycle each lane is loaded with the 8 half-cycle bits of one 2R2T sample group;
  the D1..D8 order is bit-identical to what the ODDR serializer emitted (see `gateware/ad9361/phy.py`).
- The serializer runs the 983 Mbps in the IOB while the datapath fabric runs at 122.88 MHz.

OSERDESE2 gives a clean 983 Mbps eye and the tone recovers (−0.1 dB tone/total on an analog
loopback, matching the 61.44 reference), for both SC8 and SC16 host formats.

Constraints of this build (`M2SDR_TX_OSERDES`):

- Mutually exclusive with the `tx_phase` MMCM path (both build the TX MMCM).
- The OSERDES MMCM only locks at the 491.52 `DATA_CLK`, so **TX is supported only at 2T2R@122.88**
  in an OSERDES build (RX works at all rates; 1T1R / 61.44 TX need a non-OSERDES build).

## Bring-up reliability (driver)

The AD9361's 8-way 2R2T TX de-interleave locks to one of several phases at each bring-up; about
20 % land wrong. The driver (`m2sdr_wide_bandwidth_bringup` in `software/user/libm2sdr/m2sdr_rf.c`)
makes this transparent to the application:

1. **Detect** the lock cable-free: run the FPGA PRBS through the AD9361 data-port loop test — a
   framing slip breaks the PRBS *sequence*, so the per-lane RX-deskew error counters go non-zero
   (`sum == 0` ⇔ good). The loop test only disturbs the RX capture (re-deskewed after) and preserves
   the TX lock; a single check does not corrupt a good lock.
2. **Re-roll** a wrong lock: only a full SPI re-init (`ad9361_init` with the GPIO reset masked, the
   same path a runtime layout switch uses) re-establishes the data-port framing — re-programming the
   clocks in place does not. On a wrong lock the bring-up re-runs the configure/bring-up sequence,
   bounded to 12 attempts.

Result: 2T2R@122.88 TX comes up clean on every `m2sdr_apply_config()` (typically 0–1 re-inits,
at most 4 observed). 1T1R and 2T2R@61.44 are unaffected (the check is gated on 2R2T-wide).

## Building and using

```sh
# Gateware (OSERDES TX build):
M2SDR_TX_OSERDES=1 python litex_m2sdr.py --variant=m2 --with-pcie --pcie-gen=2 \
    --pcie-lanes=4 --with-rfic-oversampling --build
# flash via scripts/flash_gateware.sh, then rebuild the user software against the build's csr.h.
```

The application just requests `sample_rate = 122.88e6` with the 2T2R layout; the driver handles the
OSERDES timing and the bring-up retry.

## Verification

- OSERDES D1..D8 bit-mapping is unit-test-proven bit-identical to the ODDR serializer.
- Gateware closes timing at 491.52 MHz (WNS ≈ +0.016 ns).
- On the analog TX→RX loopback: 2T2R@122.88 ch2 tone recovers at −0.1 dB (SC8 and SC16), reliably
  across fresh bring-ups.
