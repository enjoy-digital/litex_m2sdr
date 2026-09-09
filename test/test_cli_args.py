#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import sys
import subprocess
import importlib.util
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


def test_eth_phy_kwargs_policy():
    soc_mod = _load_soc_module()

    assert soc_mod.get_eth_phy_kwargs("1000basex") == {}
    assert soc_mod.get_eth_phy_kwargs("2500basex") == {
        "tx_cm_type"       : "MMCM",
        "rx_cm_type"       : "MMCM",
        "pcs_kwargs"       : {"eth_tx_clk_freq": 125e6},
        "with_pcs_buffers" : True,
    }


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


def test_main_selects_2500basex_and_raw_rx_probe(monkeypatch):
    soc_mod = _load_soc_module()
    captured = {}

    class FakeSoC:
        def __init__(self, **kwargs):
            captured["kwargs"] = kwargs

        def add_eth_phy_rx_probe(self):
            captured["eth_phy_rx_probe"] = True

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
        sys,
        "argv",
        [
            "litex_m2sdr.py",
            "--variant=baseboard",
            "--with-eth",
            "--eth-sfp=0",
            "--eth-phy=2500basex",
            "--with-eth-phy-rx-probe",
        ],
    )

    soc_mod.main()

    assert captured["kwargs"]["eth_phy"] == "2500basex"
    assert captured["kwargs"]["sys_clk_freq"] == 125_000_000
    assert captured["eth_phy_rx_probe"] is True
    assert captured["build_name"] == "litex_m2sdr_baseboard_eth_2500basex"
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


def test_main_wr_status_uses_lazy_wr_integration(monkeypatch):
    soc_mod = _load_soc_module()
    captured = {}

    def fake_loader(root_dir, wr_nic_dir):
        captured["loader_root_dir"] = root_dir
        captured["loader_wr_nic_dir"] = wr_nic_dir

        def fake_prepare(**kwargs):
            captured["prepare_kwargs"] = kwargs
            return {
                "wr_firmware": kwargs["wr_firmware"],
                "wr_sfp": kwargs["wr_sfp"],
                "wr_nic_dir": kwargs["wr_nic_dir"],
            }

        return fake_prepare

    monkeypatch.setattr(soc_mod, "_load_prepare_wr_environment", fake_loader)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "litex_m2sdr.py",
            "--variant=baseboard",
            "--wr-status",
            "--wr-nic-dir=/tmp/litex_wr_nic",
        ],
    )

    soc_mod.main()

    assert captured["loader_wr_nic_dir"] == "/tmp/litex_wr_nic"
    assert captured["prepare_kwargs"]["status"] is True
    assert captured["prepare_kwargs"]["with_white_rabbit"] is False


@pytest.mark.parametrize("options,cpu,memory,boot,suffix", [
    ([], "urv", "private", "embedded", ""),
    (["--wr-cpu-type=vexriscv", "--wr-cpu-memory=integrated"],
        "vexriscv", "integrated", "embedded", "_vexriscv_integrated"),
    (["--wr-cpu-type=vexriscv", "--wr-cpu-variant=lite", "--wr-cpu-memory=integrated", "--wr-cpu-boot=host"],
        "vexriscv", "integrated", "host", "_vexriscv_lite_integrated_host"),
])
def test_main_wr_cpu_configuration_and_distinct_build_names(monkeypatch, options, cpu, memory, boot, suffix):
    soc_mod  = _load_soc_module()
    captured = {}

    class FakeSoC:
        def __init__(self, **kwargs):
            captured["soc"] = kwargs

    class FakeBuilder:
        def __init__(self, soc, **kwargs):
            self.gateware_dir = "build/fake/gateware"

        def build(self, build_name, run):
            captured["build_name"] = build_name

    def prepare(**kwargs):
        captured["prepare"] = kwargs
        return dict(wr_nic_dir="/tmp/wr", wr_firmware="/tmp/wr/firmware.bram", wr_sfp=0)

    monkeypatch.setattr(soc_mod, "BaseSoC", FakeSoC)
    monkeypatch.setattr(soc_mod, "Builder", FakeBuilder)
    monkeypatch.setattr(soc_mod, "_load_prepare_wr_environment", lambda *args: prepare)
    monkeypatch.setattr(soc_mod, "generate_litepcie_software", lambda *args, **kwargs: None)
    monkeypatch.setattr(sys, "argv", [
        "litex_m2sdr.py", "--variant=baseboard", "--with-white-rabbit", *options,
    ])
    soc_mod.main()
    for name in ("soc", "prepare"):
        assert captured[name]["wr_cpu_type"] == cpu
        assert captured[name]["wr_cpu_memory"] == memory
    assert captured["soc"]["wr_cpu_boot"] == boot
    assert captured["soc"]["wr_cpu_variant"] == captured["prepare"]["wr_cpu_variant"]
    assert captured["build_name"] == "litex_m2sdr_baseboard_white_rabbit" + suffix


@pytest.mark.parametrize("package_path", [False, True])
def test_wr_loader_prefers_explicit_checkout_over_sibling(tmp_path, package_path):
    root    = Path(__file__).resolve().parents[1]
    project = tmp_path / "m2sdr"
    project.mkdir()
    for name in ("selected", "litex_wr_nic"):
        package = tmp_path / name / "litex_wr_nic"
        package.mkdir(parents=True)
        (package / "__init__.py").touch()
        (package / "integration.py").write_text(
            f"def prepare_wr_environment():\n    return {name!r}\n", encoding="utf-8")
    selected = tmp_path / "selected"
    if package_path:
        selected /= "litex_wr_nic"
    # A fresh interpreter prevents prior WR tests' cached imports from masking
    # which checkout the command-line loader actually selects.
    script = """
import importlib.util
import sys
spec = importlib.util.spec_from_file_location("m2sdr_soc", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
prepare = module._load_prepare_wr_environment(sys.argv[2], sys.argv[3])
assert prepare() == "selected"
"""
    subprocess.run([sys.executable, "-c", script, str(root / "litex_m2sdr.py"),
        str(project), str(selected)], check=True)


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
