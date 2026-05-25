# RFIC Transport Format Notes

LiteX-M2SDR keeps the AD9361 LVDS interface in its native 12-bit two's-complement format. The 8-bit mode is a transport packing mode around the host/FPGA stream path.

## Current Recommendation

- Use SC16/Q11 when link bandwidth is available.
- Use rounded SC8/Q7 for the current Ethernet bandwidth-saving mode.
- Evaluate BFP8 as the next transport mode when weak-signal quality matters more than the small metadata overhead.

## BFP8 Model

The initial BFP8 model uses signed int8 mantissas with one shared power-of-two exponent per block:

- Input samples are the AD9361's 12-bit Q11 values.
- The exponent is selected from the peak absolute value in the block.
- The exponent is capped to the source/mantissa width delta, so BFP8 does not get worse than fixed SC8 at full scale.
- Decoding reconstructs `sample_q11 = mantissa << exponent`.

The implemented FPGA framing uses one 64-bit header word followed by 127 64-bit payload words. Each payload word carries two RFIC beats, so a BFP8 block covers 254 complex samples per channel and occupies exactly 1024 bytes. A normal 8192-byte DMA/UDP payload contains 8 BFP8 blocks. This makes BFP8 about 2.016 bytes per complex sample per channel instead of exactly 2.000 for SC8.

The model is implemented in `scripts/evaluate_sample_formats.py`.

## Integration Constraint

True BFP transport needs metadata. The initial API exposes BFP8 as an encoded block format: one public BFP8 "sample" is one 1024-byte BFP8 block, not one decoded complex sample. Higher-level tools that want normal complex samples should decode BFP8 blocks explicitly.

The current implementation deliberately avoids hiding this overhead behind the plain SC8/SC16 complex-sample API. That keeps record/play and low-level streaming able to move raw BFP8 blocks while leaving decoded CF32/CS16 integration to a later host-side codec layer. Since BFP8 block alignment is fixed, raw BFP8 record/play does not combine with the optional 16-byte DMA timestamp header.
