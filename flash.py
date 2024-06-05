#!/usr/bin/env python3

import os
import time
import subprocess
import argparse

from software import get_pcie_device_id, remove_pcie_device, rescan_pcie_bus

def main():
    parser = argparse.ArgumentParser(description="Flash FPGA bistream over PCIe")
    parser.add_argument('bitstream', help='Path to the bitstream file')
    parser.add_argument('-o', '--offset', type=lambda x: int(x, 0), default=0x00000000, help='Offset for flashing (default: 0x00000000)')
    args = parser.parse_args()

    print("Flashing Board over PCIe...")
    subprocess.run(f"cd software/user && ./m2sdr_util flash_write ../../{args.bitstream} {args.offset}", shell=True)
    subprocess.run("cd software/user && ./m2sdr_util flash_reload", shell=True)
    time.sleep(1)

    print("Removing Driver...")
    subprocess.run("sudo rmmod litepcie", shell=True)
    device_ids = [
        get_pcie_device_id("0x10ee", "0x7021"),
        get_pcie_device_id("0x10ee", "0x7022"),
        get_pcie_device_id("0x10ee", "0x7024"),
    ]

    print("Removing Board from PCIe Bus...")
    for device_id in device_ids:
        if device_id:
            remove_pcie_device(device_id)

    print("Rescanning PCIe Bus...")
    rescan_pcie_bus()

    print("Loading Driver...")
    subprocess.run("sudo sh -c 'cd software/kernel && ./init.sh'", shell=True)

    print("Done!")

if __name__ == '__main__':
    main()
