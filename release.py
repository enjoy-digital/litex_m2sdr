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

def parse_number(value):
    """Convert Vivado timing table values while preserving N/A entries."""
    return None if value == "N/A" else float(value)

def parse_timing_summary(report):
    """Extract the top-level Design Timing Summary row from a Vivado report."""
    lines = report.splitlines()
    fields = [
        "wns_ns",
        "tns_ns",
        "tns_failing_endpoints",
        "tns_total_endpoints",
        "whs_ns",
        "ths_ns",
        "ths_failing_endpoints",
        "ths_total_endpoints",
        "wpws_ns",
        "tpws_ns",
        "tpws_failing_endpoints",
        "tpws_total_endpoints",
    ]

    for index, line in enumerate(lines):
        if "| Design Timing Summary" not in line:
            continue
        for row_index in range(index, min(index + 30, len(lines))):
            if "WNS(ns)" not in lines[row_index]:
                continue
            if row_index + 2 >= len(lines):
                break
            values = lines[row_index + 2].split()
            if len(values) < len(fields):
                break
            summary = dict(zip(fields, values[:len(fields)]))
            for key in fields:
                if key.endswith("_endpoints"):
                    summary[key] = int(summary[key])
                else:
                    summary[key] = parse_number(summary[key])
            return summary

    raise SystemExit("Unable to parse Design Timing Summary from timing report.")

def check_timing(build_dir, build_name, allow_pcie_pulse_width_warning=False):
    """Reject bitstreams with setup/hold timing failures before packaging."""
    timing_report = Path(build_dir, build_name, "gateware", f"{build_name}_timing.rpt")
    if not timing_report.exists():
        raise SystemExit(f"Missing timing report for {build_name}: {timing_report}")

    report = timing_report.read_text(errors="replace")
    summary = parse_timing_summary(report)

    setup_ok = (
        summary["wns_ns"] is not None and summary["wns_ns"] >= 0 and
        summary["tns_ns"] == 0 and
        summary["tns_failing_endpoints"] == 0
    )
    hold_ok = (
        summary["whs_ns"] is not None and summary["whs_ns"] >= 0 and
        summary["ths_ns"] == 0 and
        summary["ths_failing_endpoints"] == 0
    )
    if not (setup_ok and hold_ok):
        raise SystemExit(f"Setup/hold timing failed for {build_name}: {timing_report}")

    pulse_width_ok = (
        (summary["wpws_ns"] is None or summary["wpws_ns"] >= 0) and
        (summary["tpws_ns"] is None or summary["tpws_ns"] >= 0) and
        summary["tpws_failing_endpoints"] == 0
    )
    allowed_pcie_pulse_width_warning = (
        # Vivado 2024.1 reports a single -42ps pulse-width endpoint on the
        # Xilinx 7-series PCIe IP generated clocks. Keep this explicit and
        # narrow so real setup/hold or broader pulse-width failures still stop.
        allow_pcie_pulse_width_warning and
        summary["tpws_failing_endpoints"] == 1 and
        summary["tpws_ns"] is not None and
        summary["tpws_ns"] >= -0.050
    )
    summary["pulse_width_warning_allowed"] = bool(allowed_pcie_pulse_width_warning)
    if not (pulse_width_ok or allowed_pcie_pulse_width_warning):
        raise SystemExit(f"Pulse-width timing failed for {build_name}: {timing_report}")

    if allowed_pcie_pulse_width_warning:
        print(f"Setup/hold timing clean with allowed PCIe pulse-width warning: {timing_report}")
    else:
        print(f"Timing clean: {timing_report}")
    return summary

def create_manifest(build_dir, build_name, date_str, config, command, release_metadata, timing_summary=None):
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
    if timing_summary is not None:
        manifest["timing_summary"] = timing_summary
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest_path

def create_archive(build_dir, build_name, date_str, config, command, release_metadata, timing_summary=None, nobuild=False):
    """Create a release zip archive for one build configuration."""
    gateware_dir = Path(build_dir, build_name, "gateware")
    archive_name = Path(build_dir, f"{build_name}_{date_str}.zip")
    manifest_path = create_manifest(build_dir, build_name, date_str, config, command, release_metadata, timing_summary)

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
            timing_summary = None
            if not args.nobuild:
                timing_summary = check_timing(
                    build_dir,
                    config["build_name"],
                    allow_pcie_pulse_width_warning=config["with_pcie"],
                )
            # Create archive for this build and move it to build_dir.
            create_archive(build_dir, config["build_name"], date_str, config, command, release_metadata, timing_summary, args.nobuild)
    finally:
        restore_snapshot(generated_snapshot)

    print(f"All builds completed successfully. Archives created with date: {date_str}")

if __name__ == "__main__":
    main()
