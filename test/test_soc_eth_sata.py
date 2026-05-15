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


def test_eth_sata_soc_build_exposes_both_transport_csrs(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=False,
        with_eth=True,
        eth_sfp=0,
        with_sata=True,
        with_jtagbone=False,
    )

    assert soc.qpll.channel_map == {"eth": 0, "sata": 1}
    assert hasattr(soc, "eth_rx_streamer")
    assert hasattr(soc, "eth_tx_streamer")
    assert hasattr(soc, "sata_phy")
    assert hasattr(soc, "sata_rx_streamer")
    assert hasattr(soc, "sata_tx_streamer")

    builder = Builder(
        soc,
        output_dir=tmp_path / "build",
        compile_software=False,
        csr_csv=str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)

    csr_csv = (tmp_path / "csr.csv").read_text()
    assert "csr_register,capability_features" in csr_csv
    assert "csr_register,crossbar_mux_sel" in csr_csv
    assert "csr_register,crossbar_demux_sel" in csr_csv
    assert "csr_register,eth_rx_streamer_enable" in csr_csv
    assert "csr_register,eth_tx_streamer_enable" in csr_csv
    assert "csr_register,sata_phy_enable" in csr_csv
    assert "csr_register,sata_rx_streamer_start" in csr_csv
    assert "csr_register,sata_tx_streamer_start" in csr_csv
