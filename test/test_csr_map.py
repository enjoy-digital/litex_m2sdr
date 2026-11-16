#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import csv
import importlib.util
import re
from pathlib import Path

import pytest

from litex.soc.integration.builder import Builder

ROOT  = Path(__file__).resolve().parents[1]
CSR_H = ROOT / "litex_m2sdr" / "software" / "kernel" / "csr.h"

# Helpers ------------------------------------------------------------------------------------------

def _load_soc_module():
    spec   = importlib.util.spec_from_file_location("litex_m2sdr_soc", ROOT / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _csr_h_addresses():
    addresses = {}
    for line in CSR_H.read_text().splitlines():
        m = re.match(r"#define (CSR_\w+)_ADDR (0x[0-9a-fA-F]+)L", line)
        if m:
            addresses[m.group(1)] = int(m.group(2), 0)
    return addresses


def _soc_csr_addresses(soc, tmp_path):
    builder = Builder(soc,
        output_dir       = tmp_path / "build",
        compile_software = False,
        csr_csv          = str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)
    addresses = {}
    with open(tmp_path / "csr.csv") as f:
        for row in csv.reader(f):
            if row and row[0] == "csr_register":
                addresses["CSR_" + row[1].upper()] = int(row[2], 0)
    return addresses

# Tests --------------------------------------------------------------------------------------------

# The host software is built against the committed csr.h (a superset of the gateware variants):
# every CSR it defines must keep its address in the generated SoCs.
@pytest.mark.parametrize("kwargs", [
    dict(variant="m2",        with_pcie=True, pcie_lanes=4),
    dict(variant="baseboard", with_pcie=False, with_eth=True, eth_sfp=0),
], ids=["m2_pcie_x4", "baseboard_eth"])
def test_soc_csr_addresses_match_csr_h(tmp_path, kwargs):
    soc_mod  = _load_soc_module()
    soc      = soc_mod.BaseSoC(with_jtagbone=False, **kwargs)
    soc_csrs = _soc_csr_addresses(soc, tmp_path)
    csr_h    = _csr_h_addresses()

    common = sorted(set(soc_csrs) & set(csr_h))
    assert any(name.startswith("CSR_AD9361_SPI_") for name in common)
    mismatches = [(name, hex(soc_csrs[name]), hex(csr_h[name]))
        for name in common if soc_csrs[name] != csr_h[name]]
    assert mismatches == []
