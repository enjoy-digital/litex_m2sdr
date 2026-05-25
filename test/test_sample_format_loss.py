#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
from pathlib import Path
from types import SimpleNamespace


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "evaluate_sample_formats.py"
spec = importlib.util.spec_from_file_location("evaluate_sample_formats", SCRIPT)
fmt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fmt)


def rows_by_format(**kwargs):
    args = SimpleNamespace(
        samples=8192,
        cycles=37.25,
        amplitudes=[0.0, -20.0],
        block_complex_samples=kwargs.get("block_complex_samples", 254),
        header_bytes=kwargs.get("header_bytes", 8),
        channels=kwargs.get("channels", 2),
    )
    return {row["format"]: row for row in fmt.format_rows(args)}


def test_sc8_rounding_improves_over_truncation():
    rows = rows_by_format()
    gain = rows["SC8 rounded"]["snr_0dbfs"] - rows["SC8 trunc"]["snr_0dbfs"]
    assert gain > 4.0


def test_bfp8_preserves_low_level_tones_better_than_fixed_sc8():
    rows = rows_by_format()
    gain = rows["BFP8"]["snr_-20dbfs"] - rows["SC8 rounded"]["snr_-20dbfs"]
    assert gain > 15.0


def test_bfp_header_overhead_is_accounted_per_complex_sample():
    rows = rows_by_format(block_complex_samples=254, header_bytes=8)
    assert rows["BFP8"]["bytes_per_complex"] == 2 + 8 / (254 * 2)
