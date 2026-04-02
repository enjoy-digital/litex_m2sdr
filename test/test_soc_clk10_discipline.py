#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
from pathlib import Path

from litex.soc.integration.builder import Builder


def _load_soc_module():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_soc_build_exposes_clk10_discipline_csrs(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        with_pcie=False,
        with_jtagbone=False,
    )
    builder = Builder(
        soc,
        output_dir=tmp_path / "build",
        compile_software=False,
        csr_csv=str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)

    csr_csv = (tmp_path / "csr.csv").read_text()
    assert "csr_register,clk10_discipline_control" in csr_csv
    assert "csr_register,clk10_discipline_rate" in csr_csv
    assert "csr_register,clk10_discipline_update_cycles" in csr_csv
    assert "csr_register,clk10_discipline_dropped_count" in csr_csv
