#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
import re
from pathlib import Path

from litex.soc.integration.builder import Builder

from litex_m2sdr.software import generate_litepcie_software_headers

ROOT      = Path(__file__).resolve().parents[1]
LIBM2SDR  = ROOT / "litex_m2sdr" / "software" / "user" / "libm2sdr"

# Helpers ------------------------------------------------------------------------------------------

def _load_soc_module():
    spec   = importlib.util.spec_from_file_location("litex_m2sdr_soc", ROOT / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module

# Ethernet VRT SoC Tests ---------------------------------------------------------------------------

def test_eth_vrt_soc_rx_mode_register_is_known_to_libm2sdr(tmp_path):
    """Validate libm2sdr selects the Ethernet RX mode (raw UDP/VRT) with a register name that the
    csr.h of an Ethernet VRT build defines."""
    soc_mod = _load_soc_module()
    soc     = soc_mod.BaseSoC(
        variant       = "baseboard",
        with_pcie     = False,
        with_eth      = True,
        eth_sfp       = 0,
        with_eth_vrt  = True,
        with_jtagbone = False,
    )
    builder = Builder(
        soc,
        output_dir       = tmp_path / "build",
        compile_software = False,
        csr_csv          = str(tmp_path / "csr.csv"),
    )
    builder.build(run=False)

    csr_csv = (tmp_path / "csr.csv").read_text()
    assert "csr_register,main_eth_rx_mode," in csr_csv
    assert "csr_register,vrt_streamer_vrt_streamer_enable," in csr_csv

    # Generated csr.h of this build (same generation as the target script).
    generate_litepcie_software_headers(soc, str(tmp_path))
    csr_h   = (tmp_path / "csr.h").read_text()
    defined = set(re.findall(r"^#define (CSR_\w+) ", csr_h, flags=re.MULTILINE))
    assert "CSR_MAIN_ETH_RX_MODE_ADDR" in defined

    # Names libm2sdr can use for the register: at least one must be generated, else the VRT mode
    # always returns M2SDR_ERR_UNSUPPORTED and the raw UDP mode never restores the route.
    stream_c = (LIBM2SDR / "m2sdr_stream.c").read_text()
    used     = set(re.findall(r"\bCSR_\w*ETH_RX_MODE_ADDR\b", stream_c))
    assert used, "libm2sdr does not use the Ethernet RX mode register."
    assert used & defined, f"None of {sorted(used)} is defined by the csr.h of an Ethernet VRT build."

    # The VRT streamer registers used by libm2sdr are generated too.
    for name in sorted(set(re.findall(r"\bCSR_VRT_STREAMER_\w+_ADDR\b", stream_c))):
        assert name in defined, f"{name} is not defined by the csr.h of an Ethernet VRT build."
