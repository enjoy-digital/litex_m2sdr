#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litepcie.common import *

from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

# Header Inserter/Extractor ------------------------------------------------------------------------

class HeaderInserterExtractor(LiteXModule):
    def __init__(self, mode="inserter", data_width=64, with_csr=True):
        # The DMA header is a fixed 16 bytes (sync word + ns timestamp). At 64-bit it occupies
        # two stream words (HEADER then TIMESTAMP states); at 128-bit it fits in one word
        # ([timestamp(64) | sync(64)], low half = sync = first buffer bytes), so the FSM emits/
        # captures it in a single HEADER state. 128-bit is used by the wide (Oversampling) path
        # so the sys-domain word rate halves (2T2R @ 122.88 MSPS fits the 64-bit @ 125MHz fabric).
        assert data_width in [64, 128]
        self.data_width = data_width
        single_word = (data_width == 128)
        assert mode in ["inserter", "extractor"]
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width)) # i
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o

        self.reset         = Signal() # i

        self.update        = Signal()   # o
        self.header        = Signal(64) # i (Inserter) / o (Extractor)
        self.timestamp     = Signal(64) # i (Inserter) / o (Extractor)

        self.enable        = Signal()   # i (CSR).
        self.header_enable = Signal()   # i (CSR).
        self.frame_cycles  = Signal(32) # i (CSR).

        # Timed-TX gate (extractor only). Hold each frame until the FPGA time (+ tx_offset,
        # the calibrated TX-pipeline latency) reaches the frame's air-time (header ns
        # timestamp), then release so the signal is ON THE AIR at exactly the timestamp.
        # A frame that arrives already past its air-time is dropped entirely (no tolerance
        # window) and counted as a TX underflow. The PHY airs zeros during any hold or drop
        # ("Clear to avoid spurs" in phy.py), so gaps are clean.
        self.mode           = mode
        self.time           = Signal(64) # i  FPGA time (ns).
        self.tx_offset      = Signal(64) # i  CSR: TX pipeline offset (ns).
        self.underflow      = Signal(32) # o  TX underflow count: timed frames that missed
                                          #    their air-time (dropped -> RFIC aired zeros).

        if with_csr:
            self.add_csr()

        # # #

        # Signals.
        # --------
        cycles = Signal(32)
        frame_cycles_eff = Signal(32)
        self.comb += frame_cycles_eff.eq(Mux(self.frame_cycles == 0, 1, self.frame_cycles))
        # Release reference: the frame airs at self.time + tx_offset, so compare that to the
        # requested air-time (self.timestamp).
        gate_now = Signal(64)
        self.comb += gate_now.eq(self.time + self.tx_offset)

        # FSM.
        # ----
        self.fsm = fsm = ResetInserter()(FSM(reset_state="RESET"))
        self.comb += self.fsm.reset.eq(self.reset | ~self.enable)

        # Reset.
        reset_actions = [
            NextValue(cycles, 0),
            NextState("IDLE"),
        ]
        if mode == "inserter":
            # Drain the upstream while an explicit reset is asserted so the
            # producer does not stall against a held-in-reset inserter. (The
            # mode selection is an elaboration-time constant, hence the
            # Python conditional.)
            reset_actions.append(sink.ready.eq(self.reset))
        fsm.act("RESET", *reset_actions)

        # Idle.
        fsm.act("IDLE",
            NextValue(cycles, 0),
            If(self.header_enable,
                NextState("HEADER")
            ).Else(
                NextState("FRAME")
            )
        )

        # Inserter specific.
        if mode == "inserter":
            if single_word:
                # 128-bit: sync + timestamp in one word -> straight to FRAME.
                fsm.act("HEADER",
                    source.valid.eq(1),
                    source.first.eq(1),
                    source.data[0:64].eq(self.header),
                    source.data[64:128].eq(self.timestamp),
                    If(source.valid & source.ready,
                        NextValue(self.update, 1),
                        NextState("FRAME"),
                    )
                )
            else:
                # 64-bit: sync word, then timestamp word.
                fsm.act("HEADER",
                    source.valid.eq(1),
                    source.first.eq(1),
                    source.data[0:64].eq(self.header),
                    If(source.valid & source.ready,
                        NextState("TIMESTAMP"),
                    )
                )
                fsm.act("TIMESTAMP",
                    source.valid.eq(1),
                    source.data[0:64].eq(self.timestamp),
                    If(source.valid & source.ready,
                        NextValue(self.update, 1),
                        NextState("FRAME"),
                    )
                )

        # Extractor specific.
        if mode == "extractor":
            if single_word:
                # 128-bit: capture sync + timestamp from one word.
                fsm.act("HEADER",
                    sink.ready.eq(1),
                    If(sink.valid & sink.ready & sink.first,
                        NextValue(self.header, sink.data[0:64]),
                        NextValue(self.timestamp, sink.data[64:128]),
                        NextValue(self.update, 1),
                        NextState("GATE")
                    )
                )
            else:
                # 64-bit: sync word, then timestamp word.
                fsm.act("HEADER",
                    sink.ready.eq(1),
                    If(sink.valid & sink.ready & sink.first,
                        NextValue(self.header, sink.data[0:64]),
                        NextState("TIMESTAMP")
                    )
                )
                fsm.act("TIMESTAMP",
                    sink.ready.eq(1),
                    If(sink.valid & sink.ready,
                        NextValue(self.timestamp, sink.data[0:64]),
                        NextValue(self.update, 1),
                        NextState("GATE")
                    )
                )

            # Timed gate. Decide on arrival (no tolerance window):
            #  - timestamp == 0  -> UNTIMED: transmit immediately (pass-through). Continuous
            #    streaming clients (and any writer that does not set a per-buffer air-time)
            #    leave the header timestamp at 0; they must stream with zero added latency, so
            #    0 bypasses the gate. 0 is never a legitimate air-time (the FPGA time counter is
            #    0-based at power-on, so air-time 0 is always in the past) -> safe sentinel.
            #  - air-time still ahead -> WAIT (hold); exactly now -> release; already passed
            #    (frame arrived too late to hit its air-time) -> DROP.
            # A held frame releases the exact cycle the FPGA time reaches its air-time
            # (deterministic to the time tick).
            fsm.act("GATE",
                If(self.timestamp == 0,
                    NextState("FRAME")
                ).Elif(gate_now < self.timestamp,
                    NextState("WAIT")
                ).Elif(gate_now == self.timestamp,
                    NextState("FRAME")
                ).Else(
                    NextState("DROP")
                )
            )
            fsm.act("WAIT",
                # Hold (RFIC airs zeros); release the cycle the FPGA time reaches the air-time.
                If(gate_now >= self.timestamp,
                    NextState("FRAME")
                )
            )
            fsm.act("DROP",
                sink.ready.eq(1),   # Drain the payload; source stays idle -> RFIC airs zeros.
                If(sink.valid & sink.ready,
                    NextValue(cycles, cycles + 1),
                    If(cycles == (frame_cycles_eff - 1),
                        NextValue(cycles, 0),
                        NextValue(self.underflow, self.underflow + 1),
                        NextState("HEADER")
                    )
                )
            )

        # Frame.
        fsm.act("FRAME",
            sink.connect(source, omit={"first"}),
            NextValue(self.update, 0),
            If(self.header_enable,
                source.first.eq((cycles == 0) & (mode == "extractor")),
                source.last.eq( cycles == (frame_cycles_eff - 1)),
                If(source.valid & source.ready,
                    NextValue(cycles, cycles + 1),
                    If(source.last,
                        NextValue(cycles, 0),
                        NextState("HEADER")
                    )
                )
            )
        )

    def add_csr(self, default_enable=1, default_header_enable=0, default_frame_cycles=None):
        # One DMA buffer (8192 B) minus the header words, in stream words of data_width/8 bytes:
        # 64-bit -> 8192/8 - 2 header words = 1022; 128-bit -> 8192/16 - 1 header word = 511.
        if default_frame_cycles is None:
            bytes_per_word = self.data_width // 8
            header_words   = 1 if self.data_width == 128 else 2
            default_frame_cycles = 8192 // bytes_per_word - header_words
        self._control = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, values=[
                ("``0b0``", "Module Disabled."),
                ("``0b1``", "Module Enabled."),
            ], reset=default_enable),
            CSRField("header_enable", size=1, offset=1, values=[
                ("``0b0``", "Header Inserter/Extractor Disabled."),
                ("``0b1``", "Header Inserter/Extractor Enabled."),
            ], reset=default_header_enable),
        ])
        self._frame_cycles = CSRStorage(32, description="Frame Cycles (64-bit words)", reset=int(default_frame_cycles))

        # # #

        self.comb += [
            self.enable.eq(self._control.fields.enable),
            self.header_enable.eq(self._control.fields.header_enable),
            self.frame_cycles.eq(self._frame_cycles.storage),
        ]

        # Timed-TX gate CSRs (extractor only).
        if self.mode == "extractor":
            self._tx_offset      = CSRStorage(64, reset=0, description=
                "Timed-TX pipeline offset (ns): added to the FPGA time before comparing against the "
                "header air-time so the signal is on the air at exactly the timestamp (loopback-calibrated).")
            self._underflow      = CSRStatus(32, description=
                "TX underflow count: timed frames that missed their air-time (dropped whole; "
                "the RFIC aired zeros for them).")
            self.comb += [
                self.tx_offset.eq(self._tx_offset.storage),
                self._underflow.status.eq(self.underflow),
            ]

# TX Header Extractor ------------------------------------------------------------------------------

class TXHeaderExtractor(HeaderInserterExtractor):
    def __init__(self, data_width=128, with_csr=True):
        HeaderInserterExtractor.__init__(self,
            mode       = "extractor",
            data_width = data_width,
            with_csr   = with_csr,
        )

# RX Header Inserter -------------------------------------------------------------------------------

class RXHeaderInserter(HeaderInserterExtractor):
    def __init__(self, data_width=128, with_csr=True):
        HeaderInserterExtractor.__init__(self,
            mode       = "inserter",
            data_width = data_width,
            with_csr   = with_csr,
        )

# TX/RX Header -------------------------------------------------------------------------------------

class TXRXHeader(LiteXModule):
    def __init__(self, data_width, with_csr=True):
        # TX.
        self.tx = TXHeaderExtractor(data_width, with_csr)

        # RX.
        self.rx = RXHeaderInserter(data_width, with_csr)

        # CSR.
        if with_csr:
            self.last_tx_header    = CSRStatus(64, description="Last TX Header.")
            self.last_tx_timestamp = CSRStatus(64, description="Last TX Timestamp.")
            self.last_rx_header    = CSRStatus(64, description="Last RX Header.")
            self.last_rx_timestamp = CSRStatus(64, description="Last RX Timestamp.")
            self.sync += [
                # Reset.
                If(self.tx.reset,
                    self.last_tx_header.status.eq(0),
                    self.last_tx_timestamp.status.eq(0),
                ),
                If(self.rx.reset,
                    self.last_rx_header.status.eq(0),
                    self.last_rx_timestamp.status.eq(0),
                ),
                # TX Update.
                If(self.tx.update,
                    self.last_tx_header.status.eq(self.tx.header),
                    self.last_tx_timestamp.status.eq(self.tx.timestamp),
                ),
                # RX Update.
                If(self.rx.update,
                    self.last_rx_header.status.eq(self.rx.header),
                    self.last_rx_timestamp.status.eq(self.rx.timestamp),
                )
            ]
