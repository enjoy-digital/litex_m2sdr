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
RX (EP1) and TX (EP2) are on separate interfaces (0 and 1) so that RX and TX can be claimed by
separate host processes (ex: m2sdr_record + m2sdr_play); EP0 accesses do not claim an interface.

https://github.com/enjoy-digital/usb3_pipe
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litepcie.common import dma_layout

# Constants ----------------------------------------------------------------------------------------

USB3_VID          = 0x1d50
USB3_PID          = 0x605a
USB3_RX_INTERFACE = 0
USB3_TX_INTERFACE = 1
USB3_RX_EPT       = 1 # Bulk IN  (RFIC RX -> Host).
USB3_TX_EPT       = 2 # Bulk OUT (Host -> RFIC TX).
USB3_MAX_BURST    = 16

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
    """RFIC RX stream (dma_layout) to the Bulk IN Endpoint (ept_width: 32-bit, or data_width,
    first byte on data[7:0]).

    Disabled: stream drained (no back-pressure on the RFIC datapath). Enable: buffered packets of a
    previous run flushed (BulkIN.flush) so the host only receives frames of the current run.
    """
    def __init__(self, data_width=64, fifo_depth=4096, ept_width=32):
        assert ept_width in [32, data_width]
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width))
        self.source = source = stream.Endpoint([("data", ept_width), ("be", ept_width//8)])
        self.flush  = Signal() # o: to BulkIN.flush.

        self.enable = Signal() # i

        # # #

        # FIFO (absorbs the host scheduling jitter in front of the Bulk IN slots).
        self.fifo = fifo = ResetInserter()(stream.SyncFIFO(dma_layout(data_width), fifo_depth,
            buffered=True))

        # Converter (first 32-bit word from data[31:0], as the PCIe DMA in host memory).
        self.conv = conv = ResetInserter()(stream.Converter(data_width, 32) if ept_width == 32 else
            stream.Buffer([("data", data_width)]))
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
            source.be.eq(2**(ept_width//8) - 1),
        ]

# USB3 TX Streamer (Bulk OUT -> Crossbar) ----------------------------------------------------------

class USB3TXStreamer(LiteXModule):
    """Bulk OUT Endpoint (ept_width: 32-bit, or data_width) to the RFIC TX stream (dma_layout), through a FIFO primed (half
    full) before being forwarded so the RFIC is not underrun at start (as the Ethernet streamer).

    Disabled: Bulk OUT data drained, FIFO kept empty.

    The first word of each host buffer (frame_bytes, counted from the enable) is marked with first,
    as the PCIe DMA does: the TX header extractor finds the header (timestamp) of a buffer with
    it. With framed set (TX header enabled), the FIFO is not primed: a timed frame waits in the
    timed-TX gate, and must then leave without a delay that depends on the FIFO level.
    """
    def __init__(self, data_width=64, fifo_depth=(256*1024)//8, frame_bytes=8192, ept_width=32):
        assert ept_width in [32, data_width]
        self.sink   = sink   = stream.Endpoint([("data", ept_width), ("be", ept_width//8)])
        self.source = source = stream.Endpoint(dma_layout(data_width))

        self.enable  = Signal() # i
        self.framed  = Signal() # i
        self.started = Signal() # o

        # # #

        # Converter (first 32-bit word to data[31:0]).
        self.conv = conv = ResetInserter()(stream.Converter(32, data_width) if ept_width == 32 else
            stream.Buffer([("data", data_width)]))

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
            conv.source.connect(fifo.sink, omit={"valid_token_count", "first", "last"}),
        ]

        # Framing: first on the first word of each host buffer.
        frame_words = frame_bytes//(data_width//8)
        frame_count = Signal(max=frame_words)
        self.comb += fifo.sink.first.eq(frame_count == 0)
        self.sync += [
            If(~self.enable,
                frame_count.eq(0)
            ).Elif(fifo.sink.valid & fifo.sink.ready,
                If(frame_count == (frame_words - 1),
                    frame_count.eq(0)
                ).Else(
                    frame_count.eq(frame_count + 1)
                )
            )
        ]

        # Priming: forward once half full, until empty (underrun) or disabled.
        primed = Signal()
        self.comb += primed.eq(fifo.level >= fifo_depth//2)
        self.sync += [
            If(~self.enable,
                self.started.eq(0)
            ).Elif(primed | self.framed,
                self.started.eq(1)
            ).Elif(fifo.level == 0,
                self.started.eq(0)
            )
        ]
        self.comb += [
            fifo.source.connect(source, omit={"valid", "ready", "last"}),
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
        pipe_clk   = None,
        data_width = 64,
        vid        = USB3_VID,
        pid        = USB3_PID,
        serial     = "0001",
        with_debug = False):
        from usb3_pipe             import A7USB3SerDes, USB3PHY
        from usb3_core.device      import USB3DeviceCore
        from usb3_core.bulk        import USB3BulkIN, USB3BulkOUT
        from usb3_core.descriptors import USB3Descriptors, USB3InterfaceDescriptor, USB3EndpointDescriptor
        from usb3_core.wishbone    import USB3EP0Wishbone

        # PIPE/SerDes at 125MHz (32-bit words): in sys, or with a slower sys clock in the usb3 clock
        # domain (pipe_clk) with the LTSSM/Link Layer/Core in sys on 64-bit Link Layer words (as
        # USB3-PIPE on the Gowin GW5A/ECP5: USB3PHY pipe_cd).
        pipe_clk_freq = 125e6
        pipe_cd       = None if sys_clk_freq == pipe_clk_freq else "usb3"
        assert sys_clk_freq <= pipe_clk_freq,           "USB3 requires sys_clk_freq <= 125MHz."
        assert (pipe_cd is None) or (pipe_clk is not None), "USB3 with sys_clk_freq < 125MHz requires pipe_clk."

        self.sink   = stream.Endpoint(dma_layout(data_width))
        self.source = stream.Endpoint(dma_layout(data_width))

        self.tx_framed = Signal() # i: TX header enabled (see USB3TXStreamer).

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
        # Yosys maps the 8b/10b checker lookups to 8 levels of LUTs, too slow for the 250MHz RX
        # clock with nextpnr-xilinx: block ROMs there, with a USB3-PIPE that has rx_rom_style.
        import inspect
        serdes_kwargs = {}
        if getattr(platform, "toolchain_name", "vivado") != "vivado":
            if "rx_rom_style" in inspect.signature(A7USB3SerDes.__init__).parameters:
                serdes_kwargs.update(rx_rom_style="block")
        tx_pads, rx_pads = get_usb3_sfp_pads(sfp_pads)
        serdes = A7USB3SerDes(platform,
            sys_clk      = sys_clk if pipe_cd is None else pipe_clk,
            sys_clk_freq = pipe_clk_freq,
            refclk_pads  = None,
            refclk_freq  = refclk_freq,
            tx_pads      = tx_pads,
            rx_pads      = rx_pads,
            pll          = USB3QPLLChannel(qpll_channel, refclk_freq),
            **serdes_kwargs,
        )
        if pipe_cd is not None:
            serdes = ClockDomainsRenamer({"sys": pipe_cd})(serdes)
        self.serdes = serdes
        # TX polarity: swaps of the M2SDR and Baseboard compensate (as Ethernet: tx_polarity=0).
        # RX polarity: detected by the LTSSM (TS1/TS2 inversion).

        # PHY --------------------------------------------------------------------------------------
        if pipe_cd is None:
            self.phy = phy = USB3PHY(serdes, sys_clk_freq, role="device")
        else:
            assert not with_debug, "USB3 debug requires sys_clk_freq == 125MHz."
            self.phy = phy = USB3PHY(serdes, sys_clk_freq, role="device",
                link_words    = 2,
                pipe_cd       = pipe_cd,
                pipe_clk_freq = pipe_clk_freq,
                with_u1u2     = False)
        if with_debug:
            from usb3_pipe.debug import USB3PHYDebug, USB3SerDesDebug
            self.debug        = USB3PHYDebug(phy, serdes, sys_clk_freq)
            self.serdes_debug = USB3SerDesDebug(serdes, sys_clk_freq, ltssm_state=phy.ltssm_state)

        # Link status (LTSSM state: usb3_pipe.gen.get_ltssm_states), for the bring-up of the builds
        # without the PHY debug.
        if pipe_cd is not None:
            self._link = CSRStatus(fields=[
                CSRField("ltssm_state", size=len(phy.ltssm_state), offset=0, description="LTSSM state."),
                CSRField("u0",          size=1, offset=8,  description="LTSSM in U0."),
                CSRField("recovery",    size=1, offset=9,  description="LTSSM in Recovery."),
                CSRField("ready",       size=1, offset=10, description="SerDes ready."),
            ], description="USB3 link status.")
            self.comb += [
                self._link.fields.ltssm_state.eq(phy.ltssm_state),
                self._link.fields.u0.eq(phy.u0),
                self._link.fields.recovery.eq(phy.recovery),
                self._link.fields.ready.eq(serdes.ready),
            ]

        # Device Core ------------------------------------------------------------------------------
        self.descriptors = descriptors = USB3Descriptors(
            vid          = vid,
            pid          = pid,
            manufacturer = "Enjoy-Digital",
            product      = "LiteX-M2SDR",
            serial       = serial,
            interfaces   = [
                # Interface 0: RX (Bulk IN).
                USB3InterfaceDescriptor(USB3_RX_INTERFACE, endpoints=[
                    USB3EndpointDescriptor(USB3_RX_EPT, direction=1, max_burst=USB3_MAX_BURST),
                ]),
                # Interface 1: TX (Bulk OUT).
                USB3InterfaceDescriptor(USB3_TX_INTERFACE, endpoints=[
                    USB3EndpointDescriptor(USB3_TX_EPT, direction=0, max_burst=USB3_MAX_BURST),
                ]),
            ],
        )
        # Endpoints at the Link Layer words width (Bulk OUT: not adapted by the Protocol Layer).
        ept_width = 32*phy.link_words
        assert ept_width in [32, data_width]
        self.bulk_in  = bulk_in  = USB3BulkIN( ept=USB3_RX_EPT, max_burst=USB3_MAX_BURST, data_width=ept_width)
        self.bulk_out = bulk_out = USB3BulkOUT(ept=USB3_TX_EPT, max_burst=USB3_MAX_BURST, data_width=ept_width)
        self.core = core = USB3DeviceCore(phy, sys_clk_freq, descriptors, [bulk_in, bulk_out],
            with_ep0_handler = True)
        if with_debug:
            core.add_csr()

        # EP0 Wishbone Bridge ----------------------------------------------------------------------
        self.wishbone = USB3EP0Wishbone(core.ep0)
        self.bus      = self.wishbone.bus

        # RX: Crossbar -> Bulk IN ------------------------------------------------------------------
        self.rx_streamer = rx_streamer = USB3RXStreamer(data_width=data_width, ept_width=ept_width)
        self.comb += [
            rx_streamer.enable.eq(self._control.fields.rx_enable),
            self.sink.connect(rx_streamer.sink),
            rx_streamer.source.connect(bulk_in.sink),
            bulk_in.flush.eq(rx_streamer.flush),
        ]

        # TX: Bulk OUT -> Crossbar -----------------------------------------------------------------
        self.tx_streamer = tx_streamer = USB3TXStreamer(data_width=data_width, ept_width=ept_width)
        self.comb += [
            tx_streamer.enable.eq(self._control.fields.tx_enable),
            tx_streamer.framed.eq(self.tx_framed),
            bulk_out.source.connect(tx_streamer.sink, omit={"first"}),
            tx_streamer.source.connect(self.source),
        ]

        # Status -----------------------------------------------------------------------------------
        self.comb += [
            self._status.fields.ready.eq(serdes.ready),
            self._status.fields.link_up.eq(phy.u0),
            self._status.fields.tx_started.eq(tx_streamer.started),
        ]
