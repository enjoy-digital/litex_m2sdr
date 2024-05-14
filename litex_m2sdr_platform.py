#
# This file is part of LiteX-M2SDR project.
#
# Copyright (c) 2024 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex.build.generic_platform import *
from litex.build.xilinx import Xilinx7SeriesPlatform
from litex.build.openfpgaloader import OpenFPGALoader

# IOs ----------------------------------------------------------------------------------------------

_io = [
    # Clk/Rst.
    ("clk100", 0, Pins("C18"), IOStandard("LVCMOS33")),

    # Leds.
    ("user_led", 0, Pins("AB15"),  IOStandard("LVCMOS33")),

    # PCIe.
    ("pcie_x4", 0,
        Subsignal("rst_n", Pins("A15"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")),
        Subsignal("clk_p", Pins("F6")),
        Subsignal("clk_n", Pins("E6")),
        Subsignal("rx_p",  Pins("D9 D11 B10 B8")),
        Subsignal("rx_n",  Pins("C9 C11 A10 A8")),
        Subsignal("tx_p",  Pins("D7  D5  B6 B4")),
        Subsignal("tx_n",  Pins("C7  C5  A6 A4")),
    ),
]

# Platform -----------------------------------------------------------------------------------------

class Platform(Xilinx7SeriesPlatform):
    default_clk_name   = "clk100"
    default_clk_period = 1e9/100e6

    def __init__(self, toolchain="vivado"):
        Xilinx7SeriesPlatform.__init__(self, "xc7a200tsbg484-3", _io, toolchain=toolchain)

        self.toolchain.bitstream_commands = [
            "set_property BITSTREAM.CONFIG.UNUSEDPIN Pulldown [current_design]",
            "set_property CFGBVS VCCO [current_design]",
            "set_property CONFIG_VOLTAGE 3.3 [current_design]",
        ]

    def create_programmer(self):
        return OpenFPGALoader(cable="digilent_hs2", fpga_part=f"xc7a200tsbg484", freq=10e6)

    def do_finalize(self, fragment):
        Xilinx7SeriesPlatform.do_finalize(self, fragment)
        self.add_period_constraint(self.lookup_request("clk100", loose=True), 1e9/100e6)
