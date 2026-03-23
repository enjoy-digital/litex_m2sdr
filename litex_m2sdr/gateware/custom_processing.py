#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os

from migen import *

from litex.gen import *
from litex.soc.interconnect import stream

from litepcie.common import dma_layout


class TXRXCustomProcessing(LiteXModule):
    def __init__(self, platform=None, data_width=64):
        self.tx_sink   = stream.Endpoint(dma_layout(data_width))
        self.tx_source = stream.Endpoint(dma_layout(data_width))
        self.rx_sink   = stream.Endpoint(dma_layout(data_width))
        self.rx_source = stream.Endpoint(dma_layout(data_width))

        if platform is not None:
            platform.add_source(self.get_netlist_path())

        self.specials += Instance("custom_processing_passthrough",
            p_DATA_WIDTH=data_width,
            i_clk=ClockSignal("sys"),
            i_rst=ResetSignal("sys"),

            i_s_axis_tx_tvalid=self.tx_sink.valid,
            o_s_axis_tx_tready=self.tx_sink.ready,
            i_s_axis_tx_tdata=self.tx_sink.data,
            i_s_axis_tx_tlast=self.tx_sink.last,
            o_m_axis_tx_tvalid=self.tx_source.valid,
            i_m_axis_tx_tready=self.tx_source.ready,
            o_m_axis_tx_tdata=self.tx_source.data,
            o_m_axis_tx_tlast=self.tx_source.last,

            i_s_axis_rx_tvalid=self.rx_sink.valid,
            o_s_axis_rx_tready=self.rx_sink.ready,
            i_s_axis_rx_tdata=self.rx_sink.data,
            i_s_axis_rx_tlast=self.rx_sink.last,
            o_m_axis_rx_tvalid=self.rx_source.valid,
            i_m_axis_rx_tready=self.rx_source.ready,
            o_m_axis_rx_tdata=self.rx_source.data,
            o_m_axis_rx_tlast=self.rx_source.last,
        )

        # LiteX uses a ``first`` qualifier but AXI-Stream does not define it,
        # so keep it on the Python side of the wrapper.
        self.comb += [
            self.tx_source.first.eq(self.tx_sink.first),
            self.rx_source.first.eq(self.rx_sink.first),
        ]

    @staticmethod
    def get_netlist_path():
        return os.path.join(
            os.path.dirname(__file__),
            "verilog",
            "custom_processing_passthrough.v",
        )
