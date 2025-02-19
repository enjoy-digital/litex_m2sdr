#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse

from litex import RemoteClient

# Constants  ---------------------------------------------------------------------------------------

# Define bit-field constants (for all AGC modules)
CONTROL_ENABLE_OFFSET     = 0
CONTROL_ENABLE_SIZE       = 1
CONTROL_CLEAR_OFFSET      = 1
CONTROL_CLEAR_SIZE        = 1
CONTROL_THRESHOLD_OFFSET  = 16
CONTROL_THRESHOLD_SIZE    = 16
STATUS_COUNT_OFFSET       = 0
STATUS_COUNT_SIZE         = 32

# Helper function to set a field within a register value.
def set_field(reg_value, offset, size, value):
    mask = ((1 << size) - 1) << offset
    return (reg_value & ~mask) | ((value << offset) & mask)

# AGCDriver ----------------------------------------------------------------------------------------

class AGCDriver:
    """
    Driver for an AGC Saturation Count module.

    This driver wraps the control and status registers of an AGC instance.
    It provides methods to enable/disable the AGC, clear the saturation count,
    set the saturation threshold, and read the current saturation count.
    """
    def __init__(self, bus, name="agc_count"):
        self.bus = bus
        self.name = name
        self.control = getattr(self.bus.regs, f"{name}_control")
        self.status  = getattr(self.bus.regs, f"{name}_status")

    def enable(self):
        reg = self.control.read()
        reg = set_field(reg, CONTROL_ENABLE_OFFSET, CONTROL_ENABLE_SIZE, 1)
        self.control.write(reg)

    def disable(self):
        reg = self.control.read()
        reg = set_field(reg, CONTROL_ENABLE_OFFSET, CONTROL_ENABLE_SIZE, 0)
        self.control.write(reg)

    def clear(self):
        reg = self.control.read()
        reg = set_field(reg, CONTROL_CLEAR_OFFSET, CONTROL_CLEAR_SIZE, 1)
        self.control.write(reg)

    def set_threshold(self, threshold):
        reg = self.control.read()
        reg = set_field(reg, CONTROL_THRESHOLD_OFFSET, CONTROL_THRESHOLD_SIZE, threshold)
        self.control.write(reg)

    def read_count(self):
        # The entire status register is the count.
        return self.status.read()

# Test AGC ------------------------------------------------------------------------------------------

def test_agc(num_measurements=10, delay=1.0, threshold=1000, enable=1, clear=False, agc_selection="rx1_low"):
    bus = RemoteClient()
    bus.open()

    # Build the full AGC instance name from the selection.
    # For example, if agc_selection is "rx1_low", then the full CSR name is "ad9361_agc_count_rx1_low".
    agc_instance = f"ad9361_agc_count_{agc_selection}"
    agc = AGCDriver(bus, name=agc_instance)

    if enable:
        agc.enable()
    else:
        agc.disable()

    agc.set_threshold(threshold)
    if clear:
        agc.clear()

    print(f"AGC Saturation Count Test (threshold = {threshold}) on {agc_instance}")
    for i in range(num_measurements):
        count = agc.read_count()
        print(f"Measurement {i+1}: Saturation Count = {count}")
        time.sleep(delay)

    bus.close()

# Main ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="AGC Saturation Count Test Script")
    parser.add_argument("--num",       default=10,    type=int,   help="Number of measurements")
    parser.add_argument("--delay",     default=1.0,   type=float, help="Delay between measurements (seconds)")
    parser.add_argument("--threshold", default=1000,  type=int,   help="Saturation threshold (absolute value)")
    parser.add_argument("--enable",    default=1,     type=int,   help="Enable AGC (1=enabled, 0=disabled)")
    parser.add_argument("--clear",     action="store_true",       help="Clear saturation count at start")
    parser.add_argument("--agc",       default="rx1_low",         help="AGC selection: rx1_low, rx1_high, rx2_low, rx2_high")
    args = parser.parse_args()

    test_agc(
        num_measurements = args.num,
        delay            = args.delay,
        threshold        = args.threshold,
        enable           = args.enable,
        clear            = args.clear,
        agc_selection    = args.agc,
    )

if __name__ == "__main__":
    main()
