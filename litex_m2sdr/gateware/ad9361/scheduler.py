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

class Scheduler(LiteXModule): # TODO implement the valid and ready signals properly
    def __init__(self, packet_size=8176, data_width=64, max_packets=8): # just to start then maybe expand it to more packets

        assert packet_size % (data_width // 8) == 0 # packet size must be multiple of data width in bytes
    
        # Inputs/Outputs
        self.reset = Signal() # i
        self.sink   = sink   = stream.Endpoint(dma_layout_with_ts(data_width)) # i  (from TX_CDC)
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o  (to gpio_tx_unpacker)
        self.now = Signal(64) # i (current time: drive in RFIC domain)
        
        # Data Fifo: stores multiple packets
        frames_per_packet = packet_size // (data_width // 8) # a frame is 8 bytes ==> 1022 frames_per_packet
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout_with_ts(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames
        
        # Status Signals
        self.almost_full = Signal() # o
        self.almost_empty = Signal() # o
        self.empty = Signal() # o
        self.full = Signal() # o

        self.comb += [
            self.full.eq(data_fifo.level == data_fifo.depth),
            self.almost_full.eq(data_fifo.level >= data_fifo.depth - frames_per_packet), # almost full when it can no longer accept a full packet
            self.empty.eq(data_fifo.level == 0),
            self.almost_empty.eq(data_fifo.level <= frames_per_packet), # almost empty when it can no longer output a full packet
        ]

        # Counters for debugging
        # frame_count = Signal(13) # max 1022 frames per packet * 8 bytes = 8176 bytes
        # pkt_count = Signal(4) # max 8 packets
        
        # ready_to_read = Signal()
        # self.comb += [
        #     ready_to_read.eq(sink.valid & sink.ready),
        #     pkt_count.eq(frame_count // frames_per_packet)
        # ]
        # self.sync += frame_count.eq(frame_count + ready_to_read)

        # Always Read from upstream if I can
        self.comb += [
            data_fifo.sink.connect(sink)   # data_fifo.sink.valid  <- sink.valid
                                            # data_fifo.sink.data   <- sink.data
                                            # sink.ready            <- data_fifo.sink.ready (backpressure)
        ]       

        # connect fifo output to source.data 
        self.comb += [
            source.data.eq(data_fifo.source.data),
            source.first.eq(data_fifo.source.first),
            source.last.eq(data_fifo.source.last),
        ]

        # Default assignements (outside FSM)
        self.comb += data_fifo.source.ready.eq(0) # default don't pop from fifo
        
        # FSM for scheduling ------------
        self.fsm = fsm = FSM(reset_state="RESET")
        self.submodules.fsm = fsm   # need this to enable litescope debugging
        
        fsm.act("RESET",
            # NextValue(frame_count, 0),
            NextState("WAIT")
        )

        fsm.act("WAIT", # Read if I can otherwise wait
            source.valid.eq(0), # not streaming
            If((data_fifo.source.timestamp <= self.now) & (data_fifo.source.valid),
                NextState("STREAM")
            )
        )
        
        fsm.act("STREAM",
            source.valid.eq(data_fifo.source.valid & (data_fifo.source.timestamp <= self.now)),  # should be 1 ==> ready to stream out 
            data_fifo.source.ready.eq(source.ready & (data_fifo.source.timestamp <= self.now)),  # pop from fifo if source is ready
            If((data_fifo.source.timestamp > self.now) | (~data_fifo.source.valid),
                NextState("WAIT")
            )
        )
        

    
