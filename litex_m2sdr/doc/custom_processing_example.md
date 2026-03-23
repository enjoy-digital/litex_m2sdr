# Custom Processing Example

This repository now includes a minimal example showing how to insert a client AXI-Stream processing block between the DMA-side datapath and the AD9361 RFIC.

## Files

- `litex_m2sdr/gateware/verilog/custom_processing_passthrough.v`
  Dummy Verilog IP with independent TX and RX AXI-Stream paths.
- `litex_m2sdr/gateware/custom_processing.py`
  LiteX wrapper exposing the block as TX/RX `stream.Endpoint`s.
- `litex_m2sdr.py`
  Optional integration point in the normal RF datapath.

## Datapath Placement

When enabled, the example is inserted here:

```text
DMA / Comms -> Header -> Loopback -> Custom Processing -> AD9361 TX
DMA / Comms <- Header <- Loopback <- Custom Processing <- AD9361 RX
```

The example is only inserted in the normal RF path. Existing loopback behavior is preserved.

## Build

Enable the example with:

```bash
python3 litex_m2sdr.py --with-pcie --with-custom-processing-example
```

Add `--build`, `--load`, `--flash`, or the usual project-specific options as needed.

## Client Adaptation

The intended workflow for a client is:

1. Replace the passthrough logic in `custom_processing_passthrough.v` with their own AXI-Stream processing.
2. Keep the LiteX wrapper interface unchanged if the client module still uses one AXI-Stream TX input/output pair and one AXI-Stream RX input/output pair.
3. Extend `custom_processing.py` if additional sideband signals or control/status registers are needed.

## Interface Notes

- `tdata`, `tvalid`, `tready`, and `tlast` are passed through the Verilog module.
- LiteX's `first` qualifier is forwarded around the Verilog instance in the Python wrapper, since AXI-Stream does not define `tfirst`.
