#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import re

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *
from litex.soc.cores.icap import ICAP
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

from litepcie.common import dma_layout


DFX_CELL_NAME = "rfic_dfx_region"
DFX_MODULE_NAME = "m2sdr_dfx_datapath"
# Clock region X1Y1: 4000 slices, 80 DSP48E1, 40 RAMB36. Directly below the AD9361 banks
# (34/35 in X1Y2/X1Y3), away from PCIe/GTP (X0Y4) and ICAP (X0Y2).
DEFAULT_DFX_PBLOCK = "SLICE_X84Y50:SLICE_X163Y99"
DFX_DATA_WIDTHS = (64, 128)
# rm_id values reported by the shipped reconfigurable modules.
DFX_RM_IDS = {"passthrough": 0, "loopback": 1, "channel_swap": 2}


def validate_dfx_pblock(value):
    match = re.fullmatch(r"SLICE_X(\d+)Y(\d+):SLICE_X(\d+)Y(\d+)", value)
    if match is None:
        raise ValueError(
            "DFX Pblock must be a single SLICE range, for example "
            f"{DEFAULT_DFX_PBLOCK}."
        )
    x0, y0, x1, y1 = map(int, match.groups())
    if x0 > x1 or y0 > y1:
        raise ValueError("DFX Pblock coordinates must be ordered from low to high.")
    return value


class M2SDRICAP(ICAP):
    """ICAP register access plus a CSR-fed partial-bitstream stream.

    LiteX's regular ICAP and ICAPBitstream cores each instantiate ICAPE2. A
    7-series device only has one usable configuration port, so this combines
    both frontends behind one primitive and arbitrates it in hardware.
    """

    def __init__(self, fifo_depth=16, icap_clk_divider=4):
        self._icap_clk_divider = icap_clk_divider
        ICAP.__init__(self,
            with_csr   = True,
            clk_divider= icap_clk_divider,
            primitive  = "ICAPE2",
            simulation = True,
        )

        self.isolate = Signal(reset=0)

        self._bitstream_data = CSRStorage(32, reset_less=True,
            description="Partial-bitstream word written to the ICAP FIFO.")
        self._bitstream_control = CSRStorage(fields=[
            CSRField("start", size=1, offset=0, pulse=True,
                description="Isolate the DFX region and begin a partial-bitstream transfer."),
            CSRField("finish", size=1, offset=1, pulse=True,
                description="Complete the transfer after all queued words reach ICAP."),
            CSRField("release", size=1, offset=2, pulse=True,
                description="Release isolation after a successful transfer."),
        ])
        self._bitstream_status = CSRStatus(32, fields=[
            CSRField("busy", size=1, offset=0,
                description="A partial-bitstream transfer is active or draining."),
            CSRField("done", size=1, offset=1,
                description="All accepted words have reached ICAP."),
            CSRField("isolated", size=1, offset=2,
                description="The reconfigurable datapath is isolated from static logic."),
            CSRField("ready", size=1, offset=3,
                description="The FIFO can accept another 32-bit word."),
        ])
        self._bitstream_words = CSRStatus(32,
            description="Number of words accepted for the current transfer.")

        raw_active = Signal(reset=0)
        finishing = Signal(reset=0)
        done = Signal(reset=0)
        accepted_words = Signal(32)
        pending_words = Signal(32)

        fifo = stream.AsyncFIFO([("data", 32)], fifo_depth)
        fifo = ClockDomainsRenamer({"write": "sys", "read": "icap"})(fifo)
        self.submodules.bitstream_fifo = fifo

        raw_active_icap = Signal()
        self.specials += MultiReg(raw_active, raw_active_icap, odomain="icap")

        write_fire = Signal()
        self.comb += [
            fifo.sink.valid.eq(raw_active & self._bitstream_data.wr_stb),
            fifo.sink.data.eq(self._bitstream_data.storage),
            write_fire.eq(fifo.sink.valid & fifo.sink.ready),
            fifo.source.ready.eq(raw_active_icap),
        ]

        consumed = PulseSynchronizer("icap", "sys")
        self.submodules.bitstream_consumed = consumed
        self.comb += consumed.i.eq(fifo.source.valid & fifo.source.ready)

        self.sync += [
            If(self._bitstream_control.fields.start & ~raw_active & (pending_words == 0),
                raw_active.eq(1),
                finishing.eq(0),
                done.eq(0),
                self.isolate.eq(1),
                accepted_words.eq(0),
            ),
            If(self._bitstream_control.fields.finish & raw_active,
                finishing.eq(1),
            ),
            If(finishing & (pending_words == 0),
                raw_active.eq(0),
                finishing.eq(0),
                done.eq(1),
            ),
            If(self._bitstream_control.fields.release & ~raw_active & ~finishing,
                self.isolate.eq(0),
                done.eq(0),
            ),
            If(write_fire,
                accepted_words.eq(accepted_words + 1),
            ),
            Case(Cat(write_fire, consumed.o), {
                0b01: pending_words.eq(pending_words + 1),
                0b10: pending_words.eq(pending_words - 1),
            }),
        ]

        self.comb += [
            self._bitstream_status.fields.busy.eq(raw_active | finishing),
            self._bitstream_status.fields.done.eq(done),
            self._bitstream_status.fields.isolated.eq(self.isolate),
            self._bitstream_status.fields.ready.eq(raw_active & fifo.sink.ready),
            self._bitstream_words.status.eq(accepted_words),
        ]

        # The inherited ICAP register FSM owns the primitive while a raw
        # transfer is idle. During DFX, FIFO words are passed through exactly
        # as emitted by Vivado, with the per-byte bit reversal ICAPE2 expects.
        primitive_i = Signal(32)
        primitive_o = Signal(32)
        primitive_csib = Signal()
        primitive_rdwrb = Signal()
        raw_i = Signal(32, reset=0xffffffff)
        raw_csib = Signal(reset=1)

        self.comb += [
            raw_csib.eq(~fifo.source.valid),
            raw_i.eq(Mux(fifo.source.valid, fifo.source.data, 0xffffffff)),
            primitive_csib.eq(Mux(raw_active_icap, raw_csib, self._csib)),
            primitive_rdwrb.eq(Mux(raw_active_icap, 0, self._rdwrb)),
            primitive_i.eq(Mux(raw_active_icap, raw_i, self._i)),
            self._o.eq(Cat(*[primitive_o[8*i:8*(i + 1)][::-1] for i in range(4)])),
        ]
        self.specials += Instance("ICAPE2",
            p_ICAP_WIDTH = "X32",
            i_CLK   = ClockSignal("icap"),
            i_CSIB  = primitive_csib,
            i_RDWRB = primitive_rdwrb,
            i_I     = Cat(*[primitive_i[8*i:8*(i + 1)][::-1] for i in range(4)]),
            o_O     = primitive_o,
        )

    def add_timing_constraints(self, platform, sys_clk_freq, sys_clk):
        platform.add_period_constraint(
            self.cd_icap.clk,
            self._icap_clk_divider*1e9/sys_clk_freq,
        )
        platform.add_false_path_constraints(self.cd_icap.clk, sys_clk)


class DFXRFICDatapath(LiteXModule):
    """Static isolator and CSR shell around the reconfigurable RFIC-plane datapath.

    Takes the place of BaseSoC.add_rfic_datapath(): the reconfigurable module (RM) sits between
    the host streams (after header/loopback) and the AD9361, on raw I/Q sample words. Every RM
    implements the same ports: four streams of `data_width`, a 32-bit RM-defined control word
    and an 8-bit RM identifier.
    """

    def __init__(self, platform, isolate, default_rm_source, data_width=64):
        if data_width not in DFX_DATA_WIDTHS:
            raise ValueError(f"The M2SDR DFX datapath supports {DFX_DATA_WIDTHS}-bit streams.")

        # Registered partition boundary: no combinational path may cross the region, since the
        # static routing is locked after the default RM and cannot be re-timed for another RM
        # (a loopback RM otherwise links TX and RX header logic through the region).
        for name in ("tx_in", "tx_out", "rx_in", "rx_out"):
            setattr(self, name, stream.Buffer(dma_layout(data_width), pipe_valid=True, pipe_ready=True))
        self.tx_sink   = self.tx_in.sink     # Host TX samples.
        self.tx_source = self.tx_out.source  # -> RFIC.
        self.rx_sink   = self.rx_in.sink     # RFIC RX samples.
        self.rx_source = self.rx_out.source  # -> Host RX.
        tx_sink, tx_source = self.tx_in.source, self.tx_out.sink
        rx_sink, rx_source = self.rx_in.source, self.rx_out.sink

        self._control = CSRStorage(32, description="Control word, defined by the active RM.")
        self._status  = CSRStatus(fields=[
            CSRField("isolated", size=1, offset=0,
                description="The static shell is isolating the reconfigurable region."),
            CSRField("rm_id", size=8, offset=8,
                description="Identifier of the active RM (0: passthrough, 1: loopback)."),
        ])

        platform.add_source(default_rm_source)

        rm = {name: stream.Endpoint(dma_layout(data_width)) for name in ("tx", "rx")}
        rm_tx_sink_ready = Signal()
        rm_rx_sink_ready = Signal()
        rm_id = Signal(8)

        self.specials += Instance(DFX_MODULE_NAME,
            name=DFX_CELL_NAME,
            attr={("keep_hierarchy", "yes")},
            p_DATA_WIDTH=data_width,
            i_control=self._control.storage,

            i_tx_sink_valid=tx_sink.valid,
            o_tx_sink_ready=rm_tx_sink_ready,
            i_tx_sink_data=tx_sink.data,
            i_tx_sink_first=tx_sink.first,
            i_tx_sink_last=tx_sink.last,

            o_tx_source_valid=rm["tx"].valid,
            i_tx_source_ready=tx_source.ready,
            o_tx_source_data=rm["tx"].data,
            o_tx_source_first=rm["tx"].first,
            o_tx_source_last=rm["tx"].last,

            i_rx_sink_valid=rx_sink.valid,
            o_rx_sink_ready=rm_rx_sink_ready,
            i_rx_sink_data=rx_sink.data,
            i_rx_sink_first=rx_sink.first,
            i_rx_sink_last=rx_sink.last,

            o_rx_source_valid=rm["rx"].valid,
            i_rx_source_ready=rx_source.ready,
            o_rx_source_data=rm["rx"].data,
            o_rx_source_first=rm["rx"].first,
            o_rx_source_last=rm["rx"].last,

            o_rm_id=rm_id,
        )

        self.comb += [
            tx_sink.ready.eq(Mux(isolate, 0, rm_tx_sink_ready)),
            # RFIC samples are intentionally drained while the region is
            # unavailable; back-pressuring the converter can wedge its path.
            rx_sink.ready.eq(Mux(isolate, 1, rm_rx_sink_ready)),
            self._status.fields.isolated.eq(isolate),
            self._status.fields.rm_id.eq(rm_id),
        ]
        for name, source in (("tx", tx_source), ("rx", rx_source)):
            self.comb += [
                source.valid.eq(Mux(isolate, 0, rm[name].valid)),
                source.data.eq(Mux(isolate, 0, rm[name].data)),
                source.first.eq(Mux(isolate, 0, rm[name].first)),
                source.last.eq(Mux(isolate, 0, rm[name].last)),
            ]


def dfx_rm_tcl(build_name, rm_name, sources, part, data_width):
    """Vivado Tcl implementing one RM against the locked static checkpoint.

    Only the RM is synthesized (out of context) and placed/routed: the static design,
    its partition pins and routing come from ``{build_name}_static_routed.dcp``, which
    the full build writes after ``lock_design -level routing``. ``pr_verify`` against
    the full build's passthrough configuration then proves the static part is
    identical to the image the board runs. Shared by the full build and
    ``scripts/dfx_rm_build.py``.
    """
    if not re.fullmatch(r"[a-z][a-z0-9_]*", rm_name):
        raise ValueError(f"RM name must match [a-z][a-z0-9_]*, got {rm_name!r}")
    if data_width not in DFX_DATA_WIDTHS:
        raise ValueError(f"DFX data width must be one of {DFX_DATA_WIDTHS}, got {data_width}")
    find_cell = f"set dfx_cell [lindex [get_cells -hierarchical -filter {{NAME =~ *{DFX_CELL_NAME}}}] 0]"
    rm = f"{build_name}_{rm_name}"
    return [
        f"create_project -in_memory -part {part}",
        *[f"read_verilog {{{os.path.abspath(source)}}}" for source in sources],
        f"synth_design -mode out_of_context -top {DFX_MODULE_NAME} -part {part} -generic DATA_WIDTH={data_width}",
        f"write_checkpoint -force {rm}_synth.dcp",
        "close_project",

        f"open_checkpoint {build_name}_static_routed.dcp",
        find_cell,
        # The black box keeps the partition ports: refuse an RM built for the other width.
        f"set dfx_width [llength [get_pins -of_objects $dfx_cell -filter {{REF_PIN_NAME =~ tx_sink_data*}}]]",
        f"if {{$dfx_width != {data_width}}} {{ error \"Static region is $dfx_width bits wide, RM was built for {data_width}\" }}",
        f"read_checkpoint -cell $dfx_cell {rm}_synth.dcp",
        "opt_design",
        "place_design",
        "route_design",
        f"write_checkpoint -force {rm}_routed.dcp",
        f"report_drc -file {rm}_drc.rpt",
        f"report_timing_summary -datasheet -max_paths 10 -file {rm}_timing.rpt",
        f"report_utilization -pblocks [get_pblocks pblock_rfic_dfx] -file {rm}_pblock_utilization.rpt",
        "set dfx_wns [get_property SLACK [get_timing_paths -delay_type max -max_paths 1]]",
        "set dfx_whs [get_property SLACK [get_timing_paths -delay_type min -max_paths 1]]",
        f"if {{$dfx_wns < 0 || $dfx_whs < 0}} {{ error \"RM {rm_name} misses timing: WNS $dfx_wns WHS $dfx_whs\" }}",
        # Static placement, routing and partition pins must equal the full build's.
        f"pr_verify -full_check {build_name}_passthrough_routed.dcp {rm}_routed.dcp",
        # read_checkpoint/pr_verify invalidate Tcl handles: resolve the RM again.
        find_cell,
        f"write_bitstream -force -bin_file -cell $dfx_cell {rm}_partial.bit",
        "close_design",
    ]


def _litex_format_escape(line):
    """additional_commands go through str.format(build_name=...): escape Tcl braces."""
    return line.replace("{", "{{").replace("}", "}}").replace("{{build_name}}", "{build_name}")


def add_dfx_build_commands(platform, pblock, alternate_rm_source, data_width):
    """Extend LiteX's Vivado flow: default (passthrough) config, then the alternate RM."""

    pblock = validate_dfx_pblock(pblock)
    find_cell = f"set dfx_cell [lindex [get_cells -hierarchical -filter {{{{NAME =~ *{DFX_CELL_NAME}}}}}] 0]"

    platform.toolchain.pre_optimize_commands += [
        f"set dfx_cells [get_cells -hierarchical -filter {{{{NAME =~ *{DFX_CELL_NAME}}}}}]",
        "if {{[llength $dfx_cells] != 1}} {{ error \"Expected exactly one M2SDR DFX region, found [llength $dfx_cells]\" }}",
        "set dfx_cell [lindex $dfx_cells 0]",
        "set_property HD.RECONFIGURABLE TRUE $dfx_cell",
        "create_pblock pblock_rfic_dfx",
        "add_cells_to_pblock [get_pblocks pblock_rfic_dfx] $dfx_cell",
        f"resize_pblock [get_pblocks pblock_rfic_dfx] -add {{{{{pblock}}}}}",
        "set_property SNAPPING_MODE ON [get_pblocks pblock_rfic_dfx]",
        # Materialize every site type covered by the frame-aligned rectangle.
        # Otherwise Vivado derives these resources but emits HDPR-45 because
        # only the user-supplied SLICE range is present in GRID_RANGES.
        "resize_pblock [get_pblocks pblock_rfic_dfx] -add [get_property DERIVED_RANGES [get_pblocks pblock_rfic_dfx]]",
        "set_property RESET_AFTER_RECONFIG TRUE [get_pblocks pblock_rfic_dfx]",
        "set_property CONTAIN_ROUTING TRUE [get_pblocks pblock_rfic_dfx]",
        "set_property EXCLUDE_PLACEMENT TRUE [get_pblocks pblock_rfic_dfx]",
    ]

    platform.toolchain.additional_commands += [
        find_cell,
        "write_checkpoint -force {build_name}_passthrough_routed.dcp",
        "write_checkpoint -force -cell $dfx_cell {build_name}_passthrough_rm_routed.dcp",
        "write_bitstream -force -bin_file -cell $dfx_cell {build_name}_passthrough_partial.bit",
        # Locked static checkpoint: later RMs are implemented against it only
        # (scripts/dfx_rm_build.py), the static part is never rebuilt.
        "update_design -black_box -cell $dfx_cell",
        "lock_design -level routing",
        "write_checkpoint -force {build_name}_static_routed.dcp",
        "close_project",
    ] + [_litex_format_escape(line) for line in dfx_rm_tcl(
        build_name = "{build_name}",
        rm_name    = "loopback",
        sources    = [alternate_rm_source],
        part       = platform.device,
        data_width = data_width,
    )]
