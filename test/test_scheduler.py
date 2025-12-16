#!/usr/bin/env python3
"""
AD9361 Scheduler TX Driver Test
Author: Ismail Essaidi
Description:
  This script connects to a running litex_server over PCIe and interacts
  with the Scheduler hardware block inside the AD9361 module.
"""

from litex import RemoteClient
import time
import os
import subprocess
import argparse

class SchedulerDriver:
    """Interface for AD9361 TX Scheduler."""

    def __init__(self, bus, name="ad9361_scheduler_tx"):
        self.bus = bus
        self.name = name

    def _reg(self, suffix):
        """Dynamic CSR accessor."""
        try:
            return getattr(self.bus.regs, f"{self.name}_{suffix}")
        except AttributeError:
            raise AttributeError(f"CSR '{self.name}_{suffix}' not found in CSR map.")
    # ───────────────────────────────────────────────
    #  READ METHODS
    # ───────────────────────────────────────────────

    def enable_scheduler(self, enable, reset=0):
        """add (enable) scheduler in TX pipeline"""
        enable_scheduler = getattr(self.bus.regs, "ad9361_scheduler_tx").read()
        if reset:
            getattr(self.bus.regs, "ad9361_scheduler_tx").write(0)
        else:
            if enable:
                getattr(self.bus.regs, "ad9361_scheduler_tx").write(enable_scheduler | 0b1)
            else:
                getattr(self.bus.regs, "ad9361_scheduler_tx").write(enable_scheduler & ~0b1)
        enable_scheduler = getattr(self.bus.regs, "ad9361_scheduler_tx").read()
        print(f"Scheduler Status: {enable_scheduler}")
        

    def enable_header(self, enable):
        """enable header"""
        header_tx_control = getattr(self.bus.regs, "header_tx_control").read()
        if enable:
            getattr(self.bus.regs, "header_tx_control").write(header_tx_control | 0b10)
        else:
            getattr(self.bus.regs, "header_tx_control").write(header_tx_control & ~0b10)
        header_tx_control = getattr(self.bus.regs, "header_tx_control").read()
        print(f"Header Control Status: {header_tx_control}")
        
    def enable_write(self, enable):
        """Enable or disable manual time writing."""
        ctrl = self._reg("control").read()
        if enable:
            self._reg("control").write(ctrl | 0b100)  # Set enable bit
        else:
            self._reg("control").write(ctrl & ~0b100)  # Clear enable bit

    def read_current_ts(self):
        """Read timestamp at FIFO output."""
        return self._reg("current_ts").read()
    
    def read_now(self):
        """Read current time in nanoseconds"""
        # Pulse read trigger (Bit1)
        ctrl = self._reg("control").read()
        self._reg("control").write(ctrl | 0b10)
        self._reg("control").write(ctrl & ~0b10)
        return self._reg("read_time").read()
    
    # ───────────────────────────────────────────────
    #  WRITE METHODS
    # ───────────────────────────────────────────────
    # def reset(self):
    #     """Pulse reset bit."""
    #     self._reg("reset").write(1)

    def write_manual_time(self, time_ns):
        """Set new time value in nanoseconds"""
        self._reg("write_time").write(int(time_ns))
        # Pulse write trigger (Bit2)
        ctrl = self._reg("control").read()
        self._reg("control").write(ctrl | 0b100)
        self._reg("control").write(ctrl & ~0b100)

    # ───────────────────────────────────────────────
    #  DEBUG UTILITIES
    # ───────────────────────────────────────────────
    def dump_all(self):
        """Print all CSR registers."""
        for name, reg in self.bus.regs.__dict__.items():
            print(f"0x{reg.addr:08x} : 0x{reg.read():08x} {name}")
    
    def test_time(self, epochs = 10, new_time = 0):
        for i in range(epochs):
            now = self.read_now()
            print(f"Reading RFIC clock: now = {now}")
            if i % 5 == 0:
                print(f"\nManual time writing: now = {new_time}\n")
                self.write_manual_time(new_time)
                continue
            time.sleep(1)
    


# ----------------------------------------------------------------------------
# Main test routine
# ----------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="LiteX SoC on LiteX-M2SDR.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    # Build/Load/Utilities.
    parser.add_argument("--init-rfic",           action="store_true", help="Init RFIC")
    args = parser.parse_args()

    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "."))
    csr_path = os.path.join(root_dir, "csr.csv")
    
    if (args.init_rfic):
        print("Initializing the RFIC ...")
        result = subprocess.run("cd ../litex_m2sdr/software/user && ./m2sdr_rf", shell=True, capture_output=True, text=True)
        print(result.stdout)
    else:
        print("If running for the first time try initializing the RFIC by running:\n./test_scheduler.py --init-rfic\n")

    bus = RemoteClient(csr_csv= csr_path)
    bus.open()
    print("Connected to LiteX server.\n")

    scheduler = SchedulerDriver(bus=bus, name="ad9361_scheduler_tx")
    scheduler.enable_scheduler(enable= 0,reset=1)
    scheduler.enable_scheduler(enable=1)

    scheduler.enable_header(enable=1)


    # scheduler.test_time()

    # scheduler.enable_scheduler(enable=0)
    # scheduler.test_time()
    scheduler.dump_all()

    bus.close()
    print("\nDone.\n")

  


if __name__ == "__main__":
    main()
