#!/usr/bin/env python3

import os
import time
import argparse
import subprocess

from software import get_pcie_device_id, remove_pcie_device, rescan_pcie_bus

# Flash Utilities ----------------------------------------------------------------------------------

def flash_bitstream(bitstream, offset):
    print("Flashing Board over PCIe...")
    subprocess.run(f"cd user && ./m2sdr_util flash_write ../../{bitstream} {offset}", shell=True)
    subprocess.run("cd user && ./m2sdr_util flash_reload", shell=True)
    time.sleep(1)

def remove_driver():
    print("Removing Driver...")
    subprocess.run("sudo rmmod litepcie", shell=True)

def remove_board_from_pcie_bus(device_ids):
    print("Removing Board from PCIe Bus...")
    for device_id in device_ids:
        if device_id:
            remove_pcie_device(device_id)

def rescan_bus():
    print("Rescanning PCIe Bus...")
    rescan_pcie_bus()

def load_driver():
    print("Loading Driver...")
    subprocess.run("sudo sh -c 'cd kernel && ./init.sh'", shell=True)

def get_device_ids():
    return [
        get_pcie_device_id("0x10ee", "0x7021"),
        get_pcie_device_id("0x10ee", "0x7022"),
        get_pcie_device_id("0x10ee", "0x7024"),
    ]

# Main ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="FPGA flashing over PCIe.")
    parser.add_argument('bitstream', help='Path to the bitstream file')
    parser.add_argument('-o', '--offset', type=lambda x: int(x, 0), default=0x00000000, help='Offset for flashing (default: 0x00000000)')
    args = parser.parse_args()

    # Flash.
    flash_bitstream(args.bitstream, args.offset)

    # PCIe Rescan and driver Remove/Reload.
    remove_driver()
    device_ids = get_device_ids()
    remove_board_from_pcie_bus(device_ids)
    rescan_bus()
    load_driver()

if __name__ == '__main__':
    main()