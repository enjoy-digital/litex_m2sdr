#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
import sys
from pathlib import Path

import pytest


def _load_soc_module():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_rfic_clk_freq_policy():
    soc_mod = _load_soc_module()

    assert soc_mod.get_rfic_clk_freq() == 245.76e6
    assert soc_mod.get_rfic_clk_freq(with_rfic_oversampling=True) == 491.52e6
    assert soc_mod.get_rfic_clk_freq(with_eth=True, eth_phy="1000basex") == 122.88e6
    assert soc_mod.get_rfic_clk_freq(with_eth=True, eth_phy="2500basex") == 245.76e6
    assert soc_mod.get_rfic_clk_freq(
        with_eth=True,
        eth_phy="1000basex",
        with_rfic_oversampling=True,
    ) == 122.88e6


def test_main_exposes_base_soc_optional_args(monkeypatch):
    soc_mod = _load_soc_module()
    captured = {}

    class FakeSoC:
        def __init__(self, **kwargs):
            captured["kwargs"] = kwargs

    class FakeBuilder:
        def __init__(self, soc, **kwargs):
            captured["builder_soc"] = soc
            captured.update(kwargs)
            self.gateware_dir = "build/fake/gateware"

        def build(self, build_name, run):
            captured["build_name"] = build_name
            captured["run"] = run

    monkeypatch.setattr(soc_mod, "BaseSoC", FakeSoC)
    monkeypatch.setattr(soc_mod, "Builder", FakeBuilder)
    monkeypatch.setattr(soc_mod, "generate_litepcie_software", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        soc_mod,
        "prepare_wr_environment",
        lambda **kwargs: {
            "wr_firmware": kwargs["wr_firmware"],
            "wr_sfp": kwargs["wr_sfp"],
            "wr_nic_dir": kwargs["wr_nic_dir"],
        },
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "litex_m2sdr.py",
            "--variant=baseboard",
            "--sys-clk-freq=100000000",
            "--without-jtagbone",
            "--with-rfic-oversampling",
        ],
    )

    soc_mod.main()

    assert captured["kwargs"]["variant"] == "baseboard"
    assert captured["kwargs"]["sys_clk_freq"] == 100000000
    assert captured["kwargs"]["with_jtagbone"] is False
    assert captured["kwargs"]["with_rfic_oversampling"] is True
    assert captured["build_name"] == "litex_m2sdr_baseboard_sysclk_100000000_rfic_oversampling_no_jtagbone"
    assert captured["output_dir"].endswith(captured["build_name"])
    assert captured["csr_csv"] == "scripts/csr.csv"
    assert captured["run"] is False


def test_main_defaults_ethernet_pcie_builds_to_100mhz_sysclk(monkeypatch):
    soc_mod = _load_soc_module()
    captured = {}

    class FakeSoC:
        def __init__(self, **kwargs):
            captured["kwargs"] = kwargs

    class FakeBuilder:
        def __init__(self, soc, **kwargs):
            captured["builder_soc"] = soc
            captured.update(kwargs)
            self.gateware_dir = "build/fake/gateware"

        def build(self, build_name, run):
            captured["build_name"] = build_name
            captured["run"] = run

    monkeypatch.setattr(soc_mod, "BaseSoC", FakeSoC)
    monkeypatch.setattr(soc_mod, "Builder", FakeBuilder)
    monkeypatch.setattr(soc_mod, "generate_litepcie_software", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        soc_mod,
        "prepare_wr_environment",
        lambda **kwargs: {
            "wr_firmware": kwargs["wr_firmware"],
            "wr_sfp": kwargs["wr_sfp"],
            "wr_nic_dir": kwargs["wr_nic_dir"],
        },
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "litex_m2sdr.py",
            "--variant=baseboard",
            "--with-pcie",
            "--pcie-lanes=1",
            "--with-eth",
            "--eth-sfp=0",
        ],
    )

    soc_mod.main()

    assert captured["kwargs"]["sys_clk_freq"] == 100000000
    assert captured["build_name"] == "litex_m2sdr_baseboard_pcie_x1_eth"
    assert captured["output_dir"].endswith(captured["build_name"])


def test_main_accepts_ethernet_sata_source_build(monkeypatch):
    soc_mod = _load_soc_module()
    captured = {}

    class FakeSoC:
        def __init__(self, **kwargs):
            captured["kwargs"] = kwargs

    class FakeBuilder:
        def __init__(self, soc, **kwargs):
            captured["builder_soc"] = soc
            captured.update(kwargs)
            self.gateware_dir = "build/fake/gateware"

        def build(self, build_name, run):
            captured["build_name"] = build_name
            captured["run"] = run

    monkeypatch.setattr(soc_mod, "BaseSoC", FakeSoC)
    monkeypatch.setattr(soc_mod, "Builder", FakeBuilder)
    monkeypatch.setattr(soc_mod, "generate_litepcie_software", lambda *args, **kwargs: None)
    monkeypatch.setattr(
        soc_mod,
        "prepare_wr_environment",
        lambda **kwargs: {
            "wr_firmware": kwargs["wr_firmware"],
            "wr_sfp": kwargs["wr_sfp"],
            "wr_nic_dir": kwargs["wr_nic_dir"],
        },
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "litex_m2sdr.py",
            "--variant=baseboard",
            "--with-eth",
            "--eth-sfp=0",
            "--with-sata",
        ],
    )

    soc_mod.main()

    assert captured["kwargs"]["variant"] == "baseboard"
    assert captured["kwargs"]["sys_clk_freq"] == 100000000
    assert captured["kwargs"]["with_pcie"] is False
    assert captured["kwargs"]["with_eth"] is True
    assert captured["kwargs"]["with_sata"] is True
    assert captured["build_name"] == "litex_m2sdr_baseboard_eth_sata"
    assert captured["output_dir"].endswith(captured["build_name"])
    assert captured["run"] is False


def test_base_soc_rejects_pcie_eth_sata_triple_use():
    soc_mod = _load_soc_module()

    with pytest.raises(ValueError, match="shared QPLL has two channels"):
        soc_mod.BaseSoC(
            variant="baseboard",
            with_pcie=True,
            with_eth=True,
            with_sata=True,
            with_jtagbone=False,
        )
