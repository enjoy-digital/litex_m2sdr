#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
import re
from pathlib import Path

from litex.soc.integration.builder import Builder


def _load_soc_module():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module

# PCIe DMA Channels Tests --------------------------------------------------------------------------

def test_every_pcie_dma_synchronizer_gets_the_pps(tmp_path):
    """Validate each DMA channel synchronizer is driven by the PPS pulse of the board time."""
    soc_mod = _load_soc_module()
    soc     = soc_mod.BaseSoC(variant="m2", with_pcie=True, pcie_dmas=2, with_jtagbone=False)

    builder = Builder(
        soc,
        output_dir       = tmp_path / "build",
        compile_software = False,
        csr_csv          = str(tmp_path / "csr.csv"),
    )
    builder.build(build_name="pcie_dmas", run=False)

    csr_csv = (tmp_path / "csr.csv").read_text()
    assert "csr_register,pcie_dma0_synchronizer_enable" in csr_csv
    assert "csr_register,pcie_dma1_synchronizer_enable" in csr_csv

    # The kernel arms the synchronizer of the channel it starts and never bypasses it: a
    # synchronizer without the pulse never releases its channel.
    rtl = (tmp_path / "build" / "gateware" / "pcie_dmas.v").read_text()
    for n in range(2):
        assert re.search(rf"assign \w*dma{n}_synchronizer_pps = pps_gen_pps_pulse;", rtl), \
            f"DMA channel {n} synchronizer has no PPS."
