#!/usr/bin/env python3

import time
import SoapySDR

def format_hw_time(time_ns):
    """
    Convert a nanosecond timestamp into hh:mm:ss.us:ns notation.
    """
    # Calculate hours, minutes, seconds.
    ns_per_sec = 1_000_000_000
    hours      = time_ns // (3600 * ns_per_sec)
    remainder  = time_ns %  (3600 * ns_per_sec)
    minutes    = remainder // (60 * ns_per_sec)
    remainder  = remainder %  (60 * ns_per_sec)
    seconds    = remainder // ns_per_sec
    remainder  = remainder %  ns_per_sec

    # Split remainder into microseconds and leftover nanoseconds.
    microseconds = remainder // 1000
    leftover_ns  = remainder %  1000

    return f"{int(hours):02d}:{int(minutes):02d}:{int(seconds):02d}:" \
           f"{int(microseconds):06d}:{int(leftover_ns):03d}"

def main():
    # Open the device with the LiteXM2SDR driver.
    args = dict(driver="LiteXM2SDR")
    sdr  = SoapySDR.Device(args)

    # Poll hardware time every 200ms for a total of 5 seconds.
    interval_s   = 0.2
    total_time_s = 5.0
    steps        = int(total_time_s / interval_s)

    print(f"Polling hardware time every {interval_s}s for {total_time_s}s...\n")

    for i in range(steps):
        # Read hardware time (ns).
        t_ns = sdr.getHardwareTime("")
        # Format and print the timestamp.
        print(f"{i+1:2d}> {t_ns} ns  ({format_hw_time(t_ns)})")
        time.sleep(interval_s)

if __name__ == "__main__":
    main()
