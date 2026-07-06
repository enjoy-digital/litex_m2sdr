#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *
from litex.gen.genlib.misc import WaitTimer
from litex.soc.interconnect import stream, wishbone


# S7 PCIe Timing Constraints ----------------------------------------------------------------------

S7_PCIE_PHY_CLOCKS = (
    "*s7pciephy_clkout0*",
    "*s7pciephy_clkout1*",
    "*s7pciephy_clkout2*",
    "*s7pciephy_clkout3*",
    "*s7pciephy_clkout4*",
    "*s7pciephy_clkout5*",
)


def _add_guarded_false_path(platform, clk0, clk1):
    clk0 = "{{" + clk0 + "}}"
    clk1 = "{{" + clk1 + "}}"
    platform.toolchain.pre_placement_commands += [
        f"set _pcie_clk0 [get_clocks -quiet {clk0}]",
        f"set _pcie_clk1 [get_clocks -quiet {clk1}]",
        "if {{[llength $_pcie_clk0] && [llength $_pcie_clk1]}} {{",
        "    set_false_path -from $_pcie_clk0 -to $_pcie_clk1",
        "}}",
        "unset -nocomplain _pcie_clk0 _pcie_clk1",
    ]


def _add_guarded_false_paths(platform, clk0, clk1):
    _add_guarded_false_path(platform, clk0, clk1)
    _add_guarded_false_path(platform, clk1, clk0)


def _add_s7_pcie_pclk_mux_constraints(platform, pcie_lanes):
    # LitePCIe's S7 PHY selects the PIPE pclk through a BUFGCTRL. Match the
    # hard-IP clocking with generated clocks on the mux output.
    platform.add_platform_command(
        "set_false_path -to [get_pins -quiet BUFGCTRL/S0]\n"
        "set_false_path -to [get_pins -quiet BUFGCTRL/S1]"
    )

    commands = [
        "set _pcie_pclk_i0 [get_pins -quiet BUFGCTRL/I0]",
        "set _pcie_pclk_o [get_pins -quiet BUFGCTRL/O]",
        "if {{[llength $_pcie_pclk_i0] && [llength $_pcie_pclk_o]}} {{",
        "    create_generated_clock -name clk_125mhz_mux_phy -source $_pcie_pclk_i0 -divide_by 1 $_pcie_pclk_o",
        "}}",
    ]

    # In x1 mode the mux output is the 125MHz PIPE pclk. Adding the unused
    # 250MHz output clock over-constrains the hard-IP timing checks.
    if pcie_lanes > 1:
        commands += [
            "set _pcie_pclk_i1 [get_pins -quiet BUFGCTRL/I1]",
            "set _pcie_pclk250_master [get_clocks -quiet -of_objects [get_nets -quiet {{*s7pciephy_clkout1*}}]]",
            "if {{[llength $_pcie_pclk_i1] && [llength $_pcie_pclk_o] && [llength $_pcie_pclk250_master]}} {{",
            "    create_generated_clock -name clk_250mhz_mux_phy -source $_pcie_pclk_i1 -divide_by 1 -add -master_clock $_pcie_pclk250_master $_pcie_pclk_o",
            "}}",
            "set _pcie_pclk125 [get_clocks -quiet clk_125mhz_mux_phy]",
            "set _pcie_pclk250 [get_clocks -quiet clk_250mhz_mux_phy]",
            "if {{[llength $_pcie_pclk125] && [llength $_pcie_pclk250]}} {{",
            "    set_clock_groups -name pcieclkmux -physically_exclusive -group $_pcie_pclk125 -group $_pcie_pclk250",
            "}}",
        ]

    commands += [
        "set _sys_clk [get_clocks -quiet {{*crg*clkout0*}}]",
        "set _pcie_pclk_clks [get_clocks -quiet {{clk_125mhz_mux_phy clk_250mhz_mux_phy}}]",
        "if {{[llength $_sys_clk] && [llength $_pcie_pclk_clks]}} {{",
        "    set_false_path -from $_sys_clk -to $_pcie_pclk_clks",
        "    set_false_path -from $_pcie_pclk_clks -to $_sys_clk",
        "}}",
        "unset -nocomplain _pcie_pclk_i0 _pcie_pclk_i1 _pcie_pclk_o",
        "unset -nocomplain _pcie_pclk125 _pcie_pclk250 _pcie_pclk250_master",
        "unset -nocomplain _sys_clk _pcie_pclk_clks",
    ]
    platform.toolchain.pre_placement_commands += commands


def add_s7_pcie_timing_constraints(platform, pcie_lanes):
    _add_s7_pcie_pclk_mux_constraints(platform, pcie_lanes)

    # The PHY user clocks are CDC-only from sys, and the 125MHz/250MHz user-clock
    # alternatives are muxed inside the PCIe IP.
    for pcie_clk in S7_PCIE_PHY_CLOCKS:
        _add_guarded_false_paths(platform, "*crg*clkout0*", pcie_clk)

    _add_guarded_false_paths(platform, "*s7pciephy_clkout0*", "*s7pciephy_clkout1*")


# PCIe Link Reset Workaround -----------------------------------------------------------------------

class PCIeLinkResetWorkaround(LiteXModule):
    """
    Periodically toggles PCIe reset until link-up is observed.

    Behavior:
    - If link is down on a timer tick, assert reset low.
    - On next timer tick, release reset high.
    - Once link is up, keep reset released.
    """
    def __init__(self, link_up, sys_clk_freq, interval_s=10e-3, interval_cycles=None):
        self.rst_n = Signal(reset=1)

        # # #

        # Synchronize link status into sys domain.
        link_up_sys = Signal()
        self.specials += MultiReg(link_up, link_up_sys)

        # Retry timer.
        if interval_cycles is None:
            interval_cycles = max(1, int(interval_s * sys_clk_freq))
        self.timer = timer = WaitTimer(interval_cycles)
        self.comb += timer.wait.eq(~timer.done)

        # Reset sequencing:
        # - If currently in reset, release it.
        # - Else, if link is still down, assert reset.
        self.sync += If(timer.done,
            If(~self.rst_n,
                self.rst_n.eq(1)
            ).Elif(~link_up_sys,
                self.rst_n.eq(0)
            )
        )


# PCIe Wishbone Slave With Burst Reads -------------------------------------------------------------

class LitePCIeWishboneBurstReadSlave(LiteXModule):
    def __init__(self, endpoint, address_width=32, data_width=32, addressing="word",
                 read_burst_dwords=128):
        assert data_width == 32

        self.bus = bus = wishbone.Interface(
            address_width = address_width,
            data_width    = data_width,
            addressing    = addressing,
        )

        # # #

        # This bridge is used by the SATA host-memory DMA path, which performs
        # naturally aligned sequential reads from the kernel coherent buffer.
        read_burst_dwords = min(read_burst_dwords, 512//(data_width//8))
        ashift            = {"byte": 0, "word": log2_int(data_width//8)}[addressing]
        pcie_data_width   = endpoint.phy.data_width

        # Get Master port from Crossbar.
        port = endpoint.crossbar.get_master_port()

        # Completion data path.
        cmp_data = stream.Endpoint([("data", pcie_data_width)])
        conv     = ResetInserter()(stream.Converter(nbits_from=pcie_data_width, nbits_to=data_width))
        fifo     = ResetInserter()(stream.SyncFIFO([("data", data_width)], read_burst_dwords*2))
        self.conv = conv
        self.fifo = fifo
        self.comb += [
            cmp_data.connect(conv.sink),
            conv.source.connect(fifo.sink),
        ]

        store_rx = Signal()
        drop_rx  = Signal()
        self.comb += [
            cmp_data.valid.eq(port.sink.valid & store_rx),
            cmp_data.data.eq(port.sink.dat),
            port.sink.ready.eq(Mux(store_rx, cmp_data.ready, drop_rx)),
        ]

        read_base = Signal(address_width)
        next_addr = Signal(address_width)
        rx_active = Signal()
        clear_rx  = Signal()
        start_rx  = Signal()

        self.sync += [
            If(clear_rx,
                rx_active.eq(0)
            ).Elif(start_rx,
                rx_active.eq(1)
            ).Elif(port.sink.valid & port.sink.ready & port.sink.last & port.sink.end,
                rx_active.eq(0)
            )
        ]

        # Request common fields.
        req_adr = Signal(address_width)
        self.comb += [
            req_adr.eq(read_base << ashift),
            port.source.channel.eq(port.channel),
            port.source.req_id.eq(endpoint.phy.id),
            port.source.tag.eq(0),
            port.source.first.eq(1),
            port.source.last.eq(1),
            port.source.dat.eq(bus.dat_w),
        ]

        # Timeouts (cycle-count guards so a lost PCIe request/completion cannot stall the
        # Wishbone bus forever; on timeout the access is acked with bus.err set).
        self.req_timeout = req_timeout = WaitTimer(2**16) # Issuing the read/write request.
        self.cmp_timeout = cmp_timeout = WaitTimer(2**18) # Waiting for the first completion.

        # FSM.
        fsm = FSM(reset_state="IDLE")
        self.fsm = fsm

        fsm.act("IDLE",
            If(rx_active,
                store_rx.eq(1)
            ),
            If(bus.stb & bus.cyc,
                If(bus.we,
                    NextState("ISSUE-WRITE")
                ).Else(
                    If(fifo.source.valid & (bus.adr == next_addr),
                        bus.dat_r.eq(fifo.source.data),
                        bus.ack.eq(1),
                        fifo.source.ready.eq(1),
                        NextValue(next_addr, next_addr + 1),
                    ).Elif(rx_active,
                        NextState("DRAIN-READ")
                    ).Else(
                        fifo.reset.eq(1),
                        conv.reset.eq(1),
                        NextValue(read_base, bus.adr),
                        NextValue(next_addr, bus.adr),
                        NextState("ISSUE-READ")
                    )
                )
            )
        )
        fsm.act("DRAIN-READ",
            drop_rx.eq(1),
            fifo.reset.eq(1),
            conv.reset.eq(1),
            If(~rx_active,
                clear_rx.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("ISSUE-READ",
            req_timeout.wait.eq(1),
            port.source.valid.eq(1),
            port.source.we.eq(0),
            port.source.adr.eq(req_adr),
            port.source.len.eq(read_burst_dwords),
            If(port.source.ready,
                start_rx.eq(1),
                NextState("WAIT-FIRST")
            ).Elif(req_timeout.done,
                bus.ack.eq(1),
                bus.err.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("WAIT-FIRST",
            store_rx.eq(1),
            cmp_timeout.wait.eq(1),
            If(fifo.source.valid,
                bus.dat_r.eq(fifo.source.data),
                bus.ack.eq(1),
                fifo.source.ready.eq(1),
                NextValue(next_addr, read_base + 1),
                NextState("IDLE")
            ).Elif(cmp_timeout.done,
                clear_rx.eq(1),
                bus.ack.eq(1),
                bus.err.eq(1),
                NextState("IDLE")
            )
        )
        fsm.act("ISSUE-WRITE",
            req_timeout.wait.eq(1),
            port.source.valid.eq(1),
            port.source.we.eq(1),
            port.source.adr.eq(bus.adr << ashift),
            port.source.len.eq(1),
            If(port.source.ready | req_timeout.done,
                bus.ack.eq(1),
                bus.err.eq(req_timeout.done),
                NextState("IDLE")
            )
        )
