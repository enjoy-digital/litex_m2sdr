#
# This file is part of LiteX-M2SDR project.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
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

    # Debug.
    ("debug", 0, Pins("V13"),  IOStandard("LVCMOS33")), # SYNCDBG_CLK.

    # SI5351 Clocking.
    ("si5351_i2c", 0,
        Subsignal("scl", Pins("AA20")),
        Subsignal("sda", Pins("AB21")),
        IOStandard("LVCMOS33")
    ),
    ("si5351_pwm",  0, Pins("W19"), IOStandard("LVCMOS33")), # VCXO_TUNE_FPGA.
    ("si5351_clk0", 0, Pins("J19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_0.
    ("si5351_clk1", 0, Pins("E19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_1.
    ("si5351_clk2", 0, Pins("H4"),  IOStandard("LVCMOS33")), # FPGA_AUXCLK_3.
    ("si5351_clk3", 0, Pins("R4"),  IOStandard("LVCMOS33")), # FPGA_AUXCLK_4.

    # PCIe (M2 Connector).
    ("pcie_x1", 0,
        Subsignal("rst_n", Pins("A15"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")),
        Subsignal("clk_p", Pins("F6")),
        Subsignal("clk_n", Pins("E6")),
        Subsignal("rx_p",  Pins("B8")),
        Subsignal("rx_n",  Pins("A8")),
        Subsignal("tx_p",  Pins("B4")),
        Subsignal("tx_n",  Pins("A4")),
    ),
    ("pcie_x4", 0,
        Subsignal("rst_n", Pins("A15"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")),
        Subsignal("clk_p", Pins("F6")),
        Subsignal("clk_n", Pins("E6")),
        Subsignal("rx_p",  Pins("D9 D11 B10 B8")),
        Subsignal("rx_n",  Pins("C9 C11 A10 A8")),
        Subsignal("tx_p",  Pins("D7  D5  B6 B4")),
        Subsignal("tx_n",  Pins("C7  C5  A6 A4")),
    ),

    # SFP 0 (When plugged in Acorn Baseboard Mini).
    ("sfp", 0,
        Subsignal("txp", Pins("B6")),
        Subsignal("txn", Pins("A6")),
        Subsignal("rxp", Pins("B10")),
        Subsignal("rxn", Pins("A10")),
    ),

    # SFP 1 (When plugged in Acorn Baseboard Mini).
    ("sfp", 1,
        Subsignal("txp", Pins("D5")),
        Subsignal("txn", Pins("C5")),
        Subsignal("rxp", Pins("D11")),
        Subsignal("rxn", Pins("C11")),
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
            "set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]",
            "set_property BITSTREAM.CONFIG.CONFIGRATE 16 [current_design]",
            "set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]",
            "set_property CFGBVS VCCO [current_design]",
            "set_property CONFIG_VOLTAGE 3.3 [current_design]",
        ]
        self.toolchain.additional_commands = [
            "write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit \"up 0x0 \
            {build_name}.bit\" -file {build_name}.bin",
        ]

    def create_programmer(self):
        return OpenFPGALoader(cable="ft2232", fpga_part=f"xc7a200tsbg484", freq=10e6)

    def do_finalize(self, fragment):
        Xilinx7SeriesPlatform.do_finalize(self, fragment)
        self.add_period_constraint(self.lookup_request("clk100", loose=True), 1e9/100e6)
