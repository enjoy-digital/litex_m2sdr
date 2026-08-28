#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Supported SoC build configurations.

The CSR layout is a binary interface between a bitstream and the host software: the driver and
tools are compiled against litex_m2sdr/software/kernel/csr.h, but the flashed image decides what
the addresses in it actually mean. Rather than regenerate that header per image (and hope the
right one is in the tree when the tools are built), every configuration listed here is required
to place every CSR it shares with the others at the same address, and the tracked header is the
union of all of them -- so one build of the software works with any image.

scripts/gen_kernel_headers.py regenerates the header from this list; test/test_csr_layout.py
enforces the agreement and that the tracked header is up to date.

Each entry maps a name to the BaseSoC keyword arguments for that configuration. "requires" names
Python packages the configuration needs; configurations whose requirements are missing are
skipped (their CSRs are then absent from the union, so they cannot be the only source of a
register in the tracked header).

The White Rabbit configurations are deliberately absent: elaborating them needs the litex_wr_nic
package, a wr-cores VHDL checkout and a firmware image, which is more than a CSR-map check should
drag in. BaseSoC.check_csr_map() still runs on every build, so the CI White Rabbit smoke tests
cover them -- that is what caught uart_xover having no fixed location.
"""

SUPPORTED_CONFIGS = {
    # M.2 variant: PCIe host interface, the released widths plus the feature builds.
    "m2_pcie_x1"          : dict(variant="m2", with_pcie=True, pcie_lanes=1),
    "m2_pcie_x2"          : dict(variant="m2", with_pcie=True, pcie_lanes=2),
    "m2_pcie_x4"          : dict(variant="m2", with_pcie=True, pcie_lanes=4),
    "m2_pcie_x4_oversampling" : dict(variant="m2", with_pcie=True, pcie_lanes=4,
        with_rfic_oversampling=True),
    "m2_pcie_x1_gpio"     : dict(variant="m2", with_pcie=True, pcie_lanes=1, with_gpio=True),
    "m2_pcie_x4_no_jtagbone" : dict(variant="m2", with_pcie=True, pcie_lanes=4,
        with_jtagbone=False),
    # PTM lives in litex_wr_nic and is a Gen2 x1 only feature.
    "m2_pcie_x1_ptm"      : dict(variant="m2", with_pcie=True, pcie_lanes=1, with_pcie_ptm=True,
        requires=["litex_wr_nic"]),

    # Baseboard variant: Ethernet and/or SATA, with or without PCIe.
    "baseboard_eth"       : dict(variant="baseboard", with_pcie=False, with_eth=True),
    "baseboard_eth_ptp"   : dict(variant="baseboard", with_pcie=False, with_eth=True,
        with_eth_ptp=True),
    "baseboard_eth_ptp_rfic_clock" : dict(variant="baseboard", with_pcie=False, with_eth=True,
        with_eth_ptp=True, with_eth_ptp_rfic_clock=True),
    "baseboard_eth_vrt"   : dict(variant="baseboard", with_pcie=False, with_eth=True,
        with_eth_vrt=True),
    "baseboard_eth_sata"  : dict(variant="baseboard", with_pcie=False, with_eth=True,
        with_sata=True),
    "baseboard_pcie_x1_eth"  : dict(variant="baseboard", with_pcie=True, pcie_lanes=1,
        with_eth=True),
    "baseboard_pcie_x1_sata" : dict(variant="baseboard", with_pcie=True, pcie_lanes=1,
        with_sata=True),
}

# Constants that legitimately describe one image rather than the CSR interface, and so are not
# required to agree between configurations. The union header carries the reference value.
PER_IMAGE_CONSTANTS = {
    "config_identifier",       # Carries the build timestamp; read from the device at runtime.
    "config_clock_frequency",  # Follows --sys-clk-freq, which is a per-image override.
}

# Configuration the union header takes its per-image constants from.
REFERENCE_CONFIG = "m2_pcie_x4"

# Identifier written into the union header, in place of a single image's build timestamp.
UNION_IDENTIFIER = "LiteX-M2SDR SoC / CSR reference (union of the supported build configurations)"


def config_requirements(config):
    """Python packages a configuration needs, or an empty list."""
    return list(config.get("requires", []))


def config_kwargs(config):
    """BaseSoC keyword arguments for a configuration."""
    return {k: v for k, v in config.items() if k != "requires"}


def missing_requirements(config):
    """Requirements of a configuration that are not importable here."""
    import importlib.util
    return [r for r in config_requirements(config) if importlib.util.find_spec(r) is None]
