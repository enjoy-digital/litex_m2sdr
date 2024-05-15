#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from collections import namedtuple

from migen import *
from litex.soc.interconnect import wishbone


# I2C-----------------------------------------------------------------------------------------------

class I2CClockGen(Module):
    def __init__(self, width):
        self.load  = Signal(width)
        self.clk2x = Signal()

        cnt = Signal.like(self.load)
        self.comb += [
            self.clk2x.eq(cnt == 0),
        ]
        self.sync += [
            If(self.clk2x,
                cnt.eq(self.load),
            ).Else(
                cnt.eq(cnt - 1),
            ),
        ]


class I2CMasterMachine(Module):
    def __init__(self, clock_width):
        self.scl_o = Signal(reset=1)
        self.sda_o = Signal(reset=1)
        self.sda_i = Signal()

        self.submodules.cg  = CEInserter()(I2CClockGen(clock_width))
        self.idle  = Signal()
        self.start = Signal()
        self.stop  = Signal()
        self.write = Signal()
        self.read  = Signal()
        self.ack   = Signal()
        self.data  = Signal(8)

        ###

        busy = Signal()
        bits = Signal(4)

        fsm = CEInserter()(FSM("IDLE"))
        self.submodules += fsm

        fsm.act("IDLE",
            If(self.start,
                NextState("START0"),
            ).Elif(self.stop & self.start,
                NextState("RESTART0"),
            ).Elif(self.stop,
                NextState("STOP0"),
            ).Elif(self.write,
                NextValue(bits, 8),
                NextState("WRITE0"),
            ).Elif(self.read,
                NextValue(bits, 8),
                NextState("READ0"),
            )
        )

        fsm.act("START0",
            NextValue(self.scl_o, 1),
            NextState("START1"))
        fsm.act("START1",
            NextValue(self.sda_o, 0),
            NextState("IDLE"))

        fsm.act("RESTART0",
            NextValue(self.scl_o, 0),
            NextState("RESTART1"))
        fsm.act("RESTART1",
            NextValue(self.sda_o, 1),
            NextState("START0"))

        fsm.act("STOP0",
            NextValue(self.scl_o, 0),
            NextState("STOP1"))
        fsm.act("STOP1",
            NextValue(self.scl_o, 1),
            NextValue(self.sda_o, 0),
            NextState("STOP2"))
        fsm.act("STOP2",
            NextValue(self.sda_o, 1),
            NextState("IDLE"))

        fsm.act("WRITE0",
            NextValue(self.scl_o, 0),
            If(bits == 0,
                NextValue(self.sda_o, 1),
                NextState("READACK0"),
            ).Else(
                NextValue(self.sda_o, self.data[7]),
                NextState("WRITE1"),
            )
        )
        fsm.act("WRITE1",
            NextValue(self.scl_o, 1),
            NextValue(self.data[1:], self.data[:-1]),
            NextValue(bits, bits - 1),
            NextState("WRITE0"),
        )
        fsm.act("READACK0",
            NextValue(self.scl_o, 1),
            NextState("READACK1"),
        )
        fsm.act("READACK1",
            NextValue(self.ack, ~self.sda_i),
            NextState("IDLE")
        )

        fsm.act("READ0",
            NextValue(self.scl_o, 0),
            NextValue(self.sda_o, 1),
            NextState("READ1"),
        )
        fsm.act("READ1",
            NextValue(self.data[0], self.sda_i),
            NextValue(self.scl_o, 0),
            If(bits == 0,
                NextValue(self.sda_o, ~self.ack),
                NextState("WRITEACK0"),
            ).Else(
                NextValue(self.sda_o, 1),
                NextState("READ2"),
            )
        )
        fsm.act("READ2",
            NextValue(self.scl_o, 1),
            NextValue(self.data[1:], self.data[:-1]),
            NextValue(bits, bits - 1),
            NextState("READ1"),
        )
        fsm.act("WRITEACK0",
            NextValue(self.scl_o, 1),
            NextState("IDLE"),
        )

        run = Signal()
        self.comb += [
            run.eq(self.start | self.stop | self.write | self.read),
            self.idle.eq(~run & fsm.ongoing("IDLE")),
            self.cg.ce.eq(~self.idle),
            fsm.ce.eq(run | self.cg.clk2x),
        ]

# Registers:
# config = Record([
#     ("div",   20),
# ])
# xfer = Record([
#     ("data",  8),
#     ("ack",   1),
#     ("read",  1),
#     ("write", 1),
#     ("start", 1),
#     ("stop",  1),
#     ("idle",  1),
# ])
class I2CMaster(Module):
    def __init__(self, pads, bus=None):
        if bus is None:
            bus = wishbone.Interface(data_width=32)
        self.bus = bus

        ###

        # Wishbone
        self.submodules.i2c = i2c = I2CMasterMachine(
            clock_width=20)

        self.sync += [
            # read
            If(bus.adr[0],
                bus.dat_r.eq(i2c.cg.load),
            ).Else(
                bus.dat_r.eq(Cat(i2c.data, i2c.ack, C(0, 4), i2c.idle)),
            ),

            # write
            i2c.read.eq(0),
            i2c.write.eq(0),
            i2c.start.eq(0),
            i2c.stop.eq(0),

            bus.ack.eq(0),
            If(bus.cyc & bus.stb & ~bus.ack,
                bus.ack.eq(1),
                If(bus.we,
                    If(bus.adr[0],
                        i2c.cg.load.eq(bus.dat_w),
                    ).Else(
                        i2c.data.eq(bus.dat_w[0:8]),
                        i2c.ack.eq(bus.dat_w[8]),
                        i2c.read.eq(bus.dat_w[9]),
                        i2c.write.eq(bus.dat_w[10]),
                        i2c.start.eq(bus.dat_w[11]),
                        i2c.stop.eq(bus.dat_w[12]),
                    )
                )
            )
        ]

        # I/O
        self.scl_t = TSTriple()
        self.specials += self.scl_t.get_tristate(pads.scl)
        self.comb += [
            self.scl_t.oe.eq(~i2c.scl_o),
            self.scl_t.o.eq(0),
        ]

        self.sda_t = TSTriple()
        self.specials += self.sda_t.get_tristate(pads.sda)
        self.comb += [
            self.sda_t.oe.eq(~i2c.sda_o),
            self.sda_t.o.eq(0),
            i2c.sda_i.eq(self.sda_t.i),
        ]

I2C_XFER_ADDR, I2C_CONFIG_ADDR = range(2)
(
    I2C_ACK,
    I2C_READ,
    I2C_WRITE,
    I2C_START,
    I2C_STOP,
    I2C_IDLE,
) = (1 << i for i in range(8, 14))

# Sequencer-----------------------------------------------------------------------------------------

# Instruction set:
#  <2> OP  <1> ADDRESS  <20> DATA_MASK
#
# OP=00: end program, ADDRESS=don't care, DATA_MASK=don't care
# OP=01: write, ADDRESS=address, DATA_MASK=data
# OP=10: wait until masked bits set, ADDRESS=address, DATA_MASK=mask


InstEnd = namedtuple("InstEnd", "")
InstWrite = namedtuple("InstWrite", "address data")
InstWait = namedtuple("InstWait", "address mask")

def encode(inst):
    address, data_mask = 0, 0
    if isinstance(inst, InstEnd):
        opcode = 0b00
    elif isinstance(inst, InstWrite):
        opcode = 0b01
        address = inst.address
        data_mask = inst.data
    elif isinstance(inst, InstWait):
        opcode = 0b10
        address = inst.address
        data_mask = inst.mask
    else:
        raise ValueError
    return (opcode << 21) | (address << 20) | data_mask


class Sequencer(Module):
    def __init__(self, program, bus=None):
        if bus is None:
            bus = wishbone.Interface()
        self.bus  = bus
        self.done = Signal()

        ###

        assert isinstance(program[-1], InstEnd)
        program_e = [encode(inst) for inst in program]
        mem = Memory(32, len(program), init=program_e)
        self.specials += mem

        mem_port = mem.get_port()
        self.specials += mem_port

        fsm = FSM(reset_state="FETCH")
        self.submodules += fsm

        i_opcode = mem_port.dat_r[21:23]
        i_address = mem_port.dat_r[20:21]
        i_data_mask = mem_port.dat_r[0:20]

        self.sync += [
            self.bus.adr.eq(i_address),
            self.bus.sel.eq(1),
            self.bus.dat_w.eq(i_data_mask),
        ]

        fsm.act("FETCH", NextState("DECODE"))
        fsm.act("DECODE",
            If(i_opcode == 0b00,
                NextState("END")
            ).Elif(i_opcode == 0b01,
                NextState("WRITE")
            ).Elif(i_opcode == 0b10,
                NextState("WAIT")
            )
        )
        fsm.act("WRITE",
            self.bus.cyc.eq(1),
            self.bus.stb.eq(1),
            self.bus.we.eq(1),
            If(self.bus.ack,
                NextValue(mem_port.adr, mem_port.adr + 1),
                NextState("FETCH")
            )
        )
        fsm.act("WAIT",
            self.bus.cyc.eq(1),
            self.bus.stb.eq(1),
            If(self.bus.ack & ((self.bus.dat_r & i_data_mask) == i_data_mask),
                NextValue(mem_port.adr, mem_port.adr + 1),
                NextState("FETCH")
            )
        )
        fsm.act("END",
            self.done.eq(1)
        )

# SI5251 config (38.4MHz on All Outputs from 25MHz XO) ---------------------------------------------

def i2c_program_38p4(sys_clk_freq):
    i2c_sequence = [
        [(0x60 << 1), 0x0002, 0x33 ],
        [(0x60 << 1), 0x0003, 0x00 ],
        [(0x60 << 1), 0x0004, 0x10 ],
        [(0x60 << 1), 0x0007, 0x01 ],
        [(0x60 << 1), 0x000F, 0x00 ],
        [(0x60 << 1), 0x0010, 0x2F ],
        [(0x60 << 1), 0x0011, 0x8C ],
        [(0x60 << 1), 0x0012, 0x8C ],
        [(0x60 << 1), 0x0013, 0x8C ],
        [(0x60 << 1), 0x0014, 0x8C ],
        [(0x60 << 1), 0x0015, 0x8C ],
        [(0x60 << 1), 0x0016, 0x8C ],
        [(0x60 << 1), 0x0017, 0x8C ],
        [(0x60 << 1), 0x0022, 0x42 ],
        [(0x60 << 1), 0x0023, 0x40 ],
        [(0x60 << 1), 0x0024, 0x00 ],
        [(0x60 << 1), 0x0025, 0x10 ],
        [(0x60 << 1), 0x0026, 0x00 ],
        [(0x60 << 1), 0x0027, 0xF0 ],
        [(0x60 << 1), 0x0028, 0x00 ],
        [(0x60 << 1), 0x0029, 0x00 ],
        [(0x60 << 1), 0x002A, 0x00 ],
        [(0x60 << 1), 0x002B, 0x01 ],
        [(0x60 << 1), 0x002C, 0x00 ],
        [(0x60 << 1), 0x002D, 0x10 ],
        [(0x60 << 1), 0x002E, 0x00 ],
        [(0x60 << 1), 0x002F, 0x00 ],
        [(0x60 << 1), 0x0030, 0x00 ],
        [(0x60 << 1), 0x0031, 0x00 ],
        [(0x60 << 1), 0x005A, 0x00 ],
        [(0x60 << 1), 0x005B, 0x00 ],
        [(0x60 << 1), 0x0095, 0x00 ],
        [(0x60 << 1), 0x0096, 0x00 ],
        [(0x60 << 1), 0x0097, 0x00 ],
        [(0x60 << 1), 0x0098, 0x00 ],
        [(0x60 << 1), 0x0099, 0x00 ],
        [(0x60 << 1), 0x009A, 0x00 ],
        [(0x60 << 1), 0x009B, 0x00 ],
        [(0x60 << 1), 0x00A2, 0x33 ],
        [(0x60 << 1), 0x00A3, 0x2C ],
        [(0x60 << 1), 0x00A4, 0x02 ],
        [(0x60 << 1), 0x00A5, 0x00 ],
        [(0x60 << 1), 0x00B7, 0xD2 ],
    ]

    program = [
        InstWrite(I2C_CONFIG_ADDR, int(sys_clk_freq/1e2)),
    ]
    for subseq in i2c_sequence:
        program += [
            InstWrite(I2C_XFER_ADDR, I2C_START),
            InstWait(I2C_XFER_ADDR, I2C_IDLE),
        ]
        for octet in subseq:
            program += [
                InstWrite(I2C_XFER_ADDR, I2C_WRITE | octet),
                InstWait(I2C_XFER_ADDR, I2C_IDLE),
            ]
        program += [
            InstWrite(I2C_XFER_ADDR, I2C_STOP),
            InstWait(I2C_XFER_ADDR, I2C_IDLE),
        ]
    program += [
        InstEnd(),
    ]
    return program

# SI5351--------------------------------------------------------------------------------------------

class SI5351(Module):
    def __init__(self, pads, programs, sys_clk_freq):
        assert len(programs) == 2
        self.sel  = Signal()
        self.done = Signal()

        # # #

        sel_last   = Signal()
        sel_change = Signal()
        self.sync += sel_last.eq(self.sel)
        self.comb += sel_change.eq(self.sel ^ sel_last)

        # I2C Master.
        i2c_master = I2CMaster(pads)
        i2c_master = ResetInserter()(i2c_master)
        self.submodules.i2c_master = i2c_master
        self.comb += i2c_master.reset.eq(sel_change)

        # Sequencers.
        for i, program in enumerate(programs):
            # Create Sequencer.
            sequencer = Sequencer(program(sys_clk_freq))
            sequencer = ResetInserter()(sequencer)
            self.submodules += sequencer

            # Connect Sequencer.
            self.comb += If(self.sel == i,
                sequencer.bus.connect(i2c_master.bus),
                self.done.eq(sequencer.done),
            )

            # Reset Sequencer (On sel change).
            self.comb += sequencer.reset.eq(sel_change)
