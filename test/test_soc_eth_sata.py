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
    assert not hasattr(soc, "dma_bus")

    host_buffer = soc.bus.regions["sata_host_buffer"]
    assert host_buffer.origin == soc_mod.SATA_HOST_BUFFER_BASE
    assert host_buffer.size == soc_mod.SATA_HOST_BUFFER_SIZE

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


def test_pcie_sata_soc_routes_dma_to_host_buffer_and_pcie():
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=True,
        with_eth=False,
        with_sata=True,
        with_jtagbone=False,
    )

    assert soc.qpll.channel_map == {"pcie": 0, "sata": 1}
    assert hasattr(soc, "dma_bus")
    assert hasattr(soc, "sata_dma_mem")

    assert sorted(soc.dma_bus.masters) == [
        "sata_mem2sector",
        "sata_sector2mem",
        "sata_sector2mem_fence",
    ]

    host_buffer = soc.bus.regions["sata_host_buffer"]
    assert host_buffer.origin == soc_mod.SATA_HOST_BUFFER_BASE
    assert host_buffer.size == soc_mod.SATA_HOST_BUFFER_SIZE

    dma_window = soc.dma_bus.regions["sata_dma_mem"]
    assert dma_window.origin == 0
    assert dma_window.size == 2**32
