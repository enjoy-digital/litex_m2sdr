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

# Clk Driver ---------------------------------------------------------------------------------------

class ClkDriver:
    """
    Driver for a clock measurement.

    This driver latches the clock counter, reads its value, and computes the frequency (in MHz)
    based on the elapsed time and the counter delta.
    """
    def __init__(self, bus, clk_key, description):
        self.bus         = bus
        self.clk_key     = clk_key
        self.description = description
        self.latch_reg   = getattr(self.bus.regs, f"clk_measurement_{clk_key}_latch")
        self.value_reg   = getattr(self.bus.regs, f"clk_measurement_{clk_key}_value")

        # Initialize by latching and reading the first value.
        self.latch()
        self.prev_value = self.read()
        self.prev_time  = time.time()

    def latch(self):
        """Latch the current counter value."""
        self.latch_reg.write(1)

    def read(self):
        """Read the current counter value."""
        return self.value_reg.read()

    def update(self):
        """
        Latch and read a new counter value, then compute the frequency based on the difference.

        Returns:
            float: Frequency in MHz.
        """
        self.latch()
        current_value   = self.read()
        current_time    = time.time()
        elapsed         = current_time  - self.prev_time
        delta           = current_value - self.prev_value
        frequency       = delta / (elapsed * 1e6)  # Convert to MHz
        self.prev_value = current_value
        self.prev_time  = current_time
        return frequency

# Test Frequency -----------------------------------------------------------------------------------

def test_frequency(num_measurements=10, delay_between_tests=1.0):
    bus = RemoteClient()
    bus.open()

    # Create a ClkDriver for each clock.
    clk_drivers = {clk: ClkDriver(bus, clk, desc) for clk, desc in CLOCKS.items()}
    max_name_len = max(len(desc) for desc in CLOCKS.values())

    for meas in range(num_measurements):
        time.sleep(delay_between_tests)
        for clk, driver in clk_drivers.items():
            freq = driver.update()
            print(f"Measurement {meas+1}, {driver.description:>{max_name_len}}: Frequency: {freq:.2f} MHz")

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
