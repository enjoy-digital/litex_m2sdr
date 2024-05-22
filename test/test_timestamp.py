#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse

from litex import RemoteClient

# Constants ----------------------------------------------------------------------------------------

TIME_CONTROL_ENABLE = (1 << 0)
TIME_CONTROL_LATCH  = (1 << 1)
TIME_CONTROL_SET    = (1 << 2)

# Test Time ----------------------------------------------------------------------------------------

def test_time(enable=1, set_value=None, loops=16):
    # Create Bus.
    bus = RemoteClient()
    bus.open()

    # Parameters.
    loop = 0

    # Configure Timestamp module.
    bus.regs.timestamp_control.write(enable * TIME_CONTROL_ENABLE | TIME_CONTROL_LATCH)

    # Get Timestamp rate.
    print(f"Timestamp rate: {bus.regs.timestamp_rate.read():d}")

    # Set Timestamp.
    if set_value is not None:
        print(f"Set Timestamp to {int(set_value):d}...")
        bus.regs.timestamp_set_time.write(int(set_value))
        bus.regs.timestamp_control.write(enable * TIME_CONTROL_ENABLE | TIME_CONTROL_SET)

    # Read Time from Timestamp module.
    print("Read Timestamp...")
    loop = 0
    while loop < loops:
        bus.regs.timestamp_control.write(enable * TIME_CONTROL_ENABLE | TIME_CONTROL_LATCH)
        r = f"Timestamp (cycles): {bus.regs.timestamp_latch_time.read()} "
        print(r)
        loop += 1
        time.sleep(1)

    # Close Bus.
    bus.close()

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--enable", default=1,    type=int,   help="Timestamp Enable.")
    parser.add_argument("--set",    default=None, type=float, help="Set Timestamp to provided value on next PPS edge.")
    parser.add_argument("--loops",  default=8,    type=int,   help="Test Loops.")
    args = parser.parse_args()

    test_time(enable=args.enable, set_value=args.set, loops=args.loops)

if __name__ == "__main__":
    main()
