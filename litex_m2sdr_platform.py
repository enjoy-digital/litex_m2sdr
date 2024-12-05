#
# This file is part of LiteX-M2SDR project.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import subprocess

from litex.build.generic_platform import *
from litex.build.xilinx           import Xilinx7SeriesPlatform
from litex.build.openfpgaloader   import OpenFPGALoader

# IOs ----------------------------------------------------------------------------------------------

_io = [
    # Clk/Rst.
    ("clk100", 0, Pins("C18"), IOStandard("LVCMOS33")), # SYSCLK.

    # Leds.
    ("user_led", 0, Pins("AB15"),  IOStandard("LVCMOS33")), # FPGA_LED2.

    # Debug.
    ("debug", 0, Pins("V13"),  IOStandard("LVCMOS33")), # SYNCDBG_CLK.

    # Ext Sync/ClkIn..
    ("sync_clk_in", 0, Pins("V13"),  IOStandard("LVCMOS33")), # SYNCDBG_CLK.

    # SI5351 Clocking.
    ("si5351_i2c", 0,
        Subsignal("scl", Pins("AA20")), # SI5351_SCL.
        Subsignal("sda", Pins("AB21")), # SI5351_SDA.
        Misc("PULLUP=TRUE"),
        IOStandard("LVCMOS33")
    ),
    ("si5351_en_clkin", 0, Pins("W22"), IOStandard("LVCMOS33")), # SI5351_EN (B) / SI5351_CLKIN (C).
    ("si5351_pwm",      0, Pins("W19"), IOStandard("LVCMOS33")), # VCXO_TUNE_FPGA.
    ("si5351_clk0",     0, Pins("J19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_0.
    ("si5351_clk1",     0, Pins("E19"), IOStandard("LVCMOS33")), # FPGA_AUXCLK_1.
    ("si5351_clk2",     0, Pins("H4"),  IOStandard("LVCMOS25")), # FPGA_AUXCLK_3.
    ("si5351_clk3",     0, Pins("R4"),  IOStandard("LVCMOS25")), # FPGA_AUXCLK_4.

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
    ("pcie_x1_baseboard", 0,
        Subsignal("rst_n", Pins("M2:PERSTn"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("M2:REFClkp")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("M2:REFClkn")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("M2:PERn3")), # PCIe_RX3_P
        Subsignal("rx_n",  Pins("M2:PERp3")), # PCIe_RX3_N.
        Subsignal("tx_p",  Pins("M2:PETn3")), # PCIe_TX3_P.
        Subsignal("tx_n",  Pins("M2:PETp3")), # PCIe_TX3_N.
    ),
    ("pcie_x1_m2", 0,
        Subsignal("rst_n", Pins("M2:PERSTn"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("M2:REFClkp")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("M2:REFClkn")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("M2:PERn0")), # PCIe_RX0_P
        Subsignal("rx_n",  Pins("M2:PERp0")), # PCIe_RX0_N.
        Subsignal("tx_p",  Pins("M2:PETn0")), # PCIe_TX0_P.
        Subsignal("tx_n",  Pins("M2:PETp0")), # PCIe_TX0_N.
    ),
    ("pcie_x2_m2", 0,
        Subsignal("rst_n", Pins("M2:PERSTn"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("M2:REFClkp")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("M2:REFClkn")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("M2:PERn0 M2:PERn1")), # PCIe_RX0-1_P
        Subsignal("rx_n",  Pins("M2:PERp0 M2:PERp1")), # PCIe_RX0-1_N.
        Subsignal("tx_p",  Pins("M2:PETn0 M2:PETn1")), # PCIe_TX0-1_P.
        Subsignal("tx_n",  Pins("M2:PETp0 M2:PETp1")), # PCIe_TX0-1_N.
    ),
    ("pcie_x4_m2", 0,
        Subsignal("rst_n", Pins("M2:PERSTn"), IOStandard("LVCMOS33"), Misc("PULLUP=TRUE")), # PCIe_PERST.
        Subsignal("clk_p", Pins("M2:REFClkp")), # PCIe_REF_CLK_P.
        Subsignal("clk_n", Pins("M2:REFClkn")), # PCIe_REF_CLK_N.
        Subsignal("rx_p",  Pins("M2:PERn0 M2:PERn1 M2:PERn2 M2:PERn3")), # PCIe_RX0-3_P.
        Subsignal("rx_n",  Pins("M2:PERp0 M2:PERp1 M2:PERp2 M2:PERp3")), # PCIe_RX0-3_N.
        Subsignal("tx_p",  Pins("M2:PETn0 M2:PETn1 M2:PETn2 M2:PETn3")), # PCIe_TX0-3_P.
        Subsignal("tx_n",  Pins("M2:PETp0 M2:PETp1 M2:PETp2 M2:PETp3")), # PCIe_TX0-3_N.
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

_connectors = [
    ("M2", {
        # PCIe.
        "PETn3"   : " A4", #  5 / PCIe_TX3_P.
        "PETp3"   : " B4", #  7 / PCIe_TX3_N.
        "PERn3"   : " A8", # 11 / PCIe_RX3_P.
        "PERp3"   : " B8", # 13 / PCIe_RX3_N.
        "PETn2"   : " A6", # 17 / PCIe_TX2_P.
        "PETp2"   : " B6", # 19 / PCIe_TX2_N.
        "PERn2"   : "A10", # 23 / PCIe_RX2_P.
        "PERp2"   : "B10", # 25 / PCIe_RX2_N.
        "PETn1"   : " C5", # 29 / PCIe_TX1_P.
        "PETp1"   : " D5", # 31 / PCIe_TX1_N.
        "PERn1"   : "C11", # 35 / PCIe_RX1_P.
        "PERp1"   : "D11", # 37 / PCIe_RX1_N.
        "PETn0"   : " C7", # 41 / PCIe_TX0_P.
        "PETp0"   : " D7", # 43 / PCIe_TX0_N.
        "PERn0"   : " C9", # 47 / PCIe_RX0_P.
        "PERp0"   : " D9", # 49 / PCIe_RX0_N.
        "REFClkn" : " E6", # 53 / PCIe_REF_CLK_N.
        "REFClkp" : " F6", # 55 / PCIe_REF_CLK_N.
        "PERSTn"  : "A15", # 50 / PCIe_PERST.

        # SMB.
        "SMB_CLK" : "A13", # 40 / PCIe_SMCLK. /!\ Requires 0 Ohm on R82, not mounted by default /!\.
        "SMB_DAT" : "A14", # 40 / PCIe_SMDAT. /!\ Requures 0 Ohm on R83, not mounted by default /!\.

        # Synchro.
        "NC22"    : "K18", # 22 / PPS_IN.
        "NC24"    : "Y18", # 24 / PPS_OUT.
        "NC28"    : "A19", # 28 / Synchro_GPIO1.
        "NC30"    : "A18", # 30 / Synchro_GPIO2.
        "NC32"    : "A21", # 32 / Synchro_GPIO3.
        "NC34"    : "A20", # 34 / Synchro_GPIO4.
        "NC36"    : "B20", # 36 / Synchro_GPIO5.
    })
]

# Platform -----------------------------------------------------------------------------------------

class Platform(Xilinx7SeriesPlatform):
    default_clk_name   = "clk100"
    default_clk_period = 1e9/100e6

    def __init__(self, build_multiboot=False):
        device = "xc7a200t"
        Xilinx7SeriesPlatform.__init__(self, f"{device}sbg484-3", _io, _connectors, toolchain="vivado")
        self.device     = device
        self.image_size = {
            "xc7a200t" : 0x00800000,
        }[device]

        self.toolchain.bitstream_commands = [
            "set_property BITSTREAM.CONFIG.UNUSEDPIN Pulldown [current_design]",
            "set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]",
            "set_property BITSTREAM.CONFIG.CONFIGRATE 66 [current_design]",
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
                "set_property BITSTREAM.CONFIG.TIMER_CFG 0x01000000 [current_design]",
                "set_property BITSTREAM.CONFIG.CONFIGFALLBACK Enable [current_design]",
                "write_bitstream -force {build_name}_operational.bit ",
                "write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit \"up 0x0 \
                {build_name}_operational.bit\" -file {build_name}_operational.bin",

                # Build Fallback-Multiboot Bitstream.
                f"set_property BITSTREAM.CONFIG.NEXT_CONFIG_ADDR 0x{self.image_size:08x} [current_design]",
                "write_bitstream -force {build_name}_fallback.bit ",
                "write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit \"up 0x0 \
                {build_name}_fallback.bit\" -file {build_name}_fallback.bin"]

    def detect_ftdi_chip(self):
        lsusb_log = subprocess.run(['lsusb'], capture_output=True, text=True)
        for ftdi_chip in ["ft232", "ft2232", "ft4232"]:
            if f"Future Technology Devices International, Ltd {ftdi_chip.upper()}" in lsusb_log.stdout:
                return ftdi_chip
        return None

    def create_programmer(self):
        ftdi_chip = self.detect_ftdi_chip()
        if ftdi_chip is None:
            raise RuntimeError("No compatible FTDI device found.")
        return OpenFPGALoader(cable=ftdi_chip, fpga_part=f"{self.device}sbg484", freq=20e6)

    def do_finalize(self, fragment):
        Xilinx7SeriesPlatform.do_finalize(self, fragment)
        self.add_period_constraint(self.lookup_request("clk100",      loose=True), 1e9/100e6)
        self.add_period_constraint(self.lookup_request("si5351_clk0", loose=True), 1e9/38.4e6)
        self.add_period_constraint(self.lookup_request("sync_clk_in", loose=True), 1e9/10e6)