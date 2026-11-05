#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
from pathlib import Path

import pytest

from litex.soc.integration.builder import Builder

pytest.importorskip("usb3_core.wishbone", reason="USB3 requires USB3-PIPE with the EP0 Wishbone bridge.")


def _load_soc_module():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _build_csr_csv(soc, tmp_path):
    builder = Builder(
        soc,
        output_dir=tmp_path / "build",
        compile_software=False,
        csr_csv=str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)
    return (tmp_path / "csr.csv").read_text()


def test_usb3_soc_build_exposes_usb3_csrs(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=False,
        with_usb3=True,
        usb3_sfp=1,
        with_jtagbone=False,
    )

    assert soc.qpll.channel_map == {"usb3": 0}
    assert "usb3" in soc.bus.masters
    assert len(soc.crossbar.mux.sink3.data) == 64

    csr_csv = _build_csr_csv(soc, tmp_path)
    assert "csr_base,usb3,0x0000b000" in csr_csv
    assert "csr_register,usb3_control" in csr_csv
    assert "csr_register,usb3_status" in csr_csv
    assert "csr_register,crossbar_mux_sel" in csr_csv
    assert "csr_register,crossbar_demux_sel" in csr_csv
    assert "constant,config_clock_frequency,125000000" in csr_csv


def test_pcie_usb3_soc_shares_qpll():
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=True,
        with_usb3=True,
        with_jtagbone=False,
    )

    assert soc.qpll.channel_map == {"pcie": 0, "usb3": 1}
    assert soc.usb3.serdes.pll.channel == 1
    assert soc.usb3.serdes.pll.config == {"linerate": 5e9, "d": 1, "clkin": 100e6}
    assert hasattr(soc, "pcie_dma0")


@pytest.mark.parametrize("kwargs, message", [
    ({"variant": "m2"},                                   "baseboard"),
    ({"variant": "baseboard", "with_eth": True},          "cannot be combined"),
    ({"variant": "baseboard", "with_sata": True},         "cannot be combined"),
    ({"variant": "baseboard", "with_rfic_oversampling": True}, "oversampling"),
    ({"variant": "baseboard", "sys_clk_freq": int(100e6)}, "125MHz"),
])
def test_usb3_soc_rejects_unsupported_combinations(kwargs, message):
    soc_mod = _load_soc_module()
    with pytest.raises(ValueError, match=message):
        soc_mod.BaseSoC(with_pcie=False, with_usb3=True, with_jtagbone=False, **kwargs)
