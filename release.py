#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse
import json
from pathlib import Path
import subprocess
import zipfile

from datetime import datetime, timezone

# Build Utilities ----------------------------------------------------------------------------------

GENERATED_TRACKED_FILES = [
    Path("litex_m2sdr", "software", "kernel", "csr.h"),
    Path("litex_m2sdr", "software", "kernel", "mem.h"),
    Path("litex_m2sdr", "software", "kernel", "soc.h"),
]

def run_command(command):
    """Execute a shell command and handle potential errors."""
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build error: {e}")
        exit(1)

def git_output(args):
    try:
        return subprocess.check_output(["git", *args], text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"

def snapshot_files(files):
    return {
        path: path.read_bytes() if path.exists() else None
        for path in files
    }

def restore_snapshot(snapshot):
    for path, contents in snapshot.items():
        if contents is None:
            if path.exists():
                path.unlink()
        else:
            path.write_bytes(contents)

def create_manifest(build_dir, build_name, date_str, config, command, release_metadata):
    """Create a small release manifest next to the generated SoC artifacts."""
    manifest_path = Path(build_dir, build_name, "release_manifest.json")
    manifest = {
        "project"       : "litex_m2sdr",
        "release_date"  : date_str,
        "generated_utc" : datetime.now(timezone.utc).isoformat(),
        "build_name"    : build_name,
        "build_command" : command,
        "configuration" : config,
        "git_revision"  : release_metadata["git_revision"],
        "git_dirty"     : release_metadata["git_dirty"],
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest_path

def create_archive(build_dir, build_name, date_str, config, command, release_metadata, nobuild=False):
    """Create a release zip archive for one build configuration."""
    gateware_dir = Path(build_dir, build_name, "gateware")
    archive_name = Path(build_dir, f"{build_name}_{date_str}.zip")
    manifest_path = create_manifest(build_dir, build_name, date_str, config, command, release_metadata)

    # If building is disabled, package the generated build directory contents
    # that already exist. This is useful to validate archive naming/layout
    # without running Vivado.
    if nobuild:
        files_to_archive = list(gateware_dir.glob("*")) if gateware_dir.exists() else []
    else:
        files_to_archive = [
            gateware_dir / f"{build_name}.bit",
            gateware_dir / f"{build_name}.bin",
            gateware_dir / f"{build_name}_fallback.bin",
            gateware_dir / f"{build_name}_operational.bin",
        ]

    files_to_archive += [
        Path("scripts", "csr.csv"),
        Path(build_dir, build_name, "csr.json"),
        manifest_path,
    ]

    missing_files = [f for f in files_to_archive if not f.exists()]
    if missing_files and not nobuild:
        missing = "\n".join(f"  - {f}" for f in missing_files)
        raise SystemExit(f"Missing release files for {build_name}:\n{missing}")

    existing_files = [str(f) for f in files_to_archive if f.exists()]
    if existing_files:
        with zipfile.ZipFile(archive_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for filename in existing_files:
                archive.write(filename, arcname=Path(filename).name)
        print(f"Created archive: {archive_name}")
    else:
        print(f"Warning: No files found to archive for {build_name}")

# Build Configurations -----------------------------------------------------------------------------

def build_configuration(config, nobuild=False):
    """Build a specific LiteX-M2SDR configuration."""
    command = ["./litex_m2sdr.py", f"--variant={config['variant']}"]
    if config["with_pcie"]:
        command += ["--with-pcie", f"--pcie-lanes={config['pcie_lanes']}"]
    if config["with_eth"]:
        command += ["--with-eth", f"--eth-sfp={config['eth_sfp']}"]
    if config.get("with_eth_ptp", False):
        command += ["--with-eth-ptp"]
    if config.get("with_eth_ptp_rfic_clock", False):
        command += ["--with-eth-ptp-rfic-clock"]
    if not nobuild:
        command += ["--build"]

    print(f"Building configuration: {' '.join(command)}")
    run_command(command)
    return command

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
    release_metadata = {
        "git_revision" : git_output(["rev-parse", "HEAD"]),
        "git_dirty"    : bool(git_output(["status", "--short", "--untracked-files=no"])),
    }

    # Define first-release configurations.
    configurations = [
        # Baseboard variant with Ethernet only.
        {
            "variant"                   : "baseboard",
            "with_pcie"                 : False,
            "pcie_lanes"                : 0,
            "with_eth"                  : True,
            "eth_sfp"                   : 0,
            "with_eth_ptp"              : False,
            "with_eth_ptp_rfic_clock"   : False,
            "build_name"                : "litex_m2sdr_baseboard_eth",
        },

        # Baseboard variant with Ethernet PTP and PTP-disciplined RFIC clock path.
        {
            "variant"                   : "baseboard",
            "with_pcie"                 : False,
            "pcie_lanes"                : 0,
            "with_eth"                  : True,
            "eth_sfp"                   : 0,
            "with_eth_ptp"              : True,
            "with_eth_ptp_rfic_clock"   : True,
            "build_name"                : "litex_m2sdr_baseboard_eth_ptp_rfic_clock",
        },

        # Baseboard variant with PCIe Gen2-X1 and Ethernet.
        {
            "variant"                   : "baseboard",
            "with_pcie"                 : True,
            "pcie_lanes"                : 1,
            "with_eth"                  : True,
            "eth_sfp"                   : 0,
            "with_eth_ptp"              : False,
            "with_eth_ptp_rfic_clock"   : False,
            "build_name"                : "litex_m2sdr_baseboard_pcie_x1_eth",
        },

        # M2 variant with PCIe Gen2-X1.
        {
            "variant"                   : "m2",
            "with_pcie"                 : True,
            "pcie_lanes"                : 1,
            "with_eth"                  : False,
            "eth_sfp"                   : 0,
            "with_eth_ptp"              : False,
            "with_eth_ptp_rfic_clock"   : False,
            "build_name"                : "litex_m2sdr_m2_pcie_x1",
        },

        # M2 variant with PCIe Gen2-X2.
        {
            "variant"                   : "m2",
            "with_pcie"                 : True,
            "pcie_lanes"                : 2,
            "with_eth"                  : False,
            "eth_sfp"                   : 0,
            "with_eth_ptp"              : False,
            "with_eth_ptp_rfic_clock"   : False,
            "build_name"                : "litex_m2sdr_m2_pcie_x2",
        }
    ]

    # Build each configuration. Preserve generated tracked headers so running
    # the release flow does not leave the source tree dirty.
    generated_snapshot = snapshot_files(GENERATED_TRACKED_FILES)
    try:
        for config in configurations:
            command = build_configuration(config, nobuild=args.nobuild)
            # Create archive for this build and move it to build_dir.
            create_archive(build_dir, config["build_name"], date_str, config, command, release_metadata, args.nobuild)
    finally:
        restore_snapshot(generated_snapshot)

    print(f"All builds completed successfully. Archives created with date: {date_str}")

if __name__ == "__main__":
    main()
