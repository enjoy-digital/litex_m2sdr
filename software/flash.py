#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import argparse
import subprocess

# Flash Utilities ----------------------------------------------------------------------------------

def flash_bitstream(bitstream, offset, device_num):
    print("Flashing Board over PCIe...")
    subprocess.run(f"cd user && ./m2sdr_util flash_write  -c {device_num} ../{bitstream} {offset}", shell=True)
    subprocess.run(f"cd user && ./m2sdr_util flash_reload -c {device_num}", shell=True)
    time.sleep(1)

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX-M2SDR flashing over PCIe.")
    parser.add_argument("bitstream",                                                        help="Path to the bitstream file.")
    parser.add_argument("-o", "--offset",     type=lambda x: int(x, 0), default=0x00800000, help="Offset for flashing (default: 0x00800000).")
    parser.add_argument("-c", "--device_num", type=int,                 default=0,          help="Select the device number (default = 0).")
    args = parser.parse_args()

    # Ask for confirmation before flashing.
    confirm = input("Are you sure you want to flash the bitstream? (yes/no): ")
    if confirm.lower() not in ["yes", "y"]:
        print("Flashing aborted.")
        return

    # Ask for confirmation when non-default offset.
    if args.offset != 0x00800000:
        confirm = input(f"You are flashing bitstream at non-default Operational offset: 0x{args.offset:08x} vs 0x00800000, are you sure? (yes/no): ")
        if confirm.lower() not in ["yes", "y"]:
            print("Flashing aborted.")
            return

    # Flash with selected device.
    flash_bitstream(args.bitstream, args.offset, args.device_num)

    # PCIe Rescan and driver Remove/Reload.
    subprocess.run("./rescan.py")

if __name__ == "__main__":
    main()
