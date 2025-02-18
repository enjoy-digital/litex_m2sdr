#!/usr/bin/env python3

import time
import argparse
import datetime
import SoapySDR

def parse_set_time(arg: str) -> int:
    """Parse 'arg' into an integer nanosecond value.
       - 'now': use current system time in ns.
       - otherwise: parse as a float, then convert to int (to handle 10e12, etc.).
    """
    if arg.lower() == "now":
        return int(time.time_ns())
    return int(float(arg))

def unix_to_datetime(unix_timestamp_ns: int) -> str:
    """Convert a Unix nanosecond timestamp into local date/time: YYYY-MM-DD HH:MM:SS.nnn"""
    unix_timestamp_s = unix_timestamp_ns / 1e9
    dt = datetime.datetime.fromtimestamp(unix_timestamp_s)
    return dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]

def main():
    parser = argparse.ArgumentParser(description="Set/read LiteXM2SDR hardware time (ns).")
    parser.add_argument("--set", help="Set time to 'now' or a numeric value (e.g. 10e12).", default=None)
    args = parser.parse_args()

    sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})
    if args.set:
        t_ns = parse_set_time(args.set)
        sdr.setHardwareTime(t_ns, "")

    interval = 0.2
    total    = 5
    steps    = int(total / interval)
    for i in range(steps):
        hw_ns = sdr.getHardwareTime("")
        print(f"{i+1:2d}> {hw_ns} ns  ({unix_to_datetime(hw_ns)})")
        time.sleep(interval)

if __name__ == "__main__":
    main()
