#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3 (SuperSpeed 5Gbps) host interface through USB3-PIPE.

The GTP of a baseboard SFP cage is used directly as a USB3 PHY (with an SFP-to-USB3 adapter, ex:
Xillybus SFP2USB), the USB3 Device Core is implemented in the fabric:
- EP0: CSRs/SoC bus access over vendor requests (USB3EP0Wishbone, LiteX CommUSB compatible:
  litex_server --usb).
- EP1 Bulk IN : RFIC RX samples (Crossbar -> Host), 8KB frames (header + samples) as with PCIe.
- EP2 Bulk OUT: RFIC TX samples (Host -> Crossbar), primed FIFO before the RFIC.

https://github.com/enjoy-digital/usb3_pipe
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litepcie.common import dma_layout

# Constants ----------------------------------------------------------------------------------------

USB3_VID       = 0x1d50
USB3_PID       = 0x605a
USB3_RX_EPT    = 1 # Bulk IN  (RFIC RX -> Host).
USB3_TX_EPT    = 2 # Bulk OUT (Host -> RFIC TX).
USB3_MAX_BURST = 16

# QPLL Channel Adapter -----------------------------------------------------------------------------

class USB3QPLLChannel:
    """LiteEth SharedQPLL channel (index/reset/lock/clk/refclk) seen as a LiteICLink GTPQuadPLL
    (channel/config) by the USB3-PIPE A7USB3SerDes, configured for 5Gbps (2.5GHz VCO, D=1)."""
    def __init__(self, qpll_channel, refclk_freq):
        self.channel = qpll_channel.index
        self.reset   = qpll_channel.reset
        self.lock    = qpll_channel.lock
        self.clk     = qpll_channel.clk
        self.refclk  = qpll_channel.refclk
        self.config  = {"linerate": 5e9, "d": 1, "clkin": refclk_freq}

# USB3 SFP Pads ------------------------------------------------------------------------------------

def get_usb3_sfp_pads(sfp_pads):
    """Baseboard SFP pads (txp/txn/rxp/rxn) as USB3-PIPE TX/RX pads (p/n)."""
    tx_pads = Record([("p", 1), ("n", 1)])
    rx_pads = Record([("p", 1), ("n", 1)])
    tx_pads.p, tx_pads.n = sfp_pads.txp, sfp_pads.txn
    rx_pads.p, rx_pads.n = sfp_pads.rxp, sfp_pads.rxn
    return tx_pads, rx_pads

# USB3 RX Streamer (Crossbar -> Bulk IN) -----------------------------------------------------------

class USB3RXStreamer(LiteXModule):
    """RFIC RX stream (dma_layout) to the Bulk IN Endpoint (32-bit, first byte on data[7:0]).

    Disabled: stream drained (no back-pressure on the RFIC datapath). Enable: buffered packets of a
    previous run flushed (BulkIN.flush) so the host only receives frames of the current run.
    """
    def __init__(self, data_width=64, fifo_depth=4096):
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width))
        self.source = source = stream.Endpoint([("data", 32), ("be", 4)])
        self.flush  = Signal() # o: to BulkIN.flush.

        self.enable = Signal() # i

        # # #

        # FIFO (absorbs the host scheduling jitter in front of the Bulk IN slots).
        self.fifo = fifo = ResetInserter()(stream.SyncFIFO(dma_layout(data_width), fifo_depth,
            buffered=True))

        # Converter (first 32-bit word from data[31:0], as the PCIe DMA in host memory).
        self.conv = conv = ResetInserter()(stream.Converter(data_width, 32))
        self.comb += [
            fifo.reset.eq(~self.enable),
            conv.reset.eq(~self.enable),
        ]

        enable_d = Signal()
        self.sync += enable_d.eq(self.enable)
        self.comb += [
            self.flush.eq(self.enable & ~enable_d),
            If(self.enable,
                sink.connect(fifo.sink, omit={"first", "last"}),
            ).Else(
                sink.ready.eq(1),
            ),
            fifo.source.connect(conv.sink, omit={"first", "last"}),
            conv.source.connect(source, omit={"first", "last", "valid_token_count"}),
            source.be.eq(0b1111),
        ]

# USB3 TX Streamer (Bulk OUT -> Crossbar) ----------------------------------------------------------

class USB3TXStreamer(LiteXModule):
    """Bulk OUT Endpoint (32-bit) to the RFIC TX stream (dma_layout), through a FIFO primed (half
    full) before being forwarded so the RFIC is not underrun at start (as the Ethernet streamer).

    Disabled: Bulk OUT data drained, FIFO kept empty.
    """
    def __init__(self, data_width=64, fifo_depth=(256*1024)//8):
        self.sink   = sink   = stream.Endpoint([("data", 32), ("be", 4)])
        self.source = source = stream.Endpoint(dma_layout(data_width))

        self.enable  = Signal() # i
        self.started = Signal() # o

        # # #

        # Converter (first 32-bit word to data[31:0]).
        self.conv = conv = ResetInserter()(stream.Converter(32, data_width))

        # FIFO.
        self.fifo = fifo = ResetInserter()(stream.SyncFIFO(dma_layout(data_width), fifo_depth,
            buffered=True))
        self.comb += [
            conv.reset.eq(~self.enable),
            fifo.reset.eq(~self.enable),
        ]

        self.comb += [
            If(self.enable,
                sink.connect(conv.sink, omit={"be", "first", "last"}),
            ).Else(
                sink.ready.eq(1),
            ),
            conv.source.connect(fifo.sink, omit={"valid_token_count"}),
        ]

        # Priming: forward once half full, until empty (underrun) or disabled.
        primed = Signal()
        self.comb += primed.eq(fifo.level >= fifo_depth//2)
        self.sync += [
            If(~self.enable,
                self.started.eq(0)
            ).Elif(primed,
                self.started.eq(1)
            ).Elif(fifo.level == 0,
                self.started.eq(0)
            )
        ]
        self.comb += [
            fifo.source.connect(source, omit={"valid", "ready", "first", "last"}),
            source.valid.eq(fifo.source.valid & self.started),
            fifo.source.ready.eq(source.ready & self.started),
        ]

# USB3 ---------------------------------------------------------------------------------------------

class USB3(LiteXModule):
    """USB3 host interface: SerDes (on a SharedQPLL channel) + PHY + Device Core (EP0 Wishbone bridge,
    Bulk IN/OUT sample streams).

    bus   : Wishbone master (EP0 vendor requests), to add to the SoC bus.
    sink  : RFIC RX stream to the host (Crossbar demux output).
    source: RFIC TX stream from the host (Crossbar mux input).
    """
    def __init__(self, platform, sfp_pads, qpll_channel, refclk_freq, sys_clk, sys_clk_freq,
        data_width = 64,
        vid        = USB3_VID,
        pid        = USB3_PID,
        serial     = "0001",
        with_debug = False):
        from usb3_pipe             import A7USB3SerDes, USB3PHY
        from usb3_core.device      import USB3DeviceCore
        from usb3_core.bulk        import USB3BulkIN, USB3BulkOUT
        from usb3_core.descriptors import bulk_descriptors
        from usb3_core.wishbone    import USB3EP0Wishbone

        assert sys_clk_freq >= 125e6, "USB3 requires sys_clk_freq >= 125MHz."

        self.sink   = stream.Endpoint(dma_layout(data_width))
        self.source = stream.Endpoint(dma_layout(data_width))

        self._control = CSRStorage(fields=[
            CSRField("rx_enable", size=1, offset=0, description="Enable RX stream (RFIC -> Host, Bulk IN), flushes previous data."),
            CSRField("tx_enable", size=1, offset=1, description="Enable TX stream (Host -> RFIC, Bulk OUT)."),
        ], description="USB3 streams control.")
        self._status = CSRStatus(fields=[
            CSRField("ready",      size=1, offset=0, description="SerDes ready."),
            CSRField("link_up",    size=1, offset=1, description="Link in U0."),
            CSRField("tx_started", size=1, offset=2, description="TX FIFO primed, forwarding to the RFIC."),
        ], description="USB3 status.")

        # # #

        # SerDes -----------------------------------------------------------------------------------
        tx_pads, rx_pads = get_usb3_sfp_pads(sfp_pads)
        self.serdes = serdes = A7USB3SerDes(platform,
            sys_clk      = sys_clk,
            sys_clk_freq = sys_clk_freq,
            refclk_pads  = None,
            refclk_freq  = refclk_freq,
            tx_pads      = tx_pads,
            rx_pads      = rx_pads,
            pll          = USB3QPLLChannel(qpll_channel, refclk_freq),
        )
        # TX polarity: swaps of the M2SDR and Baseboard compensate (as Ethernet: tx_polarity=0).
        # RX polarity: detected by the LTSSM (TS1/TS2 inversion).

        # PHY --------------------------------------------------------------------------------------
        self.phy = phy = USB3PHY(serdes, sys_clk_freq, role="device")
        if with_debug:
            from usb3_pipe.debug import USB3PHYDebug
            self.debug = USB3PHYDebug(phy, serdes, sys_clk_freq)

        # Device Core ------------------------------------------------------------------------------
        descriptors = bulk_descriptors(
            in_ept       = USB3_RX_EPT,
            out_ept      = USB3_TX_EPT,
            max_burst    = USB3_MAX_BURST,
            vid          = vid,
            pid          = pid,
            manufacturer = "Enjoy-Digital",
            product      = "LiteX-M2SDR",
            serial       = serial,
        )
        self.bulk_in  = bulk_in  = USB3BulkIN( ept=USB3_RX_EPT, max_burst=USB3_MAX_BURST)
        self.bulk_out = bulk_out = USB3BulkOUT(ept=USB3_TX_EPT, max_burst=USB3_MAX_BURST)
        self.core = core = USB3DeviceCore(phy, sys_clk_freq, descriptors, [bulk_in, bulk_out],
            with_ep0_handler = True)

        # EP0 Wishbone Bridge ----------------------------------------------------------------------
        self.wishbone = USB3EP0Wishbone(core.ep0)
        self.bus      = self.wishbone.bus

        # RX: Crossbar -> Bulk IN ------------------------------------------------------------------
        self.rx_streamer = rx_streamer = USB3RXStreamer(data_width=data_width)
        self.comb += [
            rx_streamer.enable.eq(self._control.fields.rx_enable & phy.u0),
            self.sink.connect(rx_streamer.sink),
            rx_streamer.source.connect(bulk_in.sink),
            bulk_in.flush.eq(rx_streamer.flush),
        ]

        # TX: Bulk OUT -> Crossbar -----------------------------------------------------------------
        self.tx_streamer = tx_streamer = USB3TXStreamer(data_width=data_width)
        self.comb += [
            tx_streamer.enable.eq(self._control.fields.tx_enable),
            bulk_out.source.connect(tx_streamer.sink, omit={"first"}),
            tx_streamer.source.connect(self.source),
        ]

        # Status -----------------------------------------------------------------------------------
        self.comb += [
            self._status.fields.ready.eq(serdes.ready),
            self._status.fields.link_up.eq(phy.u0),
            self._status.fields.tx_started.eq(tx_streamer.started),
        ]
