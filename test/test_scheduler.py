
import argparse
from litex import RemoteClient


class SchedulerDriver:
    """Hardware interface for Scheduler inside AD9361 module"""

    def __init__(self, bus, name):
        self.bus = bus
        self.name = name

    def _get_reg(self, reg_suffix):
        return getattr(self.bus.regs, f"{self.name}_{reg_suffix}")

    def read_ts(self):
        """Read current timestamp in """
        return self._get_reg("current_ts").read()
    
    def read_time_now(self):
        """Read current timestamp in """
        return self._get_reg("now").read()

    def write_time_now(self, time_ns):
        """Set new time value in nanoseconds"""
        self._get_reg("manual_now").write(int(time_ns))        
        self._get_reg("set_ts").write(0b1)
        self._get_reg("set_ts").write(0b0)

    def set_adjustment_ns(self, adjustment_ns):
        """Set time adjustment value in nanoseconds"""
        self._get_reg("time_adjustment").write(int(adjustment_ns))
    

def main():
    # parser = argparse.ArgumentParser()
    # parser.add_argument("--header",       default="header",       help="Header Module Name.")
    # parser.add_argument("--frame-size",   default=1024, type=int, help="Header Frame Size.")
    # parser.add_argument("--loops",        default=8,    type=int, help="Test Loops.")
    # args = parser.parse_args()
    bus = RemoteClient()
    bus.open()

    # Scheduler Driver.
    scheduler = SchedulerDriver(bus=bus, name="scheduler")

    timestamp = scheduler.read_ts()
    print(f"Current Timestamp: {timestamp} ns")
    now = scheduler.read_time_now()
    print(f"Current Time Now: {now} ns")
    new_time = 1697050000000000
    print(f"Setting Time Now to: {new_time} ns")
    scheduler.write_time_now(new_time)
    now = scheduler.read_time_now()
    print(f"Current Time Now: {now} ns")

    
    # # Dump all CSR registers of the SoC
    # for name, reg in bus.regs.__dict__.items():
    #     print("0x{:08x} : 0x{:08x} {}".format(reg.addr, reg.read(), name))




if __name__ == "__main__":
    main()
