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
    parser.add_argument("-r", "--rescan",     action="store_true",      default=True,       help="Enable PCIe rescan after flashing.")
    args = parser.parse_args()

    # Warn when flashing non-default bitstream (file )
    if "litex_m2sdr_m2_pcie_x1_operational.bin" in str(args.bitstream) and args.offset != 0x00800000:
        # print in red the warning 
        print("\033[91m", end="")
        print(f"Warning: You are flashing a multiboot bitstream: {args.bitstream} in non-default Operational offset: 0x{args.offset:08x}")
        print("\033[0m", end="")
        print(f"Add -o 0x00800000 to flash at the correct offset.")
        print("Flashing aborted.")
        return
    elif (("litex_m2sdr_m2_pcie_x1.bin" in str(args.bitstream)) or ("litex_m2sdr_m2_pcie_x1_fallback.bin" in str(args.bitstream))) and args.offset != 0x00000000:
        print("\033[91m", end="")
        print(f"Warning: You are flashing a non-multiboot bitstream: {args.bitstream} in non-default offset: 0x{args.offset:08x}")
        print("\033[0m", end="")
        print(f"Add -o 0x00000000 to flash at the correct offset.")
        print("Flashing aborted.")
        return
    else:
        confirm = input(f"Are you sure you want to flash the bitstream {args.bitstream} at offset 0x{args.offset:08x} ? \n(yes/no): ")
        if confirm.lower() not in ["yes", "y"]:
            print("Flashing aborted.")
            return


    # Flash with selected device.
    flash_bitstream(args.bitstream, args.offset, args.device_num)

    # PCIe Rescan and driver Remove/Reload.
    if args.rescan:
        subprocess.run("./rescan.py")

if __name__ == "__main__":
    main()
