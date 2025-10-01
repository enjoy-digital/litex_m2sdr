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

from litex_m2sdr.gateware.layouts import dma_layout, metadata_layout

class Scheduler(LiteXModule): # TODO implement the valid and ready signals properly
    def __init__(self, packet_size=8176, data_width=64, max_packets=8): # just to start then maybe expand it to more packets
        self.reset = Signal() # i
        frames_per_packet = packet_size // (data_width // 8) # a frame is 8 bytes ==> 1022 frames_per_packet
        # Data Fifo: stores multiple packets
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames

        # self.sink = data_fifo.sink # i
        # self.source = data_fifo.source # o
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width)) # i   
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o  

        self.almost_full = Signal() # o
        self.almost_empty = Signal() # o
        self.empty = Signal() # o
        self.full = Signal() # o

        # Status Signals
        self.comb += [
            self.full.eq(data_fifo.level == data_fifo.depth),
            self.almost_full.eq(data_fifo.level >= data_fifo.depth - frames_per_packet), # almost full when it can no longer accept a full packet
            self.empty.eq(data_fifo.level == 0),
            self.almost_empty.eq(data_fifo.level <= frames_per_packet), # almost empty when it can no longer output a full packet
        ]


        # Control Signals
        frame_count = Signal(13) # max 1022 frames per packet * 8 bytes = 8176 bytes
        pkt_count = Signal(4) # max 8 packets

        self.comb += [
            self.pkt_count.eq(frame_count // frames_per_packet), #FIXME maybe increment manually instead of divider 
        ]
        
        current_timestamp = Signal(64)
        self.comb += current_timestamp.eq(self.data_fifo.source.timestamp)
                    

        # FSM for scheduling ------------
        self.fsm = fsm = FSM(reset_state="RESET")


        fsm.act("RESET",
            frame_count.eq(0),
            pkt_count.eq(0),
            NextValue(frame_count, 0),
            data_fifo.reset.eq(self.reset),
            NextState("WAIT")
        )

        fsm.act("WAIT", # Read if I can otherwise wait
            self.sink.ready.eq(1 if not self.full else 0),
            self.source.valid.eq(0),
            NextValue(frame_count, frame_count + 1 if (self.sink.valid & self.sink.ready) else frame_count),
            If((current_timestamp >= now()) and ~self.empty,
                NextState("STREAM")
            ).Else(
                data_fifo.sink.connect(self.sink),  # TODO check ready and valid
                NextState("WAIT")
            )
        )
        
        fsm.act("STREAM",
            self.source.valid.eq(1),
            self.sink.ready.eq(1 if not self.full else 0), # read from sink if not full
            If((current_timestamp < now())| self.empty, # stop streaming if not enabled or empty
                NextState("WAIT")
            ).Else(
                NextValue(frame_count, frame_count + 1 if (self.source.valid & self.source.ready) else frame_count),
                data_fifo.source.connect(self.source), # stream out if valid and ready # TODO check ready and valid
                NextState("STREAM")
            )
        )
        

    
