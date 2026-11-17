#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import pytest

from litex_m2sdr.gateware.dfx import DEFAULT_DFX_PBLOCK, validate_dfx_pblock


def test_dfx_default_pblock_is_valid():
    assert validate_dfx_pblock(DEFAULT_DFX_PBLOCK) == DEFAULT_DFX_PBLOCK


@pytest.mark.parametrize("pblock", [
    "",
    "SLICE_X150Y100",
    "DSP48_X0Y0:DSP48_X1Y1",
    "SLICE_X150Y100:SLICE_X159Y149 RAMB18_X0Y0:RAMB18_X0Y1",
    "{SLICE_X150Y100:SLICE_X159Y149}",
    "SLICE_X159Y149:SLICE_X150Y100",
])
def test_dfx_rejects_unsupported_pblock_syntax(pblock):
    with pytest.raises(ValueError):
        validate_dfx_pblock(pblock)


def _load_soc_module():
    import importlib.util
    from pathlib import Path
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("lanes, oversampling, width", [(1, False, 64), (4, True, 128)])
def test_dfx_region_replaces_the_rfic_hook_at_the_datapath_width(lanes, oversampling, width):
    from litex_m2sdr.gateware.dfx import DFX_CELL_NAME, DFXRFICDatapath
    from litex_m2sdr.gateware.loopback import TXRXLoopback

    soc = _load_soc_module().BaseSoC(variant="m2", with_pcie=True, pcie_lanes=lanes,
        with_rfic_oversampling=oversampling, with_dfx=True)
    assert isinstance(soc.rfic_dfx, DFXRFICDatapath)
    assert isinstance(soc.txrx_loopback, TXRXLoopback)   # Host loopback stays static.
    for name in ("tx_in", "tx_out", "rx_in", "rx_out"):   # Registered partition boundary.
        assert hasattr(getattr(soc.rfic_dfx, name), "pipe_ready")
    (region,) = [s for s in soc.rfic_dfx._fragment.specials
        if getattr(s, "name_override", None) == DFX_CELL_NAME]
    params = {item.name: item.value.value for item in region.items
        if type(item).__name__ == "Parameter"}
    assert params == {"DATA_WIDTH": width}
    commands = "\n".join(soc.platform.toolchain.pre_optimize_commands +
        soc.platform.toolchain.additional_commands)
    assert DEFAULT_DFX_PBLOCK in commands
    assert f"-generic DATA_WIDTH={width}" in commands   # Alternate RM matches the static width.
