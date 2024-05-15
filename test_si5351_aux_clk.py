#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
from litex import RemoteClient

bus = RemoteClient()
bus.open()

def latch_and_read():
    bus.regs.aux_clk_measurement_latch.write(1)
    return bus.regs.aux_clk_measurement_value.read()

num_measurements    = 10
delay_between_tests = 1

# Initialize the previous value
previous_value = latch_and_read()

for i in range(num_measurements):
    time.sleep(delay_between_tests)
    current_value = latch_and_read()
    # Compute the difference between the current and previous values
    delta_value = current_value - previous_value
    frequency_mhz = delta_value / (delay_between_tests * 1e6)
    print(f"Measurement {i + 1}: Clock Frequency: {frequency_mhz:.2f} MHz")
    # Update the previous value
    previous_value = current_value

bus.close()
