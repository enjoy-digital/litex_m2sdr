#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex_m2sdr.gateware.capability import Capability


def test_capability_builds_with_valid_configuration():
    dut = Capability(
        api_version_str="1.2",
        pcie_enabled=True,
        pcie_speed="gen2",
        pcie_lanes=4,
        pcie_ptm=True,
        eth_enabled=True,
        eth_speed="1000basex",
        sata_enabled=True,
        sata_gen="gen2",
        sata_mode="read+write",
        gpio_enabled=True,
        wr_enabled=False,
        variant="m2",
        jtagbone=True,
        eth_sfp=0,
        wr_sfp=1,
    )
    assert dut is not None
