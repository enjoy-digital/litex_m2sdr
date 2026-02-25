#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse
from pathlib import Path
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

def create_archive(build_dir, build_name, date_str, nobuild=False):
    """Create a zip archive of .bit and .bin files for a specific build and move it to build_dir."""
    gateware_dir = Path(build_dir, build_name, "gateware")
    archive_name = Path(build_dir, f"{build_name}_{date_str}.zip")

    # If we prevent building - save the source files at least
    if nobuild:
        files_to_archive = list(gateware_dir.glob("*"))
    else:
        # Files to include in the archive.
        files_to_archive = [
            gateware_dir / f"{build_name}.bit",
            gateware_dir / f"{build_name}.bin",
            gateware_dir / f"{build_name}_fallback.bin",
            gateware_dir / f"{build_name}_operational.bin",
        ]

    # Always record csr.csv/csr.json along with the images
    files_to_archive.append(Path("test", "csr.csv"))
    files_to_archive.append(Path(build_dir, build_name, "csr.json"))

    # Check which files exist and create archive.
    existing_files = [str(f) for f in files_to_archive if f.exists()]
    if existing_files:
        command = f"zip -j {archive_name} {' '.join(existing_files)}"
        run_command(command)
        print(f"Created archive: {archive_name}")
    else:
        print(f"Warning: No files found to archive for {build_name}")

# Build Configurations -----------------------------------------------------------------------------

def build_configuration(variant, with_pcie=False, pcie_lanes=1, with_eth=False, nobuild=False):
    """Build a specific LiteX-M2SDR configuration."""
    command = "./litex_m2sdr.py"
    command += f" --variant={variant}"
    if with_pcie:
        command += f" --with-pcie --pcie-lanes={pcie_lanes}"
    if with_eth:
        command += " --with-eth"
    if not nobuild:
        command += " --build"

    print(f"Building configuration: {command}")
    run_command(command)

# Main ---------------------------------------------------------------------------------------------

def main():
    """Main function to build all configurations and create archives."""

    parser = argparse.ArgumentParser(description="Build all LiteX-M2SDR configurations")
    parser.add_argument("--nobuild", action="store_true", help="Disable FPGA bitstream build")
    args = parser.parse_args()

    # Get current date for archive naming.
    date_str  = datetime.now().strftime("%Y_%m_%d")
    build_dir = "build"

    # Ensure build directory exists.
    os.makedirs(build_dir, exist_ok=True)

    # Define configurations.
    configurations = [
        # Baseboard variant with Ethernet only.
        {
            "variant"    : "baseboard",
            "with_pcie"  : False,
            "pcie_lanes" : 0,
            "with_eth"   : True,
            "build_name" : "litex_m2sdr_baseboard_eth"
        },

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
            with_eth   = config["with_eth"],
            nobuild    = args.nobuild
        )
        # Create archive for this build and move it to build_dir.
        create_archive(build_dir, config["build_name"], date_str, args.nobuild)

    print(f"All builds completed successfully. Archives created with date: {date_str}")

if __name__ == "__main__":
    main()