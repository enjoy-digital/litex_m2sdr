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
    # def read_now(self):
    #     """Read current RFIC time."""
    #     return self._reg("now").read()

    # def read_fifo_level(self):
    #     """Read FIFO fill level."""
    #     return self._reg("fifo_level").read()
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
        # self._reg("control").write(ctrl & ~0b10)
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
    


# ----------------------------------------------------------------------------
# Main test routine
# ----------------------------------------------------------------------------
def main():
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    csr_path = os.path.join(root_dir, "csr.csv")

    bus = RemoteClient(csr_csv= csr_path)
    bus.open()
    print("Connected to LiteX server.\n")

    scheduler = SchedulerDriver(bus=bus, name="ad9361_scheduler_tx")
    # scheduler.enable_write(True)
    # assert scheduler._reg("control").read() == 0x00000005, "Testing enable write"
    # print("Write enabled successfully.👍")
    # scheduler.enable_write(False)
    # assert scheduler._reg("control").read() == 0x00000001, "Testing disable write"
    # print("Write disabled successfully.👍")

    # -------------------------------------------------------------------------
    # 2. SET MANUAL TIME
    # -------------------------------------------------------------------------
    for i in range(50):
        now = scheduler.read_now()
        print(f"Now before manual write : {now}")
        if i % 5 == 0:
            new_time = 0  # reset to 0 every 5 iterations
            print(f"\nSetting manual now = {new_time}")
            scheduler.write_manual_time(new_time)
            continue
        time.sleep(1)

    bus.close()
    print("\nDone.\n")

    # --- Show FIFO head timestamp
    # current_ts = scheduler.read_current_ts()
    # # print(f"Current FIFO Head Timestamp: {current_ts}")
    # for _ in range(10):
    #     test_now = scheduler.read_test_schedule_now()
    #     temperature = (float(getattr(bus.regs, "xadc_temperature").read()) * 503.975/4096) - 273.15
    #     print(f"Main test increment counter: {test_now}")
    #     print(f"FPGA Temperature : {temperature} °C")
    #     time.sleep(0.5)

    # bus.close()
    # print("Done.")


if __name__ == "__main__":
    main()
