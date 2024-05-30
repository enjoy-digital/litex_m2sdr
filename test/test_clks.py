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

def latch_all():
    bus.regs.clk_measurement_clk0_latch.write(1)
    bus.regs.clk_measurement_clk1_latch.write(1)
    bus.regs.clk_measurement_clk2_latch.write(1)
    bus.regs.clk_measurement_clk3_latch.write(1)

def read_all():
    values = [
        bus.regs.clk_measurement_clk0_value.read(),
        bus.regs.clk_measurement_clk1_value.read(),
        bus.regs.clk_measurement_clk2_value.read(),
        bus.regs.clk_measurement_clk3_value.read()
    ]
    return values

num_measurements    = 10
delay_between_tests = 1

# Latch and read initial values for each clock
latch_all()
previous_values = read_all()
start_time = time.time()

for i in range(num_measurements):
    time.sleep(delay_between_tests)

    # Latch and read current values for each clock
    latch_all()
    current_values = read_all()
    current_time = time.time()

    # Calculate the actual elapsed time
    elapsed_time = current_time - start_time
    start_time = current_time  # Update the start_time for the next iteration

    for clk_index in range(4):
        # Compute the difference between the current and previous values
        delta_value = current_values[clk_index] - previous_values[clk_index]
        frequency_mhz = delta_value / (elapsed_time * 1e6)
        print(f"Measurement {i + 1}, Clock {clk_index}: Frequency: {frequency_mhz:.2f} MHz")

        # Update the previous value for the next iteration
        previous_values[clk_index] = current_values[clk_index]

bus.close()
