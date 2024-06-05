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
    ("clk100", 0, Pins("C18"), IOStandard("LVCMOS33")), # SYSCLK.

    # Leds.
    ("user_led", 0, Pins("AB15"),  IOStandard("LVCMOS33")), # FPGA_LED2.

    # Debug.
    ("debug", 0, Pins("V13"),  IOStandard("LVCMOS33")), # SYNCDBG_CLK.

    # SI5351 Clocking.
    ("si5351_i2c", 0,
        Subsignal("scl", Pins("AA20")), # SI5351_SCL.
        Subsignal("sda", Pins("AB21")), # SI5351_SDA.
        IOStandard("LVCMOS33")
    ),
    ("si5351_pwm",  0, Pins("W19"), IOStandard("LVCMOS33")), # VCXO_TUNE_FPGA.
    ("si5351_clk0", 0, Pins("J19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_0.
    ("si5351_clk1", 0, Pins("E19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_1.
    ("si5351_clk2", 0, Pins("H4"),  IOStandard("LVCMOS25")), # FPGA_AUXCLK_3.
    ("si5351_clk3", 0, Pins("R4"),  IOStandard("LVCMOS25")), # FPGA_AUXCLK_4.

    # SPIFlash.
    ("flash_cs_n", 0, Pins("T19"), IOStandard("LVCMOS33")),
    ("flash", 0,
        Subsignal("mosi", Pins("P22")),
        Subsignal("miso", Pins("R22")),
        Subsignal("wp",   Pins("P21")),
        Subsignal("hold", Pins("R21")),
        IOStandard("LVCMOS33")
    ),

    # PCIe (M2 Connector).
    ("pcie_x1", 0,
        Subsignal("rst_n", Pins("A15"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("F6")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("E6")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("B8")), # PCIe_RX3_P
        Subsignal("rx_n",  Pins("A8")), # PCIe_RX3_N.
        Subsignal("tx_p",  Pins("B4")), # PCIe_TX3_P.
        Subsignal("tx_n",  Pins("A4")), # PCIe_TX3_N.
    ),
    ("pcie_x4", 0,
        Subsignal("rst_n", Pins("A15"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("F6")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("E6")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("D9 D11 B10 B8")), # PCIe_RX0-3_P.
        Subsignal("rx_n",  Pins("C9 C11 A10 A8")), # PCIe_RX0-3_N.
        Subsignal("tx_p",  Pins("D7  D5  B6 B4")), # PCIe_TX0-3_P.
        Subsignal("tx_n",  Pins("C7  C5  A6 A4")), # PCIe_TX0-3_N.
    ),

    # SFP 0 (When plugged in Acorn Baseboard Mini).
    ("sfp", 0,
        Subsignal("txp", Pins("B6")),  # PCIe_TX2_P.
        Subsignal("txn", Pins("A6")),  # PCIe_TX2_N.
        Subsignal("rxp", Pins("B10")), # PCIe_RX2_P.
        Subsignal("rxn", Pins("A10")), # PCIe_RX2_N.
    ),

    # SFP 1 (When plugged in Acorn Baseboard Mini).
    ("sfp", 1,
        Subsignal("txp", Pins("D5")),  # PCIe_TX1_P.
        Subsignal("txn", Pins("C5")),  # PCIe_TX1_N.
        Subsignal("rxp", Pins("D11")), # PCIe_RX1_P.
        Subsignal("rxn", Pins("C11")), # PCIe_RX1_N.
    ),

    # AD9361.
    ("ad9361_rfic", 0,
        Subsignal("rx_clk_p",   Pins("V4"),                 IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RF_DATA_CLK_P.
        Subsignal("rx_clk_n",   Pins("W4"),                 IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RF_DATA_CLK_N.
        Subsignal("rx_frame_p", Pins("AB7"),                IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RX_FRAME_P.
        Subsignal("rx_frame_n", Pins("AB6"),                IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RX_FRAME_N.
        Subsignal("rx_data_p",  Pins("U6 W6  Y6 V7 W9 V9"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RX_DATA_P0-5.
        Subsignal("rx_data_n",  Pins("V5 W5 AA6 W7 Y9 V8"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RX_DATA_P0-5.

        Subsignal("tx_clk_p",   Pins("T5"),                    IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RF_FB_CLK_P.
        Subsignal("tx_clk_n",   Pins("U5"),                    IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # RF_FB_CLK_N.
        Subsignal("tx_frame_p", Pins("AA8"),                   IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # TX_FRAME_P.
        Subsignal("tx_frame_n", Pins("AB8"),                   IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # TX_FRAME_N.
        Subsignal("tx_data_p",  Pins("U3  Y4 AB3 AA1 W1 AA5"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # TX_DATA_P_0-5.
        Subsignal("tx_data_n",  Pins("V3 AA4 AB2 AB1 Y1 AB5"), IOStandard("LVDS_25"), Misc("DIFF_TERM=TRUE")), # TX_DATA_N_0-5.

        Subsignal("rst_n",  Pins("E1"), IOStandard("LVCMOS25")), # RF_RESET_N.
        Subsignal("enable", Pins("P4"), IOStandard("LVCMOS25")), # RF_ENABLE.
        Subsignal("txnrx",  Pins("B2"), IOStandard("LVCMOS25")), # RF_RXTX.
        Subsignal("en_agc", Pins("N5"), IOStandard("LVCMOS25")), # RF_EN_AGC.

        Subsignal("ctrl", Pins("T1 U1 M3 M1"),             IOStandard("LVCMOS25")), # RF_CTRL_IN_ 0-3.
        Subsignal("stat", Pins("L1 M2 P1 R2 R3 N3 N2 N4"), IOStandard("LVCMOS25")), # RF_CTRL_OUT_0-7.
    ),
    ("ad9361_spi", 0,
        Subsignal("clk",  Pins("P5")), # RF_SPI_CLK.
        Subsignal("cs_n", Pins("E2")), # RF_SPI_CS.
        Subsignal("mosi", Pins("P6")), # RF_SPI_DI.
        Subsignal("miso", Pins("M6")), # RF_SPI_DO.
        IOStandard("LVCMOS25")
    ),
]

# Platform -----------------------------------------------------------------------------------------

class Platform(Xilinx7SeriesPlatform):
    default_clk_name   = "clk100"
    default_clk_period = 1e9/100e6

    def __init__(self, version="ultra", build_multiboot=False):
        assert version in ["classic", "ultra"]
        device = {
            "classic" : "xc7a35t",
            "ultra"   : "xc7a200t",
        }[version]
        Xilinx7SeriesPlatform.__init__(self, f"{device}sbg484-3", _io, toolchain="vivado")
        self.device     = device
        self.image_size = {
            "xc7a35t"  : 0x00400000,
            "xc7a200t" : 0x00800000,
        }[device]

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
        if build_multiboot:
            self.toolchain.additional_commands += [
                # Build Operational-Multiboot Bitstream.
                "set_property BITSTREAM.CONFIG.TIMER_CFG 0x0001FBD0 [current_design]",
                "set_property BITSTREAM.CONFIG.CONFIGFALLBACK Enable [current_design]",
                "write_bitstream -force {build_name}_operational.bit ",
                "write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit \"up 0x0 \
                {build_name}_operational.bit\" -file {build_name}_operational.bin",

                # Build Fallback-Multiboot Bitstream.
                "set_property BITSTREAM.CONFIG.NEXT_CONFIG_ADDR {} [current_design]".format(self.image_size),
                "write_bitstream -force {build_name}_fallback.bit ",
                "write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit \"up 0x0 \
                {build_name}_fallback.bit\" -file {build_name}_fallback.bin"]

    def create_programmer(self):
        return OpenFPGALoader(cable="ft2232", fpga_part=f"{self.device}sbg484", freq=10e6)

    def do_finalize(self, fragment):
        Xilinx7SeriesPlatform.do_finalize(self, fragment)
        self.add_period_constraint(self.lookup_request("clk100", loose=True), 1e9/100e6)
