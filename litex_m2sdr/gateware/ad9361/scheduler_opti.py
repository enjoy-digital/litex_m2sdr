#
# This file is responsible for scheduling TX based on timestamps.
#
# author: Ismail Essaidi
# 


from migen import *
from litex.gen import *
from litepcie.common import *
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

from litex_m2sdr.gateware.layouts import metadata_layout

STATE_STREAM = ord('S')  
STATE_WAIT   = ord('W')  
STATE_RESET  = ord('R')

class Scheduler(LiteXModule): 
    def __init__(self, frames_per_packet = 1024, data_width=64, max_packets=8, meta_data_frames_per_packet = 2, with_csr = True): # just to start then maybe expand it to more packets
        assert frames_per_packet % (data_width // 8) == 0 # packet size must be multiple of data width in bytes (a frame is 8 bytes)
        assert meta_data_frames_per_packet == 2  # we have 2 frames for metadata: header + timestamp
        # Inputs/Outputs
        self.reset = Signal() # i
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width)) # i  (from TX_CDC)
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o  (to gpio_tx_unpacker)
        self.now = Signal(64) # i (current time: drive in RFIC domain)
        # Data Fifo: stores multiple packets
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames
        self.metadata_fifo = metadata_fifo = stream.SyncFIFO(layout=metadata_layout(data_width), depth=meta_data_frames_per_packet*max_packets)
        self.state = Signal(8) # for debug purpose only, not used currently

        
        # ────────────────────────────────────────────────
        #  FIFO connections
        # ────────────────────────────────────────────────
        # Always Read from upstream if I can
        self.comb += [
            sink.ready.eq(data_fifo.sink.ready), # backpressure
            data_fifo.sink.valid.eq(sink.valid),
            # data_fifo.sink.data.eq(sink.data),
            data_fifo.sink.first.eq(sink.first),
            data_fifo.sink.last.eq(sink.last),
            # metadata_fifo.sink.timestamp.eq(self.timestamp),
            # metadata_fifo.sink.header.eq(self.header),
        ]

        self.comb += [
                If(self.reset,
                    self.state.eq(STATE_RESET)
                ).Elif((self.metadata_fifo.source.timestamp <= self.now) & self.data_fifo.source.valid,
                    self.state.eq(STATE_STREAM)
                ).Else(
                    self.state.eq(STATE_WAIT)
                )
            ]

        # Output side: only present data when timestamp <= now
        self.comb += [
            source.first.eq(data_fifo.source.first),
            source.last.eq(data_fifo.source.last),
            source.data.eq(data_fifo.source.data),
            source.valid.eq(data_fifo.source.valid & (metadata_fifo.source.timestamp <= self.now)),
            data_fifo.source.ready.eq(source.ready & (metadata_fifo.source.timestamp <= self.now)),
        ]

        # Signals.
        # --------
        frame_count = Signal(10) # max 1024 frames per packet 
        pkt_count = Signal(4) # max 8 packets

        # FSM.
        # ----
        self.fsm = fsm = FSM(reset_state="RESET")
        #self.comb += self.fsm.reset.eq(self.reset)

        # Reset.
        fsm.act("RESET",
            NextValue(frame_count, 0),
            NextState("HEADER")
        )

        # Header.
        fsm.act("HEADER",
            If(sink.valid & sink.ready,
                metadata_fifo.sink.header.eq(sink.data[0:64]),
                metadata_fifo.sink.valid.eq(1),
                # metadata_fifo.source.header.eq(sink.data[0:64]),
                NextValue(frame_count, frame_count + 1),
                NextState("TIMESTAMP")
            )
        )
        # Timestamp.
        fsm.act("TIMESTAMP",
            If(sink.valid & sink.ready,
                metadata_fifo.sink.timestamp.eq(sink.data[0:64]),
                metadata_fifo.sink.valid.eq(1),
                # metadata_fifo.source.timestamp.eq(sink.data[0:64]),
                NextValue(frame_count, frame_count + 1),
                NextState("FRAME")
            )
        )

        # Frame.
        fsm.act("FRAME",
            metadata_fifo.sink.valid.eq(1),
            NextValue(data_fifo.sink.data, sink.data[0:64]),
            NextValue(frame_count, frame_count + 1),
            If(frame_count == (frames_per_packet - 1),
                NextState("HEADER")
            )
        )

        # metadata fifo flow control
        pop_meta = Signal()
        pop_meta_count = Signal(2)   # can count 0..3
        self.comb += [
            metadata_fifo.source.ready.eq(source.valid & source.ready & (metadata_fifo.source.timestamp <= self.now)), # ready to pop
            pop_meta.eq(metadata_fifo.source.ready)
        ] 
        
        # increment on every metadata pop
        self.sync += If(pop_meta, pop_meta_count.eq(pop_meta_count + 1))
        self.sync += If(pop_meta_count == 2, pop_meta_count.eq(0))
        self.comb += [
            metadata_fifo.source.valid.eq(pop_meta & (pop_meta_count == 0)), 
            # metadata_fifo.sink.valid.eq((frame_count <= 2) & sink.valid & sink.ready),
        ]
        # ────────────────────────────────────────────────
            #  CSR Exposure
        # ────────────────────────────────────────────────
        # if with_csr:
        #     self.add_csr()

    def add_csr(self):
        # Control
        self._reset      = CSRStorage(fields=[CSRField("reset", size=1, description="Reset scheduler FSM")])
        self._manual_now = CSRStorage(64, description="Manually set current timestamp for test")
        self._set_ts = CSRStorage(fields=[CSRField("use_manual", size=1, description="1=use manual now value instead of Timestamp at fifo")])

        # Status
        #self._fsm_state  = CSRStatus(8, description="Current FSM state (RESET=0, WAIT=1, STREAM=2)")
        self._fifo_level = CSRStatus(16, description="Current FIFO level in words")
        self._flags      = CSRStatus(fields=[
            CSRField("full",         size=1),
            CSRField("empty",        size=1),
            CSRField("almost_full",  size=1),
            CSRField("almost_empty", size=1)
        ])
        self._current_ts = CSRStatus(64, description="Current Timestamp at FIFO output")
        self._now        = CSRStatus(64, description="Current time from rfic domain")

        # Drive internal signals from CSRs
        manual_now_sig = Signal(64)
        self.comb += [
            manual_now_sig.eq(self._manual_now.storage),
            If(self._set_ts.fields.use_manual,
                self.now.eq(manual_now_sig)
            )
        ]
        # Connect status signals

        # current_ts_latched = Signal(64)
        # self.sync += If(self.data_fifo.source.valid, current_ts_latched.eq(self.data_fifo.source.timestamp[0:64]))
        self.comb += [
           # self._fsm_state.status.eq(self.fsm.state),
            self._fifo_level.status.eq(self.data_fifo.level),
            self._flags.fields.full.eq(self.full),
            self._flags.fields.empty.eq(self.empty),
            self._flags.fields.almost_full.eq(self.almost_full),
            self._flags.fields.almost_empty.eq(self.almost_empty),
            # self._current_ts.status.eq(current_ts_latched),
            self._now.status.eq(self.now)
        ]
        
        # ──────────────────────────────────────────────── New things (to check #TODO)
        # self.last_tx_header    = CSRStatus(64, description="Last TX Header.")
        # self.last_tx_timestamp = CSRStatus(64, description="Last TX Timestamp.")
        # self.last_rx_header    = CSRStatus(64, description="Last RX Header.")
        # self.last_rx_timestamp = CSRStatus(64, description="Last RX Timestamp.")
        # self.sync += [
        #     # Reset.
        #     If(self.tx.reset,
        #         self.last_tx_header.status.eq(0),
        #         self.last_tx_timestamp.status.eq(0),
        #     ),
        #     If(self.rx.reset,
        #         self.last_rx_header.status.eq(0),
        #         self.last_rx_timestamp.status.eq(0),
        #     ),
        #     # TX Update.
        #     If(self.tx.update, # only when a new frame is started
        #         self.last_tx_header.status.eq(self.tx.header),
        #         self.last_tx_timestamp.status.eq(self.tx.timestamp),
        #     ),
        #     # RX Update.
        #     If(self.rx.update, # only when a new frame is started
        #         self.last_rx_header.status.eq(self.rx.header),
        #         self.last_rx_timestamp.status.eq(self.rx.timestamp),
        #     )
        # ]
        

    
