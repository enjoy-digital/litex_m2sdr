#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse

from migen import *

from litex.gen import *

from litex_m2sdr_platform import Platform

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.integration.soc_core import *
from litex.soc.integration.builder  import *

from litex.soc.cores.clock     import *
from litex.soc.cores.led       import LedChaser
from litex.soc.cores.icap      import ICAP
from litex.soc.cores.xadc      import XADC
from litex.soc.cores.dna       import DNA

from litepcie.phy.s7pciephy import S7PCIEPHY

from litescope import LiteScopeAnalyzer

#from software import generate_litepcie_software

# CRG ----------------------------------------------------------------------------------------------

class CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq):
        self.cd_sys    = ClockDomain()
        self.cd_idelay = ClockDomain()

        # # #

        # Clk / Rst.
        clk100 = platform.request("clk100")

        # PLL.
        self.pll = pll = S7PLL(speedgrade=-3)
        pll.register_clkin(clk100, 100e6)
        pll.create_clkout(self.cd_sys, sys_clk_freq)
        pll.create_clkout(self.cd_idelay, 200e6)

        # IDelayCtrl.
        self.idelayctrl = S7IDELAYCTRL(self.cd_idelay)

# BaseSoC -----------------------------------------------------------------------------------------

class BaseSoC(SoCMini):
    SoCCore.csr_map = {
        # SoC.
        "uart"      : 0,
        "icap"      : 1,
        "flash"     : 2,
        "xadc"      : 3,
        "dna"       : 4,

        # PCIe.
        "pcie_phy"  : 10,
        "pcie_msi"  : 11,
        "pcie_dma0" : 12,

        # Analyzer.
        "analyzer"  : 30,
    }

    def __init__(self, sys_clk_freq=int(125e6), with_jtagbone=True):
        # Platform ---------------------------------------------------------------------------------
        platform = Platform()

        # SoCMini ----------------------------------------------------------------------------------
        SoCMini.__init__(self, platform, sys_clk_freq,
            ident         = f"LiteX SoC on LiteX-M2SDR",
            ident_version = True,
        )

        # Clocking ---------------------------------------------------------------------------------
        self.crg = CRG(platform, sys_clk_freq)

        # JTAGBone ---------------------------------------------------------------------------------
        if with_jtagbone:
            self.add_jtagbone()
            platform.add_period_constraint(self.jtagbone_phy.cd_jtag.clk, 1e9/20e6)
            platform.add_false_path_constraints(self.jtagbone_phy.cd_jtag.clk, self.crg.cd_sys.clk)

        # Leds -------------------------------------------------------------------------------------
        self.leds = LedChaser(
            pads         = platform.request_all("user_led"),
            sys_clk_freq = sys_clk_freq
        )

        # ICAP -------------------------------------------------------------------------------------
        self.icap = ICAP()
        self.icap.add_reload()
        self.icap.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)

        # XADC -------------------------------------------------------------------------------------
        self.xadc = XADC()

        # DNA --------------------------------------------------------------------------------------
        self.dna = DNA()
        self.dna.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)

        # PCIe -------------------------------------------------------------------------------------
        self.pcie_phy = S7PCIEPHY(platform, platform.request(f"pcie_x4"),
            data_width  = 128,
            bar0_size   = 0x20000,
            cd          = "sys",
        )
        self.pcie_phy.update_config({
            "Base_Class_Menu"          : "Wireless_controller",
            "Sub_Class_Interface_Menu" : "RF_controller",
            "Class_Code_Base"          : "0D",
            "Class_Code_Sub"           : "10",
            }
        )
        self.add_pcie(phy=self.pcie_phy, address_width=32, ndmas=1,
            with_dma_buffering    = True, dma_buffering_depth=8192,
            with_dma_loopback     = True,
            with_dma_synchronizer = True,
            with_msi              = True
        )

        # Timing Constraints/False Paths -----------------------------------------------------------
        for i in range(4):
            platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks        dna_clk] -asynchronous")
            platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks       jtag_clk] -asynchronous")
            platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks       icap_clk] -asynchronous")

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX SoC on LiteX-M2SDR.")
    parser.add_argument("--build",  action="store_true", help="Build bitstream.")
    parser.add_argument("--load",   action="store_true", help="Load bitstream.")
    parser.add_argument("--flash",  action="store_true", help="Flash bitstream.")
    parser.add_argument("--driver", action="store_true", help="Generate PCIe driver from LitePCIe (override local version).")
    args = parser.parse_args()

    # Build SoC.
    soc = BaseSoC()
    builder = Builder(soc, csr_csv="csr.csv")
    builder.build(run=args.build)

    # Generate LitePCIe Driver.
    #generate_litepcie_software(soc, "software", use_litepcie_software=args.driver)

    # Load Bistream.
    if args.load:
        prog = soc.platform.create_programmer()
        prog.load_bitstream(os.path.join(builder.gateware_dir, soc.build_name + ".bit"))

    # Flash Bitstream.
    if args.flash:
        prog = soc.platform.create_programmer()
        prog.flash(0, os.path.join(builder.gateware_dir, soc.build_name + ".bin"))

if __name__ == "__main__":
    main()
