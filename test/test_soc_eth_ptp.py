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


def test_eth_ptp_soc_build_exposes_extended_ptp_csrs(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=False,
        with_eth=True,
        with_eth_ptp=True,
        eth_sfp=0,
        with_jtagbone=False,
    )
    assert soc.ethcore_etherbone.igmp.dw == soc.ethcore_etherbone.ip.crossbar.master.dw == 32

    builder = Builder(
        soc,
        output_dir=tmp_path / "build",
        compile_software=False,
        csr_csv=str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)

    csr_csv = (tmp_path / "csr.csv").read_text()
    assert "csr_register,ptp_identity_local_clock_id" in csr_csv
    assert "csr_register,ptp_identity_master_clock_id" in csr_csv
    assert "csr_register,ptp_discipline_update_cycles" in csr_csv
    assert "csr_register,ptp_discipline_phase_step_shift" in csr_csv
    assert "csr_register,ptp_discipline_ptp_lock_losses" in csr_csv
    assert "csr_register,ptp_identity_master_port_number" in csr_csv


def test_eth_ptp_rfic_clock_soc_build_exposes_clk10_servo_csrs(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(
        variant="baseboard",
        with_pcie=False,
        with_eth=True,
        with_eth_ptp=True,
        with_eth_ptp_rfic_clock=True,
        eth_sfp=0,
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
    assert "csr_register,ptp_clk10_discipline_control" in csr_csv
    assert "csr_register,ptp_clk10_discipline_last_error_ns" in csr_csv
    assert "csr_register,ptp_clk10_discipline_i_gain" in csr_csv
    assert "csr_register,ptp_clk10_discipline_rate_update_count" in csr_csv
    assert "csr_register,clk_measurement_clk5_value" in csr_csv
