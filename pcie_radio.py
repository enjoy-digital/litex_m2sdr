#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse

from migen import *
from migen.genlib.resetsync import AsyncResetSynchronizer
from migen.genlib.cdc import PulseSynchronizer, MultiReg

from litex.gen import *

from pcie_radio_platform import Platform

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.integration.soc_core import *
from litex.soc.integration.builder  import *

from litex.soc.cores.clock     import *
from litex.soc.cores.led       import LedChaser
from litex.soc.cores.icap      import ICAP
from litex.soc.cores.xadc      import XADC
from litex.soc.cores.dna       import DNA
from litex.soc.cores.pwm       import PWM
from litex.soc.cores.bitbang   import I2CMaster

from litex.build.generic_platform import IOStandard, Subsignal, Pins

from litepcie.phy.s7pciephy import S7PCIEPHY

from liteeth.phy.a7_gtp import QPLLSettings, QPLL
from liteeth.phy.a7_1000basex import A7_1000BASEX

from litescope import LiteScopeAnalyzer

from gateware.ad9361.core import AD9361RFIC
from gateware.cdcm6208 import CDCM6208

from software import generate_litepcie_software

# CRG ----------------------------------------------------------------------------------------------

class CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq):
        self.cd_sys    = ClockDomain()
        self.cd_idelay = ClockDomain()

        # # #

        # Clk / Rst.
        clk38p4 = platform.request("clk38p4")

        # PLL.
        self.pll = pll = S7PLL(speedgrade=-2)
        pll.register_clkin(clk38p4, 38.4e6)
        pll.create_clkout(self.cd_sys, sys_clk_freq)
        pll.create_clkout(self.cd_idelay, 200e6)

        # IDelayCtrl.
        self.idelayctrl = S7IDELAYCTRL(self.cd_idelay)

# BaseSoC -----------------------------------------------------------------------------------------

class BaseSoC(SoCMini):
    def __init__(self, sys_clk_freq=int(125e6), with_pcie=True, pcie_lanes=1, with_jtagbone=True):
        # Platform ---------------------------------------------------------------------------------
        platform = Platform()

        # SoCMini ----------------------------------------------------------------------------------
        SoCMini.__init__(self, platform, sys_clk_freq,
            ident         = f"LiteX SoC on PCIe-Radio",
            ident_version = True,
        )

        # Clocking ---------------------------------------------------------------------------------
        self.crg = CRG(platform, sys_clk_freq)
        platform.add_platform_command("set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets {{*crg_clkin}}]")

        # SI5351 Clock Generator -------------------------------------------------------------------
        class SI5351FakePads:
            def __init__(self):
                self.scl = Signal()
                self.sda = Signal()
                self.pwm = Signal()
        si5351_fake_pads = SI5351FakePads()

        self.si5351_i2c = I2CMaster(pads=si5351_fake_pads)
        self.si5351_pwm = PWM(si5351_fake_pads.pwm,
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )

        # CDCM6208 Clock Generator -----------------------------------------------------------------

        self.cdcm6208 = CDCM6208(pads=platform.request("cdcm6208"), sys_clk_freq=sys_clk_freq)

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
        if with_pcie:
            self.pcie_phy = S7PCIEPHY(platform, platform.request(f"pcie_x{pcie_lanes}"),
                data_width  = {1: 64, 4: 128}[pcie_lanes],
                bar0_size   = 0x20000,
                cd          = "sys",
            )
            if pcie_lanes == 1:
                platform.toolchain.pre_placement_commands.append("reset_property LOC [get_cells -hierarchical -filter {{NAME=~pcie_s7/*gtp_channel.gtpe2_channel_i}}]")
                platform.toolchain.pre_placement_commands.append("set_property LOC GTPE2_CHANNEL_X0Y2 [get_cells -hierarchical -filter {{NAME=~pcie_s7/*gtp_channel.gtpe2_channel_i}}]")
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
            self.comb += self.pcie_dma0.synchronizer.pps.eq(1)

            # Timing Constraints/False Paths -------------------------------------------------------
            for i in range(4):
                platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks        dna_clk] -asynchronous")
                platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks       jtag_clk] -asynchronous")
                platform.toolchain.pre_placement_commands.append(f"set_clock_groups -group [get_clocks {{{{*s7pciephy_clkout{i}}}}}] -group [get_clocks       icap_clk] -asynchronous")

        # AD9361 RFIC ------------------------------------------------------------------------------

        self.ad9361 = AD9361RFIC(
            rfic_pads    = platform.request("ad9361_rfic"),
            spi_pads     = platform.request("ad9361_spi"),
            sys_clk_freq = sys_clk_freq,
        )
        self.ad9361.add_prbs()
        if with_pcie:
            self.comb += [
                self.pcie_dma0.source.connect(self.ad9361.sink),
                self.ad9361.source.connect(self.pcie_dma0.sink),
        ]

        # Debug.
        with_spi_analyzer  = False
        with_rfic_analyzer = False
        with_dma_analyzer  = False
        if with_spi_analyzer:
            analyzer_signals = [platform.lookup_request("ad9361_spi")]
            self.analyzer = LiteScopeAnalyzer(analyzer_signals,
                depth        = 4096,
                clock_domain = "sys",
                register     = True,
                csr_csv      = "analyzer.csv"
            )
        if with_rfic_analyzer:
            analyzer_signals = [
                self.ad9361.phy.sink,   # TX.
                self.ad9361.phy.source, # RX.
                self.ad9361.prbs_rx.fields.synced
            ]
            self.analyzer = LiteScopeAnalyzer(analyzer_signals,
                depth        = 4096,
                clock_domain = "rfic",
                register     = True,
                csr_csv      = "analyzer.csv"
            )
        if with_dma_analyzer:
            assert with_pcie
            analyzer_signals = [
                self.pcie_dma0.sink,   # RX.
                self.pcie_dma0.source, # TX.
                self.pcie_dma0.synchronizer.synced,
            ]
            self.analyzer = LiteScopeAnalyzer(analyzer_signals,
                depth        = 1024,
                clock_domain = "sys",
                register     = True,
                csr_csv      = "analyzer.csv"
            )

        # Clk Measurements -------------------------------------------------------------------------

        class ClkMeasurement(LiteXModule):
            def __init__(self, clk, increment=1):
                self.latch = CSR()
                self.value = CSRStatus(64)

                # # #

                # Create Clock Domain.
                self.cd_counter = ClockDomain()
                self.comb += self.cd_counter.clk.eq(clk)
                self.specials += AsyncResetSynchronizer(self.cd_counter, ResetSignal())

                # Free-running Clock Counter.
                counter = Signal(64)
                self.sync.counter += counter.eq(counter + increment)

                # Latch Clock Counter.
                latch_value = Signal(64)
                latch_sync  = PulseSynchronizer("sys", "counter")
                self.submodules += latch_sync
                self.comb += latch_sync.i.eq(self.latch.re)
                self.sync.counter += If(latch_sync.o, latch_value.eq(counter))
                self.specials += MultiReg(latch_value, self.value.status)

        self.clk0_measurement = ClkMeasurement(clk=0)
        self.clk1_measurement = ClkMeasurement(clk=ClockSignal("rfic"))
        self.clk2_measurement = ClkMeasurement(clk=0)
        self.clk3_measurement = ClkMeasurement(clk=0)

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX SoC on PCIe-Radio.")
    parser.add_argument("--build",  action="store_true", help="Build bitstream.")
    parser.add_argument("--load",   action="store_true", help="Load bitstream.")
    parser.add_argument("--flash",  action="store_true", help="Flash bitstream.")
    parser.add_argument("--driver", action="store_true", help="Generate PCIe driver from LitePCIe (override local version).")
    comopts = parser.add_mutually_exclusive_group()
    comopts.add_argument("--with-pcie",      action="store_true", help="Enable PCIe Communication.")
    args = parser.parse_args()

    # Build SoC.
    soc = BaseSoC(
        with_pcie     = args.with_pcie,
    )
    builder = Builder(soc, csr_csv="csr.csv")
    builder.build(run=args.build)

    # Generate LitePCIe Driver.
    generate_litepcie_software(soc, "software", use_litepcie_software=args.driver)

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
