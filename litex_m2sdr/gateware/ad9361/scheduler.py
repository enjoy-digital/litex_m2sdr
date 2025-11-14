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

from litex_m2sdr.gateware.layouts import dma_layout_with_ts, metadata_layout

STATE_STREAM = ord('S')  
STATE_WAIT   = ord('W')  
STATE_RESET  = ord('R')

class Scheduler(LiteXModule): 
    def __init__(self, packet_size=8176, data_width=64, max_packets=8, with_csr = True): # just to start then maybe expand it to more packets
        fsm = False  

        assert packet_size % (data_width // 8) == 0 # packet size must be multiple of data width in bytes
    
        # Inputs/Outputs
        self.reset = Signal() # i
        self.sink   = sink   = stream.Endpoint(dma_layout_with_ts(data_width)) # i  (from TX_CDC)
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o  (to gpio_tx_unpacker)
        self.now = Signal(64) # i (current time: drive in RFIC domain)
        
        # Data Fifo: stores multiple packets
        frames_per_packet = packet_size // (data_width // 8) # a frame is 8 bytes ==> 1022 frames_per_packet
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout_with_ts(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames
        self.state = Signal(8) # for debug purpose only, not used currently

        # Counters for debugging
        frame_count = Signal(13) 
        pkt_count = Signal(4) # max 8 packets
        
        ready_to_read = Signal()
        # Combinational: define handshake condition
        self.comb += ready_to_read.eq(sink.valid & sink.ready)
        
        # Sequential: update frame and packet counters on clock edges
        self.sync += [
            If(ready_to_read,
                If(frame_count == frames_per_packet - 1,
                    frame_count.eq(0),
                    pkt_count.eq(pkt_count + 1)
                ).Else(
                    frame_count.eq(frame_count + 1)
                )
            )
        ]
        
        # ────────────────────────────────────────────────
        #  FIFO connections
        # ────────────────────────────────────────────────
        # Always Read from upstream if I can
        self.comb += [
            sink.ready.eq(data_fifo.sink.ready), # backpressure
            data_fifo.sink.valid.eq(sink.valid),
            data_fifo.sink.data.eq(sink.data),
            data_fifo.sink.first.eq(sink.first),
            data_fifo.sink.last.eq(sink.last),
            data_fifo.sink.timestamp.eq(sink.timestamp)
        ]

        self.comb += [
                If(self.reset,
                    self.state.eq(STATE_RESET)
                ).Elif((self.data_fifo.source.timestamp <= self.now) & self.data_fifo.source.valid,
                    self.state.eq(STATE_STREAM)
                ).Else(
                    self.state.eq(STATE_WAIT)
                )
            ]

        self.comb += [
            source.first.eq(data_fifo.source.first),
            source.last.eq(data_fifo.source.last),
            source.data.eq(data_fifo.source.data),
            source.valid.eq(data_fifo.source.valid & (data_fifo.source.timestamp <= self.now)),
            data_fifo.source.ready.eq(source.ready & (data_fifo.source.timestamp <= self.now)),
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
        

    
