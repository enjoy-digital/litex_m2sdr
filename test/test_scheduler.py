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
    """Hardware interface for the AD9361 TX Scheduler."""

    def __init__(self, bus, name="ad9361_scheduler_tx"):
        self.bus = bus
        self.name = name

    def _get_reg(self, reg_suffix):
        """Return a register handle dynamically by suffix."""
        try:
            return getattr(self.bus.regs, f"{self.name}_{reg_suffix}")
        except AttributeError:
            raise AttributeError(f"Register '{self.name}_{reg_suffix}' not found in CSR map.")

    # ---- Read methods -------------------------------------------------------

    def read_current_ts(self):
        """Read timestamp of the packet currently at FIFO output."""
        return self._get_reg("current_ts").read()

    def read_time_now(self):
        """Read current time from the RFIC domain counter."""
        return self._get_reg("now").read()
    
    def read_test_schedule_now(self):
        return getattr(self.bus.regs, f"main_test_time").read()

    def read_fifo_level(self):
        """Return current FIFO fill level (in words)."""
        return self._get_reg("fifo_level").read()

    def read_flags(self):
        """Return bitfield of scheduler flags (full, empty, etc)."""
        return self._get_reg("flags").read()

    # ---- Write / control methods -------------------------------------------

    def reset(self):
        """Reset the scheduler FSM."""
        self._get_reg("reset").write(1)

    def write_time_now(self, time_ns):
        """
        Set a new manual time value (in nanoseconds).
        This also toggles the 'use_manual' control bit in _set_ts.
        """
        self._get_reg("manual_now").write(int(time_ns))
        # Toggle enable bit to apply
        self._get_reg("set_ts").write(1)
        self._get_reg("set_ts").write(0)
    
    def dump_csr_regs(self):
        for name, reg in self.bus.regs.__dict__.items():
            print("0x{:08x} : 0x{:08x} {}".format(reg.addr, reg.read(), name))
    


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

    # --- Read current status
    now = scheduler.read_time_now()
    print(f"Current Time (now): {now}")

    fifo_level = scheduler.read_fifo_level()
    print(f"FIFO Level: {fifo_level}")

    flags = scheduler.read_flags()
    print(f"Flags: 0x{flags:08x}")

    # --- Set manual time value
    new_time = 1697050000000000
    print(f"\nSetting manual time to {new_time} ns ...")
    scheduler.write_time_now(new_time)

    # --- Verify update
    now = scheduler.read_time_now()
    print(f"Updated Time (now): {now}")

    # --- Show FIFO head timestamp
    # current_ts = scheduler.read_current_ts()
    # print(f"Current FIFO Head Timestamp: {current_ts}")
    for i in range(10):
        test_now = scheduler.read_test_schedule_now()
        print(f"Test increment: {test_now}")
        time.sleep(1)
    
    bus.close()
    print("Done.")


if __name__ == "__main__":
    main()
