#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import shlex
import shutil
import sys
import time
from pathlib import Path

# Constants ----------------------------------------------------------------------------------------

DEFAULT_CLOCK_NAME = "m2sdr"

# Error --------------------------------------------------------------------------------------------


class TimeSyncError(RuntimeError):
    pass

# Helpers ------------------------------------------------------------------------------------------


def ptp_sort_key(path):
    name = path.name
    if name.startswith("ptp"):
        try:
            return int(name[3:])
        except ValueError:
            pass
    return name


def read_text(path):
    try:
        return path.read_text(encoding="ascii").strip()
    except OSError:
        return None

# PHC Discovery ------------------------------------------------------------------------------------


def discover_phcs(sys_class_ptp, dev_root, clock_name=DEFAULT_CLOCK_NAME):
    sys_class_ptp = Path(sys_class_ptp)
    dev_root = Path(dev_root)
    phcs = []

    try:
        entries = sorted(sys_class_ptp.glob("ptp*"), key=ptp_sort_key)
    except OSError:
        return phcs

    for entry in entries:
        if read_text(entry / "clock_name") == clock_name:
            phcs.append(dev_root / entry.name)

    return phcs


def select_phc(args):
    if args.phc:
        return Path(args.phc)

    phcs = discover_phcs(args.sys_class_ptp, args.dev_root, args.clock_name)
    if not phcs:
        raise TimeSyncError(
            "no M2SDR PHC found; expected /sys/class/ptp/ptp*/clock_name "
            f"to contain '{args.clock_name}'"
        )
    if len(phcs) > 1:
        rendered = ", ".join(str(phc) for phc in phcs)
        raise TimeSyncError(
            f"multiple M2SDR PHCs found ({rendered}); pass --phc /dev/ptpN"
        )
    return phcs[0]


def wait_for_phc(args):
    deadline = None
    if not args.wait_forever:
        deadline = time.monotonic() + args.wait_timeout

    last_error = None
    while True:
        try:
            phc = select_phc(args)
            if phc.exists():
                return phc
            last_error = f"{phc} does not exist yet"
        except TimeSyncError as e:
            last_error = str(e)

        if deadline is not None and time.monotonic() >= deadline:
            raise TimeSyncError(last_error)

        if args.verbose:
            print(f"Waiting for M2SDR PHC: {last_error}", file=sys.stderr)
        time.sleep(args.wait_interval)

# phc2sys Command ----------------------------------------------------------------------------------


def resolve_phc2sys(path):
    resolved = shutil.which(path)
    if resolved:
        return resolved

    candidate = Path(path)
    if candidate.exists():
        return str(candidate)

    raise TimeSyncError(f"phc2sys executable not found: {path}")


def build_phc2sys_command(args, phc):
    cmd = [
        resolve_phc2sys(args.phc2sys),
        "-s", args.source,
        "-c", str(phc),
        "-O", str(args.offset),
        "-R", str(args.rate),
        "-N", str(args.readings),
    ]

    if args.first_step_threshold is not None:
        cmd += ["-F", str(args.first_step_threshold)]
    if args.step_threshold is not None:
        cmd += ["-S", str(args.step_threshold)]
    if args.summary_updates:
        cmd += ["-u", str(args.summary_updates)]
    if args.domain is not None:
        cmd += ["-n", str(args.domain)]
    if args.wait_ptp4l:
        cmd += ["-w"]
    if args.stdout:
        cmd.append("-m")
    if args.no_syslog:
        cmd.append("-q")

    return cmd


def render_command(cmd):
    return " ".join(shlex.quote(part) for part in cmd)

# Arguments ----------------------------------------------------------------------------------------


def add_args(parser):
    parser.add_argument(
        "--phc",
        help="PTP hardware clock to discipline. If omitted, auto-detect clock_name=m2sdr.",
    )
    parser.add_argument(
        "--clock-name",
        default=DEFAULT_CLOCK_NAME,
        help="PHC clock_name to auto-detect.",
    )
    parser.add_argument(
        "--sys-class-ptp",
        default="/sys/class/ptp",
        help="sysfs PTP class directory.",
    )
    parser.add_argument(
        "--dev-root",
        default="/dev",
        help="device node root used with auto-detected ptpN names.",
    )
    parser.add_argument(
        "--phc2sys",
        default="phc2sys",
        help="phc2sys executable path/name.",
    )
    parser.add_argument(
        "--source",
        default="CLOCK_REALTIME",
        help="source clock for phc2sys. Default makes the board follow host time.",
    )
    parser.add_argument(
        "--offset",
        default=0.0,
        type=float,
        help="sink-source offset in seconds.",
    )
    parser.add_argument(
        "--rate",
        default=8.0,
        type=float,
        help="phc2sys update rate in Hz.",
    )
    parser.add_argument(
        "--readings",
        default=5,
        type=int,
        help="number of source reads per update.",
    )
    parser.add_argument(
        "--first-step-threshold",
        type=float,
        help="phc2sys -F threshold in seconds for startup stepping.",
    )
    parser.add_argument(
        "--step-threshold",
        type=float,
        help="phc2sys -S threshold in seconds for runtime stepping.",
    )
    parser.add_argument(
        "--summary-updates",
        default=0,
        type=int,
        help="print phc2sys summary every N updates.",
    )
    parser.add_argument(
        "--domain",
        type=int,
        help="PTP domain number passed to phc2sys.",
    )
    parser.add_argument(
        "--wait-ptp4l",
        action="store_true",
        help="pass -w to phc2sys to wait for ptp4l and use its UTC offset.",
    )
    parser.add_argument(
        "--wait-timeout",
        default=60.0,
        type=float,
        help="seconds to wait for the M2SDR PHC at startup.",
    )
    parser.add_argument(
        "--wait-forever",
        action="store_true",
        help="wait indefinitely for the M2SDR PHC.",
    )
    parser.add_argument(
        "--wait-interval",
        default=1.0,
        type=float,
        help="seconds between PHC discovery attempts.",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="pass -m so phc2sys logs to stdout.",
    )
    parser.add_argument(
        "--no-syslog",
        action="store_true",
        help="pass -q so phc2sys does not log to syslog.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the phc2sys command instead of executing it.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="print PHC discovery progress while waiting.",
    )


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Synchronize the PCIe M2SDR board clock to host time by running "
            "phc2sys with the M2SDR PHC as the sink."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_args(parser)
    return parser.parse_args(argv)

# Main ---------------------------------------------------------------------------------------------


def main(argv=None, exec_func=os.execv):
    args = parse_args(argv)

    try:
        phc = wait_for_phc(args)
        cmd = build_phc2sys_command(args, phc)
    except TimeSyncError as e:
        print(f"m2sdr_pcie_time_sync: {e}", file=sys.stderr)
        return 1

    if args.dry_run:
        print(render_command(cmd))
        return 0

    print(f"Starting {render_command(cmd)}", flush=True)
    exec_func(cmd[0], cmd)
    return 127


if __name__ == "__main__":
    sys.exit(main())
