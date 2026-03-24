#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse

from migen import *

from litex.gen import LiteXModule
from litex.soc.integration.soc_core import SoCMini
from litex.soc.integration.builder import Builder

from litex_m2sdr import Platform
from litex_m2sdr.gateware.led import StatusLedTester


class LedTestCRG(LiteXModule):
    def __init__(self, platform):
        self.cd_sys = ClockDomain()

        # # #

        clk100 = platform.request("clk100")
        self.comb += [
            self.cd_sys.clk.eq(clk100),
            self.cd_sys.rst.eq(0),
        ]


class LedTestSoC(SoCMini):
    def __init__(self, sys_clk_freq=int(100e6), with_jtagbone=True):
        platform = Platform(build_multiboot=False)

        SoCMini.__init__(self, platform, sys_clk_freq,
            ident             = "LiteX-M2SDR LED Test SoC",
            ident_version     = True,
            csr_address_width = 15,
        )

        self.crg = LedTestCRG(platform)

        self.led_test = StatusLedTester(sys_clk_freq=sys_clk_freq)
        self.comb += platform.request("user_led").eq(self.led_test.output)

        if with_jtagbone:
            self.add_jtagbone()
            platform.add_period_constraint(self.jtagbone.phy.cd_jtag.clk, 1e9/20e6)
            platform.toolchain.pre_placement_commands += [
                "set _sys_clk  [get_clocks -quiet clk100]",
                "set _jtag_clk [get_clocks -quiet jtag_clk]",
                "if {{[llength $_sys_clk] && [llength $_jtag_clk]}} {{",
                "    set_clock_groups -asynchronous -group $_sys_clk -group $_jtag_clk",
                "}}",
            ]


def main():
    parser = argparse.ArgumentParser(description="Build/load the minimal LiteX-M2SDR LED test SoC.")
    parser.add_argument("--build", action="store_true", help="Build bitstream.")
    parser.add_argument("--load",  action="store_true", help="Load bitstream.")
    args = parser.parse_args()

    build_name = "litex_m2sdr_led_test"
    output_dir = os.path.join("build", build_name)
    csr_csv    = os.path.join(output_dir, "csr.csv")

    soc = LedTestSoC()
    builder = Builder(soc, output_dir=output_dir, csr_csv=csr_csv)
    builder.build(build_name=build_name, run=args.build)

    if args.load:
        prog = soc.platform.create_programmer()
        prog.load_bitstream(os.path.join(builder.gateware_dir, build_name + ".bit"))


if __name__ == "__main__":
    main()
