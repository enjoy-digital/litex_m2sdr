#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""
test_time.py - Set/read LiteXM2SDR hardware time (ns).

This script uses the SoapySDR driver to interface with the LiteXM2SDR hardware. It can set the
hardware time to "now" or a specific nanosecond value and then repeatedly read and display the
hardware time along with its local date/time representation.

Command-line options allow you to configure the set time, the read interval, and the total duration
for reading.

Usage Example:
    ./test_time.py --set now --interval 0.2 --secs 5
"""

import time
import argparse
import datetime
import SoapySDR

# Time conversion ----------------------------------------------------------------------------------

def parse_set_time(arg: str) -> int:
    """Parse 'arg' into an integer nanosecond value.
       - 'now': use current system time in ns.
       - Otherwise: parse as a float, then convert to int (to handle 10e12, etc.).
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
    parser = argparse.ArgumentParser(
        description     = "Set/read LiteXM2SDR hardware time (ns).",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--set",                               help="Set time to 'now' or a numeric value (e.g. 10e12).", default=None)
    parser.add_argument("--interval", type=float, default=0.2, help="Interval between hardware time reads in seconds")
    parser.add_argument("--duration", type=float, default=5.0, help="Total duration for reading hardware time in seconds")
    args = parser.parse_args()

    # Open the LiteXM2SDR device using the SoapySDR driver.
    sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})

    # Set hardware time if requested.
    if args.set:
        t_ns = parse_set_time(args.set)
        sdr.setHardwareTime(t_ns, "")

    steps = int(args.duration / args.interval)
    for i in range(steps):
        hw_ns = sdr.getHardwareTime("")
        print(f"{i+1:2d}> {hw_ns} ns  ({unix_to_datetime(hw_ns)})")
        time.sleep(args.interval)

if __name__ == "__main__":
    main()
