#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.fhdl.specials import Tristate

from litex.gen import *
from litex.gen import LiteXContext
from litex.gen.genlib.misc import WaitTimer

from litex.build.io import DDROutput

from litepcie.common import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect import wishbone

from litex.soc.cores.pwm import PWM

from litei2c import LiteI2C

# Constants / Configs ------------------------------------------------------------------------------

# Constants.
# ----------

SI5351_I2C_ADDR = 0x60

SI5351_B_VERSION = 0b0
SI5351_C_VERSION = 0b1

SI5351_CLKIN_FROM_FPGA = 0b0
SI5351_CLKIN_FROM_UFL  = 0b1

# Configs.
# --------

# SI5351B-C Default Config from XO (38.4MHz on MS0/2/3/4/5/6/7 and 100MHz on MS1).
SI5351_I2C_SEQUENCE = [
    # Interrupt Mask Configuration
    ( 0x02, 0x33 ),  # Int masks: CLK_LOS(1), LOL_A(1) enabled, XO_LOS(0), LOL_B(0), SYS_INIT(0) disabled

    # Output Enable Control
    ( 0x03, 0x00 ),  # All CLK outputs enabled via register (OEB pin disabled)

    # PLL Reset Control
    ( 0x04, 0x10 ),  # Disable reset on PLLA LOS (bit4=1), PLLB reset normal (bit5=0)

    # I2C Configuration
    ( 0x07, 0x01 ),  # I2C address: 0x60 (default)

    # Clock Input Configuration
    ( 0x0F, 0x00 ),  # PLLB, src=XTAL (25MHz)

    # Output Channel Configuration (CLK0-CLK7)
    ( 0x10, 0x2D ),  # CLK0: LVCMOS 4mA, MS0 src=PLLB
    ( 0x11, 0x2D ),  # CLK1: LVCMOS 4mA, MS1 src=PLLB
    ( 0x12, 0x8C ),  # CLK2: OFF.
    ( 0x13, 0x8C ),  # CLK3: OFF.
    ( 0x14, 0x2D ),  # CLK4: LVCMOS 4mA, MS4 src=PLLB
    ( 0x15, 0x8C ),  # CLK5: OFF.
    ( 0x16, 0x8C ),  # CLK6: OFF.
    ( 0x17, 0x8C ),  # CLK7: OFF.

    # PLLB Configuration (VCO = 844.8MHz from 25MHz XTAL)
    ( 0x22, 0x42 ),
    ( 0x23, 0x40 ),
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

    # MS4 Configuration (Output Divider 22 for 38.4MHz)
    ( 0x4A, 0x00 ),
    ( 0x4B, 0x01 ),
    ( 0x4C, 0x00 ),
    ( 0x4D, 0x09 ),
    ( 0x4E, 0x00 ),
    ( 0x4F, 0x00 ),
    ( 0x50, 0x00 ),
    ( 0x51, 0x00 ),

    # Spread-Spectrum, Fractional Stepping Disabled
    ( 0x95, 0x00 ),
    ( 0x96, 0x00 ),
    ( 0x97, 0x00 ),
    ( 0x98, 0x00 ),
    ( 0x99, 0x00 ),
    ( 0x9A, 0x00 ),
    ( 0x9B, 0x00 ),

    # VCXO Configuration
    ( 0xA2, 0xF2 ),
    ( 0xA3, 0xFD ),
    ( 0xA4, 0x01 ),

    # Phase Offset Configuration
    ( 0xA5, 0x00 ),
    ( 0xA6, 0x00 ),
    ( 0xA7, 0x00 ),
    ( 0xA8, 0x00 ),
    ( 0xA9, 0x00 ),
    ( 0xAA, 0x00 ),

    # Crystal Load Capacitance
    ( 0xB7, 0x12 ),  # XTAL_CL=0 (6pF loading)
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

        # Memory and Port.
        mem      = Memory(16, len(seq_data), init=seq_data)
        mem_port = mem.get_port(async_read=True)
        self.specials += mem, mem_port

        # LiteI2C Registers.
        I2C_MASTER_ACTIVE_ADDR   = i2c_base + 0x04
        I2C_MASTER_SETTINGS_ADDR = i2c_base + 0x08
        I2C_MASTER_ADDR_ADDR     = i2c_base + 0x0c
        I2C_MASTER_RXTX_ADDR     = i2c_base + 0x10
        I2C_MASTER_STATUS_ADDR   = i2c_base + 0x14

        # FSM.
        self.fsm = fsm = FSM(reset_state="IDLE")
        self.fsm.act("IDLE",
            NextValue(mem_port.adr, 0),
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
                    NextState("CHECK-TX-READY")
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
        self.fsm.act("CHECK-TX-READY",
            bus.stb.eq(1),
            bus.cyc.eq(1),
            bus.we.eq(0),
            bus.adr.eq(I2C_MASTER_STATUS_ADDR),
            bus.sel.eq(0xf),
            If(bus.ack,
                If(bus.dat_r & 0b01,
                    NextState("CONF-0")
                ).Else(
                    NextState("CHECK-TX-READY")
                )
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
            bus.dat_w.eq(2 | (0 << 8)), # TX=2, RX=0
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
                If(mem_port.adr == (len(seq_data) - 1),
                    NextState("DONE")
                ).Else(
                    NextState("FLUSH-RX")
                )
            )
        )
        self.fsm.act("DONE",
            self.done.eq(1)
        )

# SI5351 -------------------------------------------------------------------------------------------

class SI5351(LiteXModule):
    def __init__(self, pads, i2c_base, with_csr=True):
        self.version   = Signal() # SI5351 Version (0=B, 1=C).
        self.ss_en     = Signal() # SI5351 Spread spectrum enable (versions A and B).
        self.clkin_src = Signal() # SI5351 ClkIn Source.
        self.clkin_ufl = Signal() # SI5351 ClkIn from uFL.

        # # #

        # Context.
        # --------
        soc      = LiteXContext.top
        platform = LiteXContext.platform

        # LiteI2C Master.
        # ---------------
        self.i2c = LiteI2C(soc.sys_clk_freq,
            pads                     = pads,
            i2c_master_tx_fifo_depth = 8,
            i2c_master_rx_fifo_depth = 8,
        )

        # LiteI2C Sequencer for I2C Gateware Init.
        # ----------------------------------------
        self.sequencer = ResetInserter()(LiteI2CSequencer(
            sys_clk_freq = soc.sys_clk_freq,
            i2c_base     = i2c_base,
            i2c_adr      = SI5351_I2C_ADDR,
            i2c_sequence = SI5351_I2C_SEQUENCE,
        ))

        # VCXO PWM.
        # ---------
        pwm_o = Signal()
        self.pwm = PWM(
            pwm            = pwm_o,
            default_enable = 1,
            default_width  = 1024,
            default_period = 2048,
        )
        self.comb += pads.pwm.eq(Mux(self.version == SI5351_C_VERSION, 1, pwm_o))

        # Enable / Clkin.
        # ---------------

        # ClkIn Source Mux.
        si5351_clkin = Signal()
        self.specials += Instance("BUFGMUX",
            i_S  = self.clkin_src,
            i_I0 = ClockSignal("clk10"),
            i_I1 = self.clkin_ufl,
            o_O  = si5351_clkin,
        )

        # ClkIn/Enable Output.
        si5351_ddr_i1 = Signal()
        si5351_ddr_i2 = Signal()
        self.comb += [
            Case(self.version, {
                # SI5351B: Spread Spectrum Enable.
                SI5351_B_VERSION : [
                   si5351_ddr_i1.eq(self.ss_en),
                   si5351_ddr_i2.eq(self.ss_en),
                ],
                # SI5351C: 10MHz ClkIn.
                SI5351_C_VERSION : [
                    si5351_ddr_i1.eq(1),
                    si5351_ddr_i2.eq(0),
                ]
            })
        ]
        self.specials += DDROutput(
            clk = si5351_clkin,
            i1  = si5351_ddr_i1,
            i2  = si5351_ddr_i2,
            o   = pads.ssen_clkin,
        )

        # CSRs.
        if with_csr:
            self.add_csr()

    def add_csr(self, default_version=SI5351_B_VERSION, default_clkin=SI5351_CLKIN_FROM_FPGA):
        self.control = CSRStorage(fields=[
            CSRField("version",  size=1, offset=0, values=[
                ("``0b0``", "SI5351B Version."),
                ("``0b1``", "SI5351C Version."),
            ], reset=default_version),
            CSRField("clkin_src",  size=1, offset=1, values=[
                ("``0b0``", "10MHz ClkIn from FPGA (clk10)."),
                ("``0b1``", "10MHz ClkIn from uFL."),
            ], reset=default_clkin),
            CSRField("ss_en",  size=1, offset=2, values=[
                ("``0b0``", "SI5351B Spread spectrum disabled."),
                ("``0b1``", "SI5351B Spread spectrum enabled."),
            ], reset=default_version),
            CSRField("seq_reset",  size=1, offset=8, pulse=True, values=[
                ("``0b1``", "Gateware Sequencer Reset."),
            ], reset=default_version),
        ])
        self.status = CSRStatus(fields=[
            CSRField("seq_done",  size=1, offset=8, values=[
                ("``0b0``", "Gateware Sequencer Ongoing."),
                ("``0b1``", "Gateware Sequencer Done."),
            ]),
        ])

        # # #

        # Control.
        self.comb += [
            self.version.eq(        self.control.fields.version),
            self.ss_en.eq(          self.control.fields.ss_en),
            self.clkin_src.eq(      self.control.fields.clkin_src),
            self.sequencer.reset.eq(self.control.fields.seq_reset),
        ]

        # Status.
        self.comb += [
            self.status.fields.seq_done.eq(self.sequencer.done),
        ]
