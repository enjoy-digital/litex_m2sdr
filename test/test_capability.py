#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex_m2sdr.gateware.capability import Capability

# Capability Tests --------------------------------------------------------------------------------


def test_capability_builds_with_valid_configuration():
    """Check Capability CSR block instantiates with a representative valid config."""
    dut = Capability(
        api_version_str="1.2",
        pcie_enabled=True,
        pcie_speed="gen2",
        pcie_lanes=4,
        pcie_ptm=True,
        eth_enabled=True,
        eth_speed="1000basex",
        eth_ptp=True,
        eth_ptp_rfic_clock=True,
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


def test_capability_accepts_experimental_5000basex():
    dut = Capability(
        api_version_str="1.2",
        pcie_enabled=False,
        pcie_speed="gen1",
        pcie_lanes=1,
        pcie_ptm=False,
        eth_enabled=True,
        eth_speed="5000basex",
        eth_ptp=False,
        eth_ptp_rfic_clock=False,
        sata_enabled=False,
        sata_gen="gen1",
        sata_mode="read-only",
        gpio_enabled=False,
        wr_enabled=False,
        variant="baseboard",
        jtagbone=True,
        eth_sfp=0,
        wr_sfp=1,
    )
    assert dut._eth_config.fields.speed.reset == 2
