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
from litex_m2sdr.gateware.ad9361.scheduler_simple import Scheduler

CLK_HZ = 50000000  # sys clock for the sim (20 ns period)
frames_per_packet = 1024
data_width = 64
class Top(Module):
    def __init__(self):
        # If your scheduler normally runs in "rfic", keep it in sys for this sim:
        self.submodules.sched = Scheduler(frames_per_packet=frames_per_packet, data_width=data_width, max_packets=8)

        # Simple free-running timebase for 'now'
        self.now = Signal(64)
        self.sync += self.now.eq(self.now + 1)
        self.comb += self.sched.now.eq(self.now)
        self.reset = self.sched.reset

        # Shorthand handles
        self.sink   = self.sched.sink
        self.source = self.sched.source
        self.data_fifo = self.sched.data_fifo
        # Always accept output (no backpressure) so it's easy to observe
        self.comb += [ self.source.ready.eq(1)
                    ]

def drive_packet(dut, ts_when_due, n_words, header = 0xDEADBEEF):
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
        # Present data + meta for one cycle
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(i == 0)
        yield dut.sink.last.eq(i == (n_words - 1))
        if i == 0:
            yield dut.sink.data.eq(header)
        elif i == 1:
            yield dut.sink.data.eq(ts_when_due)
        else:
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

def wait(dut, wait_cycles):
    """Wait for a number of cycles with reset asserted."""
    for _ in range(wait_cycles):
        yield dut.reset.eq(1)
        yield
    yield dut.reset.eq(0)

def stimulus(dut):
    print("[stim] Starting stimulus...")
    time = 0
    yield from wait(dut, wait_cycles = 20)
    time += 20

    yield from drive_packet(dut, 
                            ts_when_due = frames_per_packet * 10, 
                            n_words = frames_per_packet * 1)
    time += frames_per_packet * 1
    yield from wait(dut, 
                    wait_cycles = frames_per_packet * 8)
    time += frames_per_packet * 8

    yield from drive_packet(dut, 
                            ts_when_due = frames_per_packet * 12, 
                            n_words = frames_per_packet * 1)
    yield from wait(dut, wait_cycles = 3000)

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
