#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex.gen import *

from litex.soc.interconnect.csr import *

# Capability ---------------------------------------------------------------------------------------

class Capability(LiteXModule):
    """
    Capability Module for LiteX M2SDR.
    Provides read-only CSRs to indicate the API version, hardware capabilities such as PCIe
    presence and configuration, Ethernet presence and speed, SATA presence and configuration,
    GPIO presence, White Rabbit presence, and board-level details.

    Parameters:
    - api_version_str (str) : API version as a string (e.g., "1.0" for v1.0).
    - pcie_enabled (bool)   : Indicates if PCIe is present.
    - pcie_speed (str)      : PCIe speed (e.g., "gen2").
    - pcie_lanes (int)      : Number of PCIe lanes (e.g., 1, 2, 4, 8).
    - pcie_ptm (bool)       : PCIe Precision Time Measurement enable status (True if PTM is enabled).
    - eth_enabled (bool)    : Indicates if Ethernet is present.
    - eth_speed (str)       : Ethernet speed (e.g., "1000basex" for 1Gbps).
    - sata_enabled (bool)   : Indicates if SATA is present.
    - sata_gen (str)        : SATA generation (e.g., "gen2").
    - sata_mode (str)       : SATA mode (e.g., "read+write").
    - gpio_enabled (bool)   : Indicates if GPIO is present.
    - wr_enabled (bool)     : Indicates if White Rabbit is present.
    - variant (str)         : Board variant ("m2" or "baseboard").
    - jtagbone (bool)       : Indicates if JTAGBone is present.
    - eth_sfp (int)         : Ethernet SFP index.
    - wr_sfp (int)          : White Rabbit SFP index.
    """
    def __init__(self, api_version_str,
        # PCIe.
        pcie_enabled, pcie_speed, pcie_lanes, pcie_ptm,
        # Ethernet.
        eth_enabled, eth_speed,
        # SATA.
        sata_enabled, sata_gen, sata_mode,
        # GPIO.
        gpio_enabled,
        # White Rabbit.
        wr_enabled,
        # Board.
        variant, jtagbone, eth_sfp, wr_sfp):

        # API Version.
        # ------------
        major, minor   = map(int, api_version_str.split('.'))
        api_version    = (major << 16) | minor
        self._api_version = CSRStatus(32,
            description = "API Version of the gateware (e.g., 0x0100 for v1.0).",
            reset       = api_version
        )
        # Features.
        # ---------
        self._features = CSRStatus(32, fields=[
            CSRField("pcie",     size=1, offset=0, reset=int(pcie_enabled), description="PCIe     is present."),
            CSRField("eth",      size=1, offset=1, reset=int(eth_enabled),  description="Ethernet is present."),
            CSRField("sata",     size=1, offset=2, reset=int(sata_enabled), description="SATA     is present."),
            CSRField("gpio",     size=1, offset=3, reset=int(gpio_enabled), description="GPIO     is present."),
            CSRField("wr",       size=1, offset=4, reset=int(wr_enabled),   description="White Rabbit is present."),
            CSRField("jtagbone", size=1, offset=5, reset=int(jtagbone),     description="JTAGBone is present."),
            # Reserved bits for future features
        ], description="Hardware feature presence bitfield.")

        # PCIe Config.
        # ------------
        self.pcie_speed_map  = {"gen1": 0, "gen2": 1}
        self.pcie_lanes_map  = {1: 0, 2: 1, 4: 2}
        pcie_speed_value     = self.pcie_speed_map[pcie_speed] if pcie_enabled else 0
        pcie_lanes_value     = self.pcie_lanes_map[pcie_lanes] if pcie_enabled else 0
        self._pcie_config = CSRStatus(32, fields=[
            CSRField("speed", size=2, offset=0, reset=pcie_speed_value, values=[
                ("``0b00``", "Gen1"),
                ("``0b01``", "Gen2"),
            ], description="PCIe speed configuration."),
            CSRField("lanes", size=2, offset=2, reset=pcie_lanes_value, values=[
                ("``0b00``", "X1"),
                ("``0b01``", "X2"),
                ("``0b10``", "X4"),
                ("``0b11``", "Reserved"),
            ], description="PCIe lanes configuration."),
            CSRField("ptm", size=1, offset=4, reset=pcie_ptm, values=[
                ("``0``", "PTM disabled or not present."),
                ("``1``", "PTM enabled."),
            ], description="PCIe Precision Time Measurement (PTM) enable status."),
            # Reserved bits
        ], description="PCIe configuration. Valid only if features.pcie is set.")

        # Ethernet Config.
        # ----------------
        self.eth_speed_map = {"1000basex": 0, "2500basex": 1}
        eth_speed_value    = self.eth_speed_map[eth_speed] if eth_enabled else 0
        self._eth_config = CSRStatus(32, fields=[
            CSRField("speed", size=2, offset=0, reset=eth_speed_value, values=[
                ("``0b00``", "1Gbps"),
                ("``0b01``", "2.5Gbps"),
                ("``0b10``", "Reserved"),
                ("``0b11``", "Reserved"),
            ], description="Ethernet speed configuration."),
            # Reserved bits
        ], description="Ethernet configuration. Valid only if features.eth is set.")

        # SATA Config.
        # ------------
        self.sata_gen_map  = {
            "gen1": 0,
            "gen2": 1,
            "gen3": 2,
        }
        self.sata_mode_map = {
            "read-only":  0,
            "write-only": 1,
            "read+write": 2,
        }
        sata_gen_value  = self.sata_gen_map[sata_gen] if sata_enabled else 0
        sata_mode_value = self.sata_mode_map[sata_mode] if sata_enabled else 0
        self._sata_config = CSRStatus(32, fields=[
            CSRField("gen", size=2, offset=0, reset=sata_gen_value, values=[
                ("``0b00``", "Gen1"),
                ("``0b01``", "Gen2"),
                ("``0b10``", "Gen3"),
                ("``0b11``", "Reserved"),
            ], description="SATA generation configuration."),
            CSRField("mode", size=2, offset=2, reset=sata_mode_value, values=[
                ("``0b00``", "Read-only"),
                ("``0b01``", "Write-only"),
                ("``0b10``", "Read+Write"),
                ("``0b11``", "Reserved"),
            ], description="SATA mode configuration."),
            # Reserved bits
        ], description="SATA configuration. Valid only if features.sata is set.")

        # Board Info.
        # -----------
        self.variant_map = {"m2": 0, "baseboard": 1}
        variant_value = self.variant_map.get(variant, 0)
        self._board_info = CSRStatus(32, fields=[
            CSRField("variant",  size=2, offset=0, reset=variant_value, values=[
                ("``0b00``", "M.2"),
                ("``0b01``", "Baseboard"),
                ("``0b10``", "Reserved"),
                ("``0b11``", "Reserved"),
            ], description="Board variant."),
            CSRField("jtagbone", size=1, offset=2, reset=int(jtagbone), description="JTAGBone enabled."),
            CSRField("eth_sfp",  size=1, offset=3, reset=int(eth_sfp),  description="Ethernet SFP index."),
            CSRField("wr_sfp",   size=1, offset=4, reset=int(wr_sfp),   description="White Rabbit SFP index."),
            # Reserved bits
        ], description="Board-level configuration.")
