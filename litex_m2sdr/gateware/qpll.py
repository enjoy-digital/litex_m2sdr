#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from liteeth.phy.a7_gtp import QPLLSettings, QPLL

# Shared QPLL --------------------------------------------------------------------------------------

class SharedQPLL(LiteXModule):
    def __init__(self, platform, with_pcie=False, with_eth=False, eth_refclk_freq=156.25e6, eth_phy="1000basex", with_sata=False):
        assert not (with_pcie and with_eth and with_sata) # QPLL only has 2 PLLs :(

        # PCIe QPLL Settings.
        qpll_pcie_settings = QPLLSettings(
            refclksel  = 0b001,
            fbdiv      = 5,
            fbdiv_45   = 5,
            refclk_div = 1,
        )

        # Ethernet QPLL Settings.
        qpll_eth_settings = QPLLSettings(
            refclksel  = 0b111,
            fbdiv      = {"1000basex": 4, "2500basex": 5}[eth_phy],
            fbdiv_45   = {125e6: 5, 156.25e6 : 4}[eth_refclk_freq],
            refclk_div = 1,
        )

        # SATA QPLL Settings.
        qpll_sata_settings = QPLLSettings(
            refclksel  = 0b111,
            fbdiv      = 5,
            fbdiv_45   = 4,
            refclk_div = 1,
        )

        # QPLL Configs.
        class QPLLConfig:
            def __init__(self, refclk, settings):
                self.refclk   = refclk
                self.settings = settings

        self.configs = configs = {}
        if with_pcie:
            configs["pcie"] = QPLLConfig(
                refclk   = ClockSignal("refclk_pcie"),
                settings = qpll_pcie_settings,
            )
        if with_eth:
            configs["eth"] = QPLLConfig(
                refclk   = ClockSignal("refclk_eth"),
                settings = qpll_eth_settings,
            )
        if with_sata:
            configs["sata"] = QPLLConfig(
                refclk   = ClockSignal("refclk_sata"),
                settings = qpll_sata_settings,
            )

        # Shared QPLL.
        self.qpll        = None
        self.channel_map = {}
        # Single QPLL configuration.
        if len(configs) == 1:
            name, config = next(iter(configs.items()))
            gtrefclk0, gtgrefclk0 = self.get_gt_refclks(config)
            self.qpll = QPLL(
                gtrefclk0     = gtrefclk0,
                gtgrefclk0    = gtgrefclk0,
                qpllsettings0 = config.settings,
                gtrefclk1     = None,
                gtgrefclk1    = None,
                qpllsettings1 = None,
            )
            self.channel_map[name] = 0
         # Dual QPLL configuration.
        elif len(configs) == 2:
            config_items = list(configs.items())
            gtrefclk0, gtgrefclk0 = self.get_gt_refclks(config_items[0][1])
            gtrefclk1, gtgrefclk1 = self.get_gt_refclks(config_items[1][1])
            self.qpll = QPLL(
                gtrefclk0     = gtrefclk0,
                gtgrefclk0    = gtgrefclk0,
                qpllsettings0 = config_items[0][1].settings,
                gtrefclk1     = gtrefclk1,
                gtgrefclk1    = gtgrefclk1,
                qpllsettings1 = config_items[1][1].settings,
            )
            self.channel_map[config_items[0][0]] = 0
            self.channel_map[config_items[1][0]] = 1

        platform.add_platform_command("set_property SEVERITY {{Warning}} [get_drc_checks REQP-49]")

    @staticmethod
    def get_gt_refclks(config):
        if config.settings.refclksel == 0b111:
            return None, config.refclk
        else:
            return config.refclk, None

    def get_channel(self, name):
        if name in self.channel_map:
            channel_index = self.channel_map[name]
            return self.qpll.channels[channel_index]
        else:
            raise ValueError(f"Invalid QPLL name: {name}")