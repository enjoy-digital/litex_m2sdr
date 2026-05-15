#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import pytest

import release


TIMING_REPORT = """
------------------------------------------------------------------------------------------------
| Design Timing Summary
| ---------------------
------------------------------------------------------------------------------------------------

    WNS(ns)      TNS(ns)  TNS Failing Endpoints  TNS Total Endpoints      WHS(ns)      THS(ns)  THS Failing Endpoints  THS Total Endpoints     WPWS(ns)     TPWS(ns)  TPWS Failing Endpoints  TPWS Total Endpoints
    -------      -------  ---------------------  -------------------      -------      -------  ---------------------  -------------------     --------     --------  ----------------------  --------------------
{summary}
"""


def write_timing_report(tmp_path, build_name, summary):
    gateware_dir = tmp_path / build_name / "gateware"
    gateware_dir.mkdir(parents=True)
    report = gateware_dir / f"{build_name}_timing.rpt"
    report.write_text(TIMING_REPORT.format(summary=summary))
    return report


def test_release_timing_check_accepts_clean_setup_hold_with_pcie_pulse_width_warning(tmp_path):
    build_name = "litex_m2sdr_m2_pcie_x1"
    write_timing_report(
        tmp_path,
        build_name,
        "      0.180        0.000                      0                31557        "
        "0.006        0.000                      0                31557       "
        "-0.042       -0.042                       1                 10747",
    )

    summary = release.check_timing(tmp_path, build_name, allow_pcie_pulse_width_warning=True)

    assert summary["wns_ns"] == 0.180
    assert summary["tns_failing_endpoints"] == 0
    assert summary["ths_failing_endpoints"] == 0
    assert summary["pulse_width_warning_allowed"] is True


def test_release_timing_check_rejects_setup_failures(tmp_path):
    build_name = "litex_m2sdr_m2_pcie_x1"
    write_timing_report(
        tmp_path,
        build_name,
        "     -0.125       -1.250                      4                31557        "
        "0.006        0.000                      0                31557       "
        "0.000        0.000                       0                 10747",
    )

    with pytest.raises(SystemExit):
        release.check_timing(tmp_path, build_name, allow_pcie_pulse_width_warning=True)
