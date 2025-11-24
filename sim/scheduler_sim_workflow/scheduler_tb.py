#!/usr/bin/env python3

from migen import *
from migen.genlib.cdc import ClockDomainsRenamer
from litex.soc.interconnect import stream

from litex.gen import *
from ExperimentManager import ExperimentManager

# import your actual scheduler and layouts

import os, sys, argparse, subprocess

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, BASE_DIR)

from litex_m2sdr.gateware.ad9361.scheduler_simple import Scheduler
from testbench_helpers import wait, drive_packet

sys_freq = 50000000  # sys clock for the sim (20 ns period)

class Top(Module):
    def __init__(self, frames_per_packet=1024, data_width=64, max_packets=8):
        # If your scheduler normally runs in "rfic", keep it in sys for this sim
        self.submodules.top = Scheduler(frames_per_packet, data_width, max_packets)
        # Simple free-running timebase for 'now'
        self.now = Signal(64)
        self.sync += self.now.eq(self.now + 1)
        self.comb += self.top.now.eq(self.now)
        self.reset = self.top.reset

        # Shorthand handles
        self.sink   = self.top.sink
        self.source = self.top.source
        self.data_fifo = self.top.data_fifo
        # Always accept output (no backpressure) so it's easy to observe
        self.comb += [ self.source.ready.eq(1)]

class SchedulerTestbench():
    """Main testbench orchestrator using config-driven approach."""
    
    def __init__(self, dut, config = ExperimentManager(config_file ="test_config.yaml").config):

        self.dut = dut
        self.config = config
        self.wait_initial_cycles = self.config.get_global_param("wait_initial_cycles", 20)
        self.wait_final_cycles = self.config.get_global_param("wait_final_cycles", 500)
        self.frames_per_packet = self.config.get_global_param("frames_per_packet", 1024)
        self.data_width = self.config.get_global_param("data_width", 64)
        self.max_packets = self.config.get_global_param("max_packets", 8)
        self.current_time = 0

    
    def run_all_tests(self):
        """Run all tests from config."""
        print("\n[TB] ========== SCHEDULER TESTBENCH ==========")
        self.config.list_tests()
        print("\n")
        for test_id ,test_name in enumerate(self.config.get_all_tests()):
            yield from self.run_test(test_name, test_id + 1)
        
        print("\n[TB] ========== ALL TESTS COMPLETED ==========\n")
        yield
    
    def run_test(self, test_name, test_id):
        """Run a single test by name."""
        test_cfg = self.config.get_test(test_name)

        print("-" * 70)
        print(f"\n[TEST] {test_name}")
        print(f"[TEST] {test_cfg['description']}")
        print("-" * 70)
        
        # Initial wait
        yield from wait(self.wait_initial_cycles)
        self.current_time += self.wait_initial_cycles
        
        # Check if this is a dynamic packet test (num_packets) or static (packets list)
        if "num_packets" in test_cfg:
            yield from self._run_dynamic_packets(test_cfg, test_id)
        else:
            yield from self._run_static_packets(test_cfg, test_id)
        
        # Final wait
        wait_final = test_cfg.get("wait_after_all_packets", self.wait_final_cycles)
        yield from wait(wait_final)
        self.current_time += wait_final
        
        print(f"[TEST] {test_name}: DONE\n")
    
    def _run_static_packets(self, test_cfg, test_id):
        """Run test with a static list of packets."""
        packets = test_cfg.get("packets", [])
        
        for pkt_cfg in packets:
            pkt_id = pkt_cfg.get("id", 0)
            description = pkt_cfg.get("description", f"Packet {pkt_id}")
            
            # Determine timestamp (absolute or relative)
            if "timestamp" in pkt_cfg:
                ts = pkt_cfg["timestamp"]
            elif "timestamp_offset" in pkt_cfg:
                ts = self.current_time + pkt_cfg["timestamp_offset"]
            else:
                ts = self.current_time
            
            print(f"  {description}")
            yield from drive_packet(self.dut, ts_when_due=ts, packet_id=pkt_id, test_id = test_id)
            self.current_time += self.frames_per_packet
            
            # Wait between packets
            wait_between = self.config.get_global_param("wait_between_packets", 10)
            yield from wait(wait_between)
            self.current_time += wait_between
    
    def _run_dynamic_packets(self, test_cfg, test_id):
        """Run test with dynamically generated packets."""
        num_packets = test_cfg.get("num_packets", 1)
        base_offset = test_cfg.get("base_timestamp_offset", 100)
        increment = test_cfg.get("timestamp_increment", 100)
        desc_template = test_cfg.get("description_template", "Packet {id}: ts=base+{ts_offset}")
        
        base_time = self.current_time + base_offset
        
        for pkt_id in range(1, num_packets + 1):
            ts_offset = (pkt_id - 1) * increment
            ts = base_time + ts_offset
            description = desc_template.format(id=pkt_id, ts_offset=ts_offset)
            
            print(f"  {description}")
            yield from drive_packet(self.dut, ts_when_due=ts, packet_id=pkt_id, test_id = test_id)
            self.current_time += self.frames_per_packet
            
            # Wait between packets
            wait_between = self.config.get_global_param("wait_between_packets", 10)
            yield from wait(wait_between)
            self.current_time += wait_between

def stimulus(tb, test_name="all"):
    """Main stimulus function."""
    if test_name == "all":
        yield from tb.run_all_tests()
    else:
        yield from tb.run_test(test_name)

def main():
    argparser = argparse.ArgumentParser(
        description="Scheduler Testbench - LiteX-M2SDR Simulation",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
            Examples:
            # Run all tests and save VCD
            python scheduler_tb.py
            
            # Run with custom VCD directory
            python scheduler_tb.py --vcd-dir my_simulations
            
            # Open latest VCD in GTKWave automatically
            python scheduler_tb.py --gtk
            
        """
    )
    argparser.add_argument("--gtk",              action="store_true",        help="Open GTKWave at end of simulation")
    argparser.add_argument("--vcd-dir",         default="vcd_outputs",      help="Directory to store VCD files")
    argparser.add_argument("--experiment-name",  default="",                  help="Name of the experiment (used in folder and VCD naming)")
    args = argparser.parse_args()

    experiment = ExperimentManager(experiment_name=args.experiment_name, config_file="test_config.yaml", vcd_dir=args.vcd_dir)
    frames_per_packet = experiment.config.get_global_param("frames_per_packet", 1024)
    data_width = experiment.config.get_global_param("data_width", 64)
    max_packets = experiment.config.get_global_param("max_packets", 8)
    sys_freq = experiment.config.get_global_param("sys_freq", 50000000)

    top = Top(frames_per_packet, data_width, max_packets)

    tb = SchedulerTestbench(dut =  top, config = experiment.config)

    # Generate VCD filename
    vcd_path = experiment.get_vcd_path()
    experiment.generate_report()
    print("Top created, starting simulation...")
    print(f"[VCD] Output will be saved to: {vcd_path}\n")

    gens = [
        stimulus(tb, test_name="all") 
    ]
    run_simulation(
        top, gens,
        clocks={"sys": 1e9/sys_freq},    # sys clock period
        vcd_name=vcd_path                # Use generated VCD path
    )
    
    print(f"\n[VCD] Simulation complete. VCD saved to: {vcd_path}")
    
    # Open in GTKWave if requested
    if args.gtk:
        print(f"[VCD] Opening {vcd_path} in GTKWave...")
        subprocess.run(f"gtkwave {vcd_path} gtkwave_config.sav", shell=True)
    


if __name__ == "__main__":
    main()
