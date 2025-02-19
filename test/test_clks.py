#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse

from litex import RemoteClient

# Constants ----------------------------------------------------------------------------------------

CLOCKS = {
    "clk0": "       Sys Clk",
    "clk1": "      PCIe Clk",
    "clk2": "AD9361 Ref Clk",
    "clk3": "AD9361 Dat Clk",
    "clk4": "  Time Ref Clk",
}

# Test Frequency -----------------------------------------------------------------------------------

def test_frequency(num_measurements=10, delay_between_tests=1.0):
    bus = RemoteClient()
    bus.open()

    max_name_len = max(len(name) for name in CLOCKS.values())

    def latch_all():
        for clk in CLOCKS:
            reg_name = f"clk_measurement_{clk}_latch"
            getattr(bus.regs, reg_name).write(1)

    def read_all():
        readings = {}
        for clk in CLOCKS:
            reg_name = f"clk_measurement_{clk}_value"
            readings[clk] = getattr(bus.regs, reg_name).read()
        return readings

    latch_all()
    previous_values = read_all()
    prev_time = time.time()

    for meas in range(num_measurements):
        time.sleep(delay_between_tests)
        latch_all()
        current_values = read_all()
        cur_time = time.time()

        elapsed = cur_time - prev_time
        prev_time = cur_time

        for clk in CLOCKS:
            delta = current_values[clk] - previous_values[clk]
            frequency = delta / (elapsed * 1e6)  # Frequency in MHz.
            print(f"Measurement {meas + 1}, {CLOCKS[clk]:>{max_name_len}}: Frequency: {frequency:.2f} MHz")
            previous_values[clk] = current_values[clk]

    bus.close()

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Frequency Measurement Script")
    parser.add_argument("--num",   default=10,  type=int,   help="Number of measurements")
    parser.add_argument("--delay", default=1.0, type=float, help="Delay between measurements (seconds)")
    args = parser.parse_args()

    test_frequency(num_measurements=args.num, delay_between_tests=args.delay)

if __name__ == "__main__":
    main()
