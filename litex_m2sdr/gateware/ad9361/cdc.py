#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc       import MultiReg, GrayCounter
from migen.genlib.fifo      import _FIFOInterface
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.interconnect import stream

# Async FIFO (Registered Flags) --------------------------------------------------------------------

class AsyncFIFORegistered(Module, _FIFOInterface):
    """Asynchronous FIFO with registered readable/writable flags.

    Same interface and ordering semantics as migen's AsyncFIFO. The readable/writable flags are
    registers, computed from the post-increment pointer (q_next) compared against the synchronized
    opposite pointer. A synchronized pointer can change on any cycle, so everything it feeds
    combinationally is a genuine single-cycle path from the synchronizer flops; here it feeds
    exactly one flop, and all memory port address/enable launchers are local registers - short
    cones that close timing at high read/write clock rates (491.52MHz rfic_clk with Oversampling).

    Computing the flags from q_next keeps them exact for the events of their own domain (no
    over-read on drain, no over-write on fill); they see the opposite pointer one cycle late,
    which only makes them conservative (readable asserts one cycle after a write lands, writable
    re-asserts one cycle after a read frees space).

    With register_storage the storage is an array of registers; distributed-LUTRAM write clocks
    have a 1.05ns min low-pulse-width limit (RAMD32) that a 491.52MHz write clock (1.017ns low
    pulse) violates, while register clock enables have no such limit. Use it when the write domain
    runs at the rfic_clk Oversampling rate.
    """
    def __init__(self, width, depth, register_storage=False, registered_read=False):
        _FIFOInterface.__init__(self, width, depth)

        # # #

        depth_bits = log2_int(depth, True)

        produce = ClockDomainsRenamer("write")(GrayCounter(depth_bits+1))
        consume = ClockDomainsRenamer("read")(GrayCounter(depth_bits+1))
        self.submodules += produce, consume
        self.comb += produce.ce.eq(self.writable & self.we)
        if not registered_read:
            self.comb += consume.ce.eq(self.readable & self.re)

        produce_rdomain = Signal(depth_bits+1)
        produce.q.attr.add("no_retiming")
        self.specials += MultiReg(produce.q, produce_rdomain, "read")
        consume_wdomain = Signal(depth_bits+1)
        consume.q.attr.add("no_retiming")
        self.specials += MultiReg(consume.q, consume_wdomain, "write")

        writable = Signal(reset=1)
        if depth_bits == 1:
            self.sync.write += writable.eq((produce.q_next[-1] == consume_wdomain[-1])
                | (produce.q_next[-2] == consume_wdomain[-2]))
        else:
            self.sync.write += writable.eq((produce.q_next[-1] == consume_wdomain[-1])
                | (produce.q_next[-2] == consume_wdomain[-2])
                | (produce.q_next[:-2] != consume_wdomain[:-2]))
        self.comb += self.writable.eq(writable)

        if not registered_read:
            readable = Signal()
            self.sync.read += readable.eq(consume.q_next != produce_rdomain)
            self.comb += self.readable.eq(readable)

        if registered_read:
            # Synchronous-read style for a fast read clock (rfic 491.52MHz with Oversampling): the
            # mux select is the REGISTERED binary read pointer (consume.q_binary), so the read path
            # is reg -> mux -> reg, a single cycle with no timing exception. A prefetch read
            # (words[q_next_binary]) instead puts a comb adder + gray->binary in front of the 128-bit
            # mux, which cannot close single-cycle at this rate. dout lags q_binary by one cycle, so
            # for one cycle right after a read it is stale; readable is held low that cycle (a
            # one-cycle bubble). The consumer pulls at most once per two cycles (the PHY/scatter
            # rate), so the bubble never throttles it. Only register_storage is supported.
            assert register_storage
            words = Array(Signal(width) for _ in range(depth))
            self.sync.write += If(produce.ce, words[produce.q_binary[:-1]].eq(self.din))

            dout = Signal(width)
            self.sync.read += dout.eq(words[consume.q_binary[:-1]])
            self.comb += self.dout.eq(dout)

            # readable is registered so source.valid is not a long combinational cone into the PHY,
            # and is held low for the one stale cycle after each read (the bubble above).
            readable     = Signal()
            consumed     = Signal()
            mem_notempty = Signal()
            self.comb += [
                mem_notempty.eq(consume.q != produce_rdomain),
                consumed.eq(readable & self.re),
                consume.ce.eq(consumed),
                self.readable.eq(readable),
            ]
            self.sync.read += readable.eq(mem_notempty & ~consumed)
        elif register_storage:
            words = Array(Signal(width) for _ in range(depth))
            self.sync.write += If(produce.ce, words[produce.q_binary[:-1]].eq(self.din))
            dout = Signal(width)
            self.sync.read += dout.eq(words[consume.q_next_binary[:-1]])
            self.comb += self.dout.eq(dout)
        else:
            storage = Memory(width, depth)
            self.specials += storage
            wrport = storage.get_port(write_capable=True, clock_domain="write")
            self.specials += wrport
            self.comb += [
                wrport.adr.eq(produce.q_binary[:-1]),
                wrport.dat_w.eq(self.din),
                wrport.we.eq(produce.ce)
            ]
            rdport = storage.get_port(clock_domain="read")
            self.specials += rdport
            self.comb += [
                rdport.adr.eq(consume.q_next_binary[:-1]),
                self.dout.eq(rdport.dat_r)
            ]

# Clock Domain Crossing (Registered Flags) ---------------------------------------------------------

class ClockDomainCrossingRegistered(LiteXModule):
    """stream.ClockDomainCrossing equivalent (async, common rst) built on AsyncFIFORegistered."""
    _instance = 0
    def __init__(self, layout, cd_from="sys", cd_to="sys", depth=4, register_storage=False,
                 registered_read=False):
        self.sink   = stream.Endpoint(layout)
        self.source = stream.Endpoint(layout)

        # # #

        # Create intermediate Clk Domains and generate a common Rst.
        _cd_id = ClockDomainCrossingRegistered._instance
        ClockDomainCrossingRegistered._instance += 1
        _cd_rst  = Signal()
        _cd_from = ClockDomain(f"cdcreg{_cd_id}_from")
        _cd_to   = ClockDomain(f"cdcreg{_cd_id}_to")
        self.clock_domains += _cd_from, _cd_to
        self.comb += [
            _cd_from.clk.eq(ClockSignal(cd_from)),
            _cd_to.clk.eq(  ClockSignal(cd_to)),
            _cd_rst.eq(ResetSignal(cd_from) | ResetSignal(cd_to))
        ]
        self.specials += [
            AsyncResetSynchronizer(_cd_from, _cd_rst),
            AsyncResetSynchronizer(_cd_to,   _cd_rst),
        ]

        # Add Asynchronous FIFO.
        fifo_class = lambda width, depth: AsyncFIFORegistered(width, depth, register_storage,
                                                              registered_read)
        cdc = stream._FIFOWrapper(fifo_class, layout, depth)
        cdc = ClockDomainsRenamer({"write": _cd_from.name, "read": _cd_to.name})(cdc)
        self.submodules += cdc

        # Sink -> AsyncFIFO -> Source.
        self.comb += self.sink.connect(cdc.sink)
        self.comb += cdc.source.connect(self.source)
