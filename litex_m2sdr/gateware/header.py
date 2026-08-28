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
        assert data_width == 64
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
        # Release reference: the frame airs at self.time + tx_offset, so this is what the FSM
        # compares against the requested air-time (self.timestamp).
        #
        # Registered on the sys clock (not combinational): tx_offset is a CSR register and this
        # 64-bit add feeds three 64-bit comparisons that select the FSM next-state. The sys clock
        # is the design's critical domain, so keeping the whole CSR -> 64-bit adder -> 64-bit
        # comparator -> FSM chain in one cycle costs timing slack; registering splits it into
        # (CSR -> adder -> flop) and (flop -> comparator -> FSM). The one added sys-clock cycle of
        # release latency is a fixed constant, absorbed by the tx_offset calibration.
        gate_now = Signal(64)
        self.sync += gate_now.eq(self.time + self.tx_offset)

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
            # Header.
            fsm.act("HEADER",
                source.valid.eq(1),
                source.first.eq(1),
                source.data[0:64].eq(self.header),
                If(source.valid & source.ready,
                    NextState("TIMESTAMP"),
                )
            )
            # Timestamp.
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
            # Header.
            fsm.act("HEADER",
                sink.ready.eq(1),
                If(sink.valid & sink.ready & sink.first,
                    NextValue(self.header, sink.data[0:64]),
                    NextState("TIMESTAMP")
                )
            )
            # Timestamp. On capture, enter the timed gate instead of the frame directly.
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
                # A late frame is dropped by airing ZEROS for its whole duration, PACED to the RFIC
                # sample rate exactly like FRAME (advance on source.valid & source.ready) -- NOT
                # drained at sink.ready=1 (fabric/DMA speed). Fast-draining empties the TX DMA ring
                # far faster than real time, so the LOOP-mode reader races ahead of the host
                # (reader_hw_count >> host submit) -> m2sdr_sync_tx -11 UNDERFLOW; and once ANY frame
                # is late every later frame is late too, each fast-dropped -> unrecoverable positive
                # feedback (the timed-TX storm). Pacing makes a dropped slot cost one frame air-time,
                # so the reader stays real-time-locked. source.data is 0 by default in non-FRAME
                # states, so the RFIC airs zeros; only the 1-bit valid/last/ready are added here.
                source.valid.eq(sink.valid),
                source.last.eq(cycles == (frame_cycles_eff - 1)),
                sink.ready.eq(source.ready),
                If(source.valid & source.ready,
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

    def add_csr(self, default_enable=1, default_header_enable=0, default_frame_cycles=8192/8 - 2):
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
