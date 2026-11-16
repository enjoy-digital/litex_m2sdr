#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

from litex.soc.integration.builder import Builder


def _load_script():
    path = Path(__file__).resolve().parents[1] / "scripts" / "test_pcie_ltssm.py"
    spec = importlib.util.spec_from_file_location("test_pcie_ltssm", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_soc_module():
    path = Path(__file__).resolve().parents[1] / "litex_m2sdr.py"
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_ltssm_tracer_register_matches_a_pcie_probe_build(tmp_path, monkeypatch):
    # The script must find the register under the name a --with-pcie-probe build exports.
    script  = _load_script()
    soc_mod = _load_soc_module()
    monkeypatch.chdir(tmp_path)
    (tmp_path / "test").mkdir() # LiteScope analyzer CSV of add_pcie_probe().

    soc = soc_mod.BaseSoC(variant="m2", with_pcie=True, with_jtagbone=False)
    soc.add_pcie_probe()
    builder = Builder(soc, output_dir=str(tmp_path / "build"), csr_csv=str(tmp_path / "csr.csv"),
        compile_software=False, compile_gateware=False)
    builder.build(run=False)

    names = [line.split(",")[1] for line in (tmp_path / "csr.csv").read_text().splitlines()
        if line.startswith("csr_register,")]
    regs  = SimpleNamespace(**{name: name for name in names})
    assert script.get_ltssm_tracer_history(regs) in script.LTSSM_TRACER_HISTORY_NAMES


def test_ltssm_tracer_register_lookup_accepts_both_names_and_reports_absence():
    script = _load_script()

    for name in script.LTSSM_TRACER_HISTORY_NAMES:
        assert script.get_ltssm_tracer_history(SimpleNamespace(**{name: "csr"})) == "csr"
    with pytest.raises(ValueError, match="--with-pcie-probe"):
        script.get_ltssm_tracer_history(SimpleNamespace())
