#
# This file is responsible for scheduling TX based on timestamps.
#
# author: Ismail Essaidi
# 


from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer
from litex.gen import *
from litepcie.common import *
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *


class Scheduler(LiteXModule): 
    def __init__(self, frames_per_packet = 1024, data_width=64, max_packets=8, meta_data_frames_per_packet = 2, init=0, with_csr = True): # just to start then maybe expand it to more packets
        assert frames_per_packet % (data_width // 8) == 0 # packet size must be multiple of data width in bytes (a frame is 8 bytes)
        assert meta_data_frames_per_packet == 2  # we have 2 frames for metadata: header + timestamp

        # Inputs/Outputs
        self.reset = Signal() # i
        self.sink   = sink   = stream.Endpoint(dma_layout(data_width)) # i  (from TX_CDC)
        self.source = source = stream.Endpoint(dma_layout(data_width)) # o  (to gpio_tx_unpacker)
        self.timestamp = Signal(64) # i (timestamp of the packet at fifo output)
        self.header = Signal(64) # i (header of the packet at fifo output)
        self.now = Signal(64) # i (current time: drive in RFIC domain)
        self.write_time_manually = Signal()
        self.manual_now = Signal(64)

        # Data Fifo: stores multiple packets
        self.data_fifo = data_fifo = stream.SyncFIFO(layout=dma_layout(data_width), depth=frames_per_packet * max_packets) 

        # Time Handling.
        self.sync += [
            # Disable: Reset Time to 0.
            If(self.write_time_manually,
                self.now.eq(self.manual_now),
            # Increment.
            ).Else(
                self.now.eq(self.now + 1),
            )
        ]

        # States
        self.streaming = streaming =  Signal(reset=0)
        frame_count = Signal(10)   # enough for 0..1023 (1024 frames)
        self.latched_ts = latched_ts = Signal(64)
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
        # Capture header + timestamp
        # ---------------------------
        # This triggers only once per packet.
        is_header    = Signal()
        is_timestamp = Signal()
        self.comb += [ 
            is_header.eq(data_fifo.source.valid & ~streaming & (frame_count == 0)),
            is_timestamp.eq(data_fifo.source.valid & ~streaming & (frame_count == 1)),
        ]

        # ------------------------------------------------
        # Latching Metadata only when we consume from Fifo
        # ------------------------------------------------
        self.sync += [
            If(data_fifo.source.ready & data_fifo.source.valid,
                If(is_header,   
                    latched_header.eq(data_fifo.source.data)
                ),
                If(is_timestamp,
                    latched_ts.eq(data_fifo.source.data),
                )
            )
        ]

        # ---------------------------
        # Start streaming when time has come
        # ---------------------------
        can_start = Signal()
        self.comb += can_start.eq((self.now >= latched_ts) &
                                (frame_count == 2) &
                                data_fifo.source.valid)
        self.sync += [
            If(can_start & ~streaming,
                streaming.eq(1)
            )
        ]
        # ---------------------------
        # Assign output
        # ---------------------------
        self.comb += [
            source.valid.eq(streaming & data_fifo.source.valid),
            source.data.eq(data_fifo.source.data),
            source.first.eq(data_fifo.source.first & streaming),
            source.last.eq(data_fifo.source.last & streaming)
        ]

        # Pop FIFO when downstream is ready
        pop_meta = is_header | is_timestamp
        self.comb += data_fifo.source.ready.eq((source.ready & streaming) | pop_meta)

        # Count frames
        self.sync += [
            If(data_fifo.source.ready & data_fifo.source.valid,
                frame_count.eq(frame_count + 1)
            )
        ]
        # ---------------------------
        # Reset for next packet
        # ---------------------------
        self.sync += [
            If((frame_count == frames_per_packet - 1) & streaming & data_fifo.source.ready & data_fifo.source.valid,
                frame_count.eq(0),
                streaming.eq(0)
            )
        ]
        # ────────────────────────────────────────────────
        #      CSR Exposure
        # ────────────────────────────────────────────────
        if with_csr:
            self.add_csr()

    
    def add_csr(self):
        # Control
        self._control = CSRStorage(fields=[
            CSRField("read", size=1, offset=1, pulse=True),
            CSRField("write", size=1, offset=2, pulse=True),
            CSRField("read_current_ts", size=1, offset=3, pulse=True),
        ])

        # Status
        self._fifo_level = CSRStatus(16, description="Current FIFO level in words")
        self._current_ts = CSRStatus(64, description="Current Timestamp at FIFO output")
        self._read_time  = CSRStatus(64, description="Current time from rfic domain")
        self._write_time = CSRStorage(64, description="Write Time (ns) (SW Time -> FPGA).")
        # ---------------------------
        # Important to Sync from scheduler domain to sys domain

        # Time Write (SW -> FPGA).
        self.specials += MultiReg(self._write_time.storage, self.manual_now, "rfic")
        time_write_ps = PulseSynchronizer("sys", "rfic")
        self.submodules += time_write_ps
        self.comb += time_write_ps.i.eq(self._control.fields.write)
        self.comb += self.write_time_manually.eq(time_write_ps.o)

        # Time Read (RFIC -> SW). 
        time_read = Signal(64)
        time_read_ps = PulseSynchronizer("sys", "rfic")
        self.submodules += time_read_ps
        self.comb += time_read_ps.i.eq(self._control.fields.read)
        self.sync.rfic += If(time_read_ps.o, time_read.eq(self.now))
        self.specials += MultiReg(time_read, self._read_time.status)         # rfic -> sys
       

        # # Read current ts in the Fifo
        # current_ts = Signal(64)
        # current_ts_ps = PulseSynchronizer("sys", "rfic")
        # self.submodules += current_ts_ps
        # self.comb += current_ts_ps.i.eq(self._control.fields.read_current_ts)
        # self.sync.rfic += If(current_ts_ps.o, current_ts.eq(self.latched_ts_shadow))
        # self.specials += MultiReg(current_ts, self._current_ts.status)         # rfic -> sys

        # # Read Fifo level
        # fifo_level_sys = Signal(16)
        # self.comb += fifo_level_sys.eq(self.data_fifo.level)
        # self.specials += MultiReg(fifo_level_sys, self._fifo_level.status)
    