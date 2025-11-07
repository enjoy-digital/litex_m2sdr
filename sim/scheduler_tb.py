#!/usr/bin/env python3

from migen import *
from migen.genlib.cdc import ClockDomainsRenamer
from litex.soc.interconnect import stream

from litex.gen import *
# import your actual scheduler and layouts

import argparse
import sys 
import subprocess
sys.path.append("..")  # add parent directory to path
from litex_m2sdr.gateware.ad9361.scheduler import Scheduler

CLK_HZ = 50000000  # sys clock for the sim (20 ns period)

class Top(Module):
    def __init__(self):
        # If your scheduler normally runs in "rfic", keep it in sys for this sim:
        self.submodules.sched = Scheduler(packet_size=8176, data_width=64, max_packets=4)

        # Simple free-running timebase for 'now'
        self.now = Signal(64)
        self.sync += self.now.eq(self.now + 1)
        self.comb += self.sched.now.eq(self.now)
        self.reset = self.sched.reset

        # Shorthand handles
        self.sink   = self.sched.sink
        self.source = self.sched.source
        self.data_fifo = self.sched.data_fifo
        #self.fsm = self.sched.fsm
        # Always accept output (no backpressure) so it's easy to observe
        self.comb += [ self.source.ready.eq(1)
                    ]

def drive_packet(dut, ts_when_due, n_words):
    """Drive one packet into sched.sink with a given timestamp and N 64-bit words."""
    print(f"[stim] Driving packet with ts={ts_when_due} n_words={n_words}")
    
    # Start consumption
    yield dut.sink.valid.eq(1)
    sink_ready = (yield dut.sink.ready)
    # Wait until FIFO can accept the first word
    while sink_ready == 0: # yield signal : Migen way to read signal value
        print("[stim] Waiting for sink.ready...")
        yield # advances simulation by one cycle

    # Drive the frames
    for i in range(n_words):
        print(f"[stim] Driving word {i}")
        first = 1 if i == 0 else 0
        last  = 1 if i == (n_words - 1) else 0
        # Present data + meta for one cycle
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(first)
        yield dut.sink.last.eq(last)
        # timestamp field is part of dma_layout_with_ts
        yield dut.sink.timestamp.eq(ts_when_due)
        yield dut.sink.data.eq(i)  # any test pattern

        # Wait for handshake
        while True:
            ready = (yield dut.sink.ready)
            yield
            if ready: break

    # Deassert after last word
    yield dut.sink.valid.eq(0)
    yield dut.sink.first.eq(0)
    yield dut.sink.last.eq(0)
    print(f"[stim] Finished driving packet with ts={ts_when_due}")


def stimulus(dut):
    print("[stim] Starting stimulus...")
    frames_per_packet = 8176 // 8  # 64-bit words (1022 frames per packet)
    number_of_packets = 4
    data_size = frames_per_packet * number_of_packets
    # Let 'now' run a little
    wait = 20
    for _ in range(wait): 
        yield dut.reset.eq(1)
        yield
    yield dut.reset.eq(0)
    # Packet with timestamp in the future → should wait, then stream when due
    ts1 = 500
    yield from drive_packet(dut, ts1, data_size)

    # Let time advance; observe that streaming begins only when now >= ts1
    wait = 2 * ts1
    for _ in range(wait): yield

    # A second packet due later
    ts2 = data_size + wait + 2*frames_per_packet
    yield from drive_packet(dut, ts2, frames_per_packet * 1)
    for _ in range(3000): yield

def main():
    argparser = argparse.ArgumentParser(description="Scheduler Testbench")
    argparser.add_argument("--gtk",         action="store_true",        help="open GTKWave at end of simulation")
    argparser.add_argument("--vcd-name",   default="scheduler_tb.vcd", help="VCD output filename")
    args = argparser.parse_args()

    top = Top()
    print("Top created, starting simulation...")
    gens = [
        stimulus(top) 
    ]
    run_simulation(
        top, gens,
        clocks={"sys": 1e9/CLK_HZ},      # 10 ns period
        vcd_name=args.vcd_name # open in GTKWave    
    )
    if args.gtk:
        subprocess.run("gtkwave scheduler_tb.vcd gtkwave_config.sav", shell=True)

if __name__ == "__main__":
    main()
