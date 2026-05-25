# RFIC Transport Format Notes

LiteX-M2SDR keeps the AD9361 LVDS interface in its native 12-bit two's-complement format. The 8-bit mode is a transport packing mode around the host/FPGA stream path.

## Current Recommendation

- Use SC16/Q11 when link bandwidth is available.
- Use rounded SC8/Q7 for the current Ethernet bandwidth-saving mode.
- Evaluate BFP8 as the next transport mode when weak-signal quality matters more than the small metadata overhead.
- Treat BFP4 as a monitoring-only option unless a specific application can tolerate large quantization loss.

## BFP8 Model

The initial BFP8 model uses signed int8 mantissas with one shared power-of-two exponent per block:

- Input samples are the AD9361's 12-bit Q11 values.
- The exponent is selected from the peak absolute value in the block.
- The exponent is capped to the source/mantissa width delta, so BFP8 does not get worse than fixed SC8 at full scale.
- Decoding reconstructs `sample_q11 = mantissa << exponent`.

The model is implemented in `scripts/evaluate_sample_formats.py`. The default overhead assumes an 8-byte stream header per 256 complex samples, so BFP8 is about 2.031 bytes per complex sample per channel instead of exactly 2.000 for SC8.

## Integration Constraint

True BFP transport needs metadata. The current libm2sdr streaming API treats formats as fixed bytes-per-complex-sample values, which works for SC16 and SC8 but not for BFP blocks with headers. A production BFP8 path should first define one of these wire/API choices:

- In-band block headers in the stream, with sync/recovery rules and adjusted buffer sizing.
- A metadata sideband tied to existing stream headers.
- A fixed block framing API where callers pass encoded blocks rather than plain complex samples.

The simulator should be used to choose block size and header overhead before adding the public format enum.
