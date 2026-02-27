#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse
import datetime
import os

from litex import RemoteClient

# Time Module Driver -------------------------------------------------------------------------------

class TimeDriver:
    """Hardware interface for time generator module"""

    def __init__(self, bus, name):
        self.bus = bus
        self.name = name

    def _get_reg(self, reg_suffix):
        return getattr(self.bus.regs, f"{self.name}_{reg_suffix}")

    def enable(self):
        """Enable time generator"""
        self._get_reg("control").write(0b1)

    def disable(self):
        """Disable time generator"""
        self._get_reg("control").write(0b0)

    def read_ns(self):
        """Read current time in nanoseconds"""
        # Pulse read trigger (Bit1)
        ctrl = self._get_reg("control").read()
        self._get_reg("control").write(ctrl | 0b10)
        self._get_reg("control").write(ctrl & ~0b10)
        return self._get_reg("read_time").read()

    def write_ns(self, time_ns):
        """Set new time value in nanoseconds"""
        self._get_reg("write_time").write(int(time_ns))
        # Pulse write trigger (Bit2)
        ctrl = self._get_reg("control").read()
        self._get_reg("control").write(ctrl | 0b100)
        self._get_reg("control").write(ctrl & ~0b100)

    def set_adjustment_ns(self, adjustment_ns):
        """Set time adjustment value in nanoseconds"""
        self._get_reg("time_adjustment").write(int(adjustment_ns))


# Time conversion helper functions -----------------------------------------------------------------

def parse_set_time(arg: str) -> int:
    """Parse 'arg' into an integer nanosecond value.
       - 'now': use current system time in ns.
       - Otherwise: parse as a float, then convert to int.
    """
    if arg.lower() == "now":
        return int(time.time_ns())
    return int(float(arg))


def unix_to_datetime(unix_timestamp_ns: int) -> str:
    """Convert a Unix nanosecond timestamp into local date/time: YYYY-MM-DD HH:MM:SS.nnn"""
    unix_timestamp_s = unix_timestamp_ns / 1e9
    dt = datetime.datetime.fromtimestamp(unix_timestamp_s)
    return dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]

# Main ---------------------------------------------------------------------------------------------
def main():
    default_csr_csv = os.path.join(os.path.dirname(__file__), "csr.csv")
    parser = argparse.ArgumentParser(
        description="Set/read LiteX M2SDR hardware time (ns).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--csr-csv",  default=default_csr_csv,   help="CSR definition file")
    parser.add_argument("--host",     default="localhost",       help="Remote host (default: localhost)")
    parser.add_argument("--port",     default=1234, type=int,    help="Remote port (default: 1234)")
    parser.add_argument("--set",      default=None,              help="Set time to 'now' or a numeric value (e.g. 10e12).")
    parser.add_argument("--interval", type=float,   default=0.2, help="Interval between hardware time reads in seconds")
    parser.add_argument("--duration", type=float,   default=5.0, help="Total duration for reading hardware time in seconds")
    parser.add_argument("--enable",   default=1,    type=int,    help="Enable Time Generator (1=enabled, 0=disabled)")
    args = parser.parse_args()

    bus = RemoteClient(host=args.host, port=args.port, csr_csv=args.csr_csv)
    bus.open()

    # Instantiate the TimeDriver using the CSR name "time_gen"
    time_driver = TimeDriver(bus, "time_gen")

    # Enable or disable time generator.
    if args.enable:
        time_driver.enable()
    else:
        time_driver.disable()

    # Optionally set a new timestamp.
    if args.set is not None:
        t_ns = parse_set_time(args.set)
        print(f"Setting Timestamp to {t_ns} ns  ({unix_to_datetime(t_ns)})")
        time_driver.write_ns(t_ns)
        # Allow a moment for the write to take effect.
        time.sleep(0.001)

    # Display initial timestamp.
    initial_ts = time_driver.read_ns()
    print(f"Initial Timestamp: {initial_ts} ns  ({unix_to_datetime(initial_ts)})")

    # Calculate the number of steps based on duration and interval.
    steps = int(args.duration / args.interval)
    for i in range(steps):
        ts = time_driver.read_ns()
        print(f"{i+1:2d}> {ts} ns  ({unix_to_datetime(ts)})")
        time.sleep(args.interval)

    bus.close()


if __name__ == "__main__":
    main()
