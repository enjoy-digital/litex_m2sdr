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
        frames_per_packet = packet_size // (data_width // 8) # a frame is 8 bytes ==> 1022 frames_per_packet
        # Data Fifo: stores multiple packets
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames
        # Metadata Fifo: stores metadata for each packet
        self.metadata = meta_fifo = stream.SyncFIFO(
            layout=metadata_layout(timestamp_width=64, ptr_width=8),
            depth=max_packets
        )

        self.sink = data_fifo.sink # i
        self.meta_sink = meta_fifo.sink # i
        self.source = data_fifo.source # o

        # Control Signals
        frame_count = Signal(10) # max 1022 frames per packet
        current_timestamp = Signal(64)

    
