#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import shutil
import subprocess

from datetime import datetime

# Build Utilities ----------------------------------------------------------------------------------

def run_command(command):
    """Execute a shell command and handle potential errors."""
    try:
        subprocess.run(command, shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build error: {e}")
        exit(1)

def create_archive(build_dir, build_name, date_str):
    """Create a zip archive of .bit and .bin files for a specific build and move it to build_dir."""
    gateware_dir = os.path.join(build_dir, build_name, "gateware")
    archive_name = f"{build_name}_{date_str}.zip"
    archive_path_temp = os.path.join(gateware_dir, archive_name)
    archive_path_final = os.path.join(build_dir, archive_name)

    # Files to include in the archive.
    files_to_archive = [
        f"{build_name}.bit",
        f"{build_name}.bin",
        f"{build_name}_fallback.bin",
        f"{build_name}_operational.bin"
    ]

    # Check which files exist and create archive.
    existing_files = [f for f in files_to_archive if os.path.exists(os.path.join(gateware_dir, f))]
    if existing_files:
        command = f"cd {gateware_dir} && zip {archive_name} {' '.join(existing_files)}"
        run_command(command)
        # Move the archive to the build directory
        shutil.move(archive_path_temp, archive_path_final)
        print(f"Created and moved archive to: {archive_path_final}")
    else:
        print(f"Warning: No files found to archive for {build_name}")

# Build Configurations -----------------------------------------------------------------------------

def build_configuration(variant, with_pcie=False, pcie_lanes=1, with_eth=False):
    """Build a specific LiteX-M2SDR configuration."""
    command = "./litex_m2sdr.py"
    command += f" --variant={variant}"
    if with_pcie:
        command += f" --with-pcie --pcie-lanes={pcie_lanes}"
    if with_eth:
        command += " --with-eth"
    command += " --build"

    print(f"Building configuration: {command}")
    run_command(command)

# Main ---------------------------------------------------------------------------------------------

def main():
    """Main function to build all configurations and create archives."""

    # Get current date for archive naming.
    date_str  = datetime.now().strftime("%Y_%m_%d")
    build_dir = "build"

    # Ensure build directory exists.
    os.makedirs(build_dir, exist_ok=True)

    # Define configurations.
    configurations = [
        # Baseboard variant with PCIe Gen2-X1 and Ethernet.
        {
            "variant"    : "baseboard",
            "with_pcie"  : True,
            "pcie_lanes" : 1,
            "with_eth"   : True,
            "build_name" : "litex_m2sdr_baseboard_pcie_x1_eth"
        },

        # M2 variant with PCIe Gen2-X1.
        {
            "variant"    : "m2",
            "with_pcie"  : True,
            "pcie_lanes" : 1,
            "with_eth"   : False,
            "build_name" : "litex_m2sdr_m2_pcie_x1"
        },

        # M2 variant with PCIe Gen2-X2.
        {
            "variant"    : "m2",
            "with_pcie"  : True,
            "pcie_lanes" : 2,
            "with_eth"   : False,
            "build_name" : "litex_m2sdr_m2_pcie_x2"
        }
    ]

    # Build each configuration.
    for config in configurations:
        build_configuration(
            variant    = config["variant"],
            with_pcie  = config["with_pcie"],
            pcie_lanes = config["pcie_lanes"],
            with_eth   = config["with_eth"]
        )
        # Create archive for this build and move it to build_dir.
        create_archive(build_dir, config["build_name"], date_str)

    print(f"All builds completed successfully. Archives created with date: {date_str}")

if __name__ == "__main__":
    main()