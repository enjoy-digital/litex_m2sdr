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
    ("clk38p4", 0, Pins("V22"), IOStandard("LVCMOS33")),

    # Leds.
    ("user_led", 0, Pins("D22"),  IOStandard("LVCMOS33")),

    # SPIFlash.
    ("flash_cs_n", 0, Pins("T19"), IOStandard("LVCMOS33")),
    ("flash", 0,
        Subsignal("mosi", Pins("P22")),
        Subsignal("miso", Pins("R22")),
        Subsignal("wp",   Pins("P21")),
        Subsignal("hold", Pins("R21")),
        IOStandard("LVCMOS33")
    ),

    # PCIe.
    ("pcie_x1", 0,
        Subsignal("rst_n", Pins("A13"), IOStandard("LVCMOS33")),
        Subsignal("clk_p", Pins("F6")),
        Subsignal("clk_n", Pins("E6")),
        Subsignal("rx_p",  Pins("B10")),
        Subsignal("rx_n",  Pins("A10")),
        Subsignal("tx_p",  Pins("B6")),
        Subsignal("tx_n",  Pins("A6"))
    ),

    # CDCM 6208.
    ("cdcm6208", 0,
        Subsignal("clk",    Pins("H2")),
        Subsignal("rst_n",  Pins("K2")),
        Subsignal("cs_n",   Pins("H3")),
        Subsignal("mosi",   Pins("K1")),
        Subsignal("miso",   Pins("J1")),
        Subsignal("status", Pins("M1 L1")),
        IOStandard("LVCMOS25")
    ),

    # AD9361.
    ("ad9361_rfic", 0,
        Subsignal("rx_clk_p",   Pins("L19"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("rx_clk_n",   Pins("L20"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("rx_frame_p", Pins("M15"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("rx_frame_n", Pins("M16"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("rx_data_p",  Pins("N20 K21 K17 J19 G17 N22"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("rx_data_n",  Pins("M20 K22 J17 H19 G18 M22"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),

        Subsignal("tx_clk_p",   Pins("N18"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("tx_clk_n",   Pins("N19"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("tx_frame_p", Pins("W9"),  IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("tx_frame_n", Pins("Y9"),  IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("tx_data_p",  Pins("AA5 Y4 AB7 AA1 W1 Y8"),  IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),
        Subsignal("tx_data_n",  Pins("AB5 AA4 AB6 AB1 Y1 Y7"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")),

        Subsignal("rst_n",  Pins("L18"), IOStandard("LVCMOS25")),
        Subsignal("enable", Pins("W6"),  IOStandard("LVCMOS25")),
        Subsignal("txnrx",  Pins("AB8"), IOStandard("LVCMOS25")),
        Subsignal("en_agc", Pins("W5"),  IOStandard("LVCMOS25")),

        Subsignal("ctrl", Pins("U1 V2 U2 T1"), IOStandard("LVCMOS25")),
        Subsignal("stat", Pins("H13 H14 J14 G13 J16 H15 J15 G15"), IOStandard("LVCMOS25")),
    ),
    ("ad9361_spi", 0,
        Subsignal("clk",  Pins("L14")),
        Subsignal("cs_n", Pins("M17")),
        Subsignal("mosi", Pins("L15")),
        Subsignal("miso", Pins("L16")),
        IOStandard("LVCMOS25")
    ),
]

# Platform -----------------------------------------------------------------------------------------

class Platform(Xilinx7SeriesPlatform):
    default_clk_name   = "clk38p4"
    default_clk_period = 1e9/38.4e6

    def __init__(self, toolchain="vivado"):
        Xilinx7SeriesPlatform.__init__(self, "xc7a35t-fgg484-2", _io, toolchain=toolchain)

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
        return OpenFPGALoader(cable="digilent_hs2", fpga_part=f"xc7a35tfgg484", freq=10e6)

    def do_finalize(self, fragment):
        Xilinx7SeriesPlatform.do_finalize(self, fragment)
        self.add_period_constraint(self.lookup_request("clk38p4", loose=True), 1e9/38.4e6)
