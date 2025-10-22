#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse
import os 
from litex import RemoteClient

# Header Driver ------------------------------------------------------------------------------------

class HeaderDriver:
    def __init__(self, bus, name):
        self.last_tx_header    = getattr(bus.regs, f"{name}_last_tx_header")
        self.last_rx_header    = getattr(bus.regs, f"{name}_last_rx_header")
        self.last_tx_timestamp = getattr(bus.regs, f"{name}_last_tx_timestamp")
        self.last_rx_timestamp = getattr(bus.regs, f"{name}_last_rx_timestamp")

# Test Header --------------------------------------------------------------------------------------

def test_header(header="header", loops=16):
    # Create Bus.
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    csr_path = os.path.join(root_dir, "csr.csv")

    bus = RemoteClient(csr_csv = csr_path)
    bus.open()

    # Header Driver.
    header = HeaderDriver(bus=bus, name=f"{header}")

    # Status Loop.
    loop = 0
    while loop < loops:
        if (loop % 8) == 0:
            print("       TX_HEADER     TX_TIMESTAMP        RX_HEADER     RX_TIMESTAMP")
        tx_header    = header.last_tx_header.read()
        rx_header    = header.last_rx_header.read()
        tx_timestamp = header.last_tx_timestamp.read()
        rx_timestamp = header.last_rx_timestamp.read()
        print(f"{tx_header:016x} {tx_timestamp:016x} {rx_header:016x} {rx_timestamp:016x}")
        time.sleep(1)
        loop += 1

    # Close Bus.
    bus.close()

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--header",       default="header",       help="Header Module Name.")
    parser.add_argument("--frame-size",   default=1024, type=int, help="Header Frame Size.")
    parser.add_argument("--loops",        default=8,    type=int, help="Test Loops.")
    args = parser.parse_args()

    test_header(header=args.header, loops=args.loops)

if __name__ == "__main__":
    main()
