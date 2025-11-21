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
        self.timestamp = Signal(64) # i (timestamp of the packet at fifo output)
        self.header = Signal(64) # i (header of the packet at fifo output)
        self.now = Signal(64) # i (current time: drive in RFIC domain)
        # Data Fifo: stores multiple packets
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout(data_width), depth=frames_per_packet*max_packets) # depth is in number of frames


        # States
        self.streaming = streaming =  Signal(reset=0)
        frame_count = Signal(10)   # enough for 0..1023 (1024 frames)
        latched_ts = Signal(64)
        latched_header = Signal(64)

        # Always Read from upstream if I can
        self.comb += [
            sink.ready.eq(data_fifo.sink.ready), # backpressure
            data_fifo.sink.valid.eq(sink.valid),
            data_fifo.sink.data.eq(sink.data), # push data with timestamp and header into fifo
            data_fifo.sink.first.eq(sink.first),
            data_fifo.sink.last.eq(sink.last),
        ]

        # ---------------------------
        # 1. Capture header + timestamp
        # ---------------------------
        # This triggers only once per packet.
        meta_data = Signal()
        self.comb += [ 
            If(data_fifo.source.valid & data_fifo.source.first & (frame_count == 0) & ~streaming,
                meta_data.eq(1),
                )
            .Elif(data_fifo.sink.valid & (frame_count == 1) & ~streaming,
                meta_data.eq(1),
                )
            .Else(meta_data.eq(0))
        ]


        # Generate second
        data_fifo_sink_second = Signal()
        self.sync += [
            data_fifo_sink_second.eq(data_fifo.sink.first),  # delay 1 cycle
        ]

        self.sync += [
            If(data_fifo.sink.valid & data_fifo.sink.first  & ~streaming, 
                latched_header.eq(data_fifo.sink.data)
            ),
            If(data_fifo.sink.valid & data_fifo_sink_second & ~streaming,
                latched_ts.eq(data_fifo.sink.data),
            )
        ]

        # ---------------------------
        # 2. Start streaming when time has come
        # ---------------------------
        self.sync += [
            If((self.now >= latched_ts) & (frame_count == 2) & data_fifo.source.valid,
                streaming.eq(1)
            )
        ]

        # ---------------------------
        # 3. Streaming logic
        # ---------------------------

        # Default: not reading FIFO
        self.comb += data_fifo.source.ready.eq(0)

        stream_enable = Signal()
        self.comb += stream_enable.eq(streaming & data_fifo.source.valid)

        # Assign output
        self.comb += [
            source.valid.eq(stream_enable),
            source.data.eq(data_fifo.source.data)
        ]

        # Pop FIFO when downstream is ready
        self.comb += data_fifo.source.ready.eq((source.ready & stream_enable) | meta_data)

        # Count frames
        self.sync += [
            If((stream_enable & source.ready) | meta_data,
                frame_count.eq(frame_count + 1)
            )
        ]
        # ---------------------------
        # 4. Reset for next packet
        # ---------------------------
        self.sync += [
            If((frame_count == 1023) & stream_enable & source.ready,
                frame_count.eq(0),
                streaming.eq(0)
            )
        ]
