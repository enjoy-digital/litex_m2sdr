#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.fhdl.specials import Tristate

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer

from litex.build.io import DDROutput

from litepcie.common import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect import wishbone

from litex.soc.cores.pwm import PWM

from litei2c import LiteI2C

# SI5351B-C Default Config from XO (38.4MHz on MS0/2/3/4/5/6/7 and 100MHz on MS1) ------------------

si5351_i2c_sequence = [
    # Interrupt Mask Configuration
    ( 0x02, 0x33 ),  # Int masks: CLK_LOS(1), LOL_A(1) enabled, XO_LOS(0), LOL_B(0), SYS_INIT(0) disabled

    # Output Enable Control
    ( 0x03, 0x00 ),  # All CLK outputs enabled via register (OEB pin disabled)

    # PLL Reset Control
    ( 0x04, 0x10 ),  # Disable reset on PLLA LOS (bit4=1), PLLB reset normal (bit5=0)

    # I2C Configuration
    ( 0x07, 0x01 ),  # I2C address: 0x60 (default)

    # Clock Input Configuration
    ( 0x0F, 0x00 ),  # PLL src=XTAL (25MHz), CLKIN_DIV=1

    # Output Channel Configuration (CLK0-CLK7)
    ( 0x10, 0x2F ),  # CLK0: LVCMOS 8mA, MS0 src=PLLB
    ( 0x11, 0x2F ),  # CLK1: LVCMOS 8mA, MS1 src=PLLB
    ( 0x12, 0x2F ),  # CLK2: LVCMOS 8mA, MS2 src=PLLB
    ( 0x13, 0x2F ),  # CLK3: LVCMOS 8mA, MS3 src=PLLB
    ( 0x14, 0x2F ),  # CLK4: LVCMOS 8mA, MS4 src=PLLB
    ( 0x15, 0x2F ),  # CLK5: LVCMOS 8mA, MS5 src=PLLB
    ( 0x16, 0x2F ),  # CLK6: LVCMOS 8mA, MS6 src=PLLB
    ( 0x17, 0x2F ),  # CLK7: LVCMOS 8mA, MS7 src=PLLB

    # PLLB Configuration (VCO = 844.8MHz from 25MHz XTAL)
    ( 0x22, 0x42 ),  # PLLB feedback: Multisynth NA (integer mode)
    ( 0x23, 0x40 ),  # PLLB reset=normal
    ( 0x24, 0x00 ),
    ( 0x25, 0x0E ),
    ( 0x26, 0xE5 ),
    ( 0x27, 0xF5 ),
    ( 0x28, 0xBC ),
    ( 0x29, 0xC0 ),

    # MS0 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x2A, 0x00 ),
    ( 0x2B, 0x01 ),
    ( 0x2C, 0x00 ),
    ( 0x2D, 0x09 ),
    ( 0x2E, 0x00 ),
    ( 0x2F, 0x00 ),
    ( 0x30, 0x00 ),
    ( 0x31, 0x00 ),

    # MS1 Configuration (Output Divider 8.448 for 100MHz)
    ( 0x32, 0x00 ),
    ( 0x33, 0x7D ),
    ( 0x34, 0x00 ),
    ( 0x35, 0x02 ),
    ( 0x36, 0x39 ),
    ( 0x37, 0x00 ),
    ( 0x38, 0x00 ),
    ( 0x39, 0x2B ),

    # MS2 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x3A, 0x00 ),
    ( 0x3B, 0x01 ),
    ( 0x3C, 0x00 ),
    ( 0x3D, 0x09 ),
    ( 0x3E, 0x00 ),
    ( 0x3F, 0x00 ),
    ( 0x40, 0x00 ),
    ( 0x41, 0x00 ),

    # MS3 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x42, 0x00 ),
    ( 0x43, 0x01 ),
    ( 0x44, 0x00 ),
    ( 0x45, 0x09 ),
    ( 0x46, 0x00 ),
    ( 0x47, 0x00 ),
    ( 0x48, 0x00 ),
    ( 0x49, 0x00 ),

    # MS4 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x4A, 0x00 ),
    ( 0x4B, 0x01 ),
    ( 0x4C, 0x00 ),
    ( 0x4D, 0x09 ),
    ( 0x4E, 0x00 ),
    ( 0x4F, 0x00 ),
    ( 0x50, 0x00 ),
    ( 0x51, 0x00 ),

    # MS5 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x52, 0x00 ),
    ( 0x53, 0x01 ),
    ( 0x54, 0x00 ),
    ( 0x55, 0x09 ),
    ( 0x56, 0x00 ),
    ( 0x57, 0x00 ),
    ( 0x58, 0x00 ),
    ( 0x59, 0x00 ),

    # MS6 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x5A, 0x16 ),

    # MS7 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x5B, 0x16 ),

    # Spread-Spectrum, Fractional Stepping Disabled
    ( 0x95, 0x00 ),  # SSDN_P2=0, SSC_EN=0 => no SS
    ( 0x96, 0x00 ),  # Reserved = 0
    ( 0x97, 0x00 ),  # SSDN_P3=0, SSC_MODE=0 => off
    ( 0x98, 0x00 ),  # Reserved = 0
    ( 0x99, 0x00 ),  # SSDN_P1=0
    ( 0x9A, 0x00 ),  # SSUDP=0 => no up/down spread
    ( 0x9B, 0x00 ),  # Reserved = 0

    # VCXO Configuration
    ( 0xA2, 0xF2 ),
    ( 0xA3, 0xFD ),
    ( 0xA4, 0x01 ),

    # Phase Offset Configuration
    ( 0xA5, 0x00 ),  # CLK0 phase offset=0
    ( 0xA6, 0x00 ),  # CLK1 phase offset=0
    ( 0xA7, 0x00 ),  # CLK2 phase offset=0
    ( 0xA8, 0x00 ),  # CLK3 phase offset=0
    ( 0xA9, 0x00 ),  # CLK4 phase offset=0
    ( 0xAA, 0x00 ),  # CLK5 phase offset=0

    # Crystal Load Capacitance
    (0xB7, 0x12 ),  # XTAL_CL=0 (6pF loading)
]

# LiteI2C Sequencer -----------------------------------------------------------------------------

class LiteI2CSequencer(LiteXModule):
    def __init__(self, sys_clk_freq, i2c_base, i2c_adr, i2c_sequence):
        self.done = Signal()
        self.bus = bus = wishbone.Interface(address_width=32, data_width=32, addressing="byte")

        # # #

        # Sequences Preparation.
        seq_data = []
        for i, (addr, data) in enumerate(i2c_sequence):
            seq_data.append(addr << 8 | data)

        # Memory and Port
        mem      = Memory(16, len(seq_data), init=seq_data)
        mem_port = mem.get_port(async_read=True)
        self.specials += mem, mem_port

        # Sequence Addr/Length.
        seq_adr = 0
        seq_len = len(i2c_sequence)

        # LiteI2C Registers.
        I2C_MASTER_ACTIVE_ADDR   = i2c_base + 0x04
        I2C_MASTER_SETTINGS_ADDR = i2c_base + 0x08
        I2C_MASTER_ADDR_ADDR     = i2c_base + 0x0c
        I2C_MASTER_RXTX_ADDR     = i2c_base + 0x10
        I2C_MASTER_STATUS_ADDR   = i2c_base + 0x14

        # FSM
        self.fsm = fsm = FSM(reset_state="IDLE")
        self.fsm.act("IDLE",
            NextValue(mem_port.adr, seq_adr),
            NextState("CHECK-RX-READY")
        )
        self.fsm.act("CHECK-RX-READY",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(0),
            bus.adr.eq(I2C_MASTER_STATUS_ADDR),
            bus.sel.eq(0xf),
            If(bus.ack,
                If(bus.dat_r & 0b10,
                    NextState("FLUSH-RX")
                ).Else(
                    NextState("CONF-0")
                )
            )
        )
        self.fsm.act("FLUSH-RX",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(0),
            bus.adr.eq(I2C_MASTER_RXTX_ADDR),
            bus.sel.eq(0xf),
            If(bus.ack,
                NextState("CHECK-RX-READY")
            )
        )
        self.fsm.act("CONF-0",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(1),
            bus.adr.eq(I2C_MASTER_ACTIVE_ADDR),
            bus.sel.eq(0xf),
            bus.dat_w.eq(1),
            If(bus.ack,
                NextState("CONF-1")
            )
        )
        self.fsm.act("CONF-1",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(1),
            bus.adr.eq(I2C_MASTER_SETTINGS_ADDR),
            bus.sel.eq(0xf),
            bus.dat_w.eq(2 | (0 << 8)),  # TX=2, RX=0
            If(bus.ack,
                NextState("CONF-2")
            )
        )
        self.fsm.act("CONF-2",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(1),
            bus.adr.eq(I2C_MASTER_ADDR_ADDR),
            bus.sel.eq(0xf),
            bus.dat_w.eq(i2c_adr),
            If(bus.ack,
                NextState("DATA")
            )
        )
        self.fsm.act("DATA",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(1),
            bus.adr.eq(I2C_MASTER_RXTX_ADDR),
            bus.sel.eq(0xf),
            bus.dat_w.eq(mem_port.dat_r),
            If(bus.ack,
                NextValue(mem_port.adr, mem_port.adr + 1),
                If(mem_port.adr == (seq_adr + seq_len - 1),
                    NextState("WAIT")
                )
            )
        )
        self.wait_timer = WaitTimer(sys_clk_freq*100e-3) # 100ms.
        self.fsm.act("WAIT",
            self.wait_timer.wait.eq(1),
            If(self.wait_timer.done,
                NextState("DONE")
            )
        )
        self.fsm.act("DONE",
            self.done.eq(1)
        )

# SI5351 -------------------------------------------------------------------------------------------

class SI5351(LiteXModule):
    def __init__(self, platform, sys_clk_freq, clk_in=0, with_csr=True):
        self.version    = Signal() # SI5351 Version (0=B, 1=C).
        self.ss_en      = Signal() # SI5351 Spread spectrum enable (versions A and B).
        self.clk_in_src = Signal() # SI5351 ClkIn Source.

        # # #

        # I2C Pads.
        i2c_pads = platform.request("si5351_i2c")

        # LiteI2C Master.
        self.i2c = LiteI2C(sys_clk_freq,
            pads                     = i2c_pads,
            i2c_master_tx_fifo_depth = 32,
            i2c_master_rx_fifo_depth = 32,
        )

        # I2C Sequencer for Gateware Init.
        self.sequencer = LiteI2CSequencer(
            sys_clk_freq = sys_clk_freq,
            i2c_base     = 0xa000, # FIXME: Avoid hardcoded value.
            i2c_adr      = 0x60,
            i2c_sequence = si5351_i2c_sequence,
        )

        # VCXO PWM.
        self.pwm = PWM(platform.request("si5351_pwm"),
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )

        # Enable / Clkin.
        platform.add_platform_command("set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk10_clk]")
        self.specials += Instance("BUFGMUX",
            i_S  = self.version,
            i_I0 = self.ss_en,
            i_I1 = ClockSignal("clk10"),
            o_O  = platform.request("si5351_ssen_clkin"),
        )

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_version=0, default_clk_in=0):
        self.control = CSRStorage(fields=[
            CSRField("version",  size=1, offset=0, values=[
                ("``0b0``", "SI5351B Version."),
                ("``0b1``", "SI5351C Version."),
            ], reset=default_version),
            CSRField("clk_in_src",  size=1, offset=1, values=[ # FIXME use this instead of version
                ("``0b0``", "10MHz ClkIn from XO."),
                ("``0b1``", "10MHz ClkIn from FPGA."),
            ], reset=default_clk_in),
            CSRField("ss_en",  size=1, offset=2, values=[
                ("``0b0``", "SI5351B Spread spectrum disabled."),
                ("``0b1``", "SI5351B Spread spectrum enabled."),
            ], reset=default_version),
        ])

        # # #

        self.comb += [
            self.version.eq(self.control.fields.version),
            self.clk_in_src.eq(self.control.fields.clk_in_src),
        ]
