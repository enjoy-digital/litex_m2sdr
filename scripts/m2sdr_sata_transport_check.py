#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


class CheckError(RuntimeError):
    pass


def repo_root():
    return Path(__file__).resolve().parents[1]


def default_user_dir():
    return repo_root() / "litex_m2sdr" / "software" / "user"


def default_tool(name):
    return default_user_dir() / name


def render_command(cmd):
    return " ".join(shlex.quote(str(part)) for part in cmd)


def strip_ansi(text):
    return ANSI_RE.sub("", text)


def parse_u64(text):
    return int(text, 0)


def run_command(args, cmd, timeout=None):
    rendered = render_command(cmd)
    print(f"+ {rendered}")
    proc = subprocess.run(
        [str(part) for part in cmd],
        cwd=args.user_dir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout if timeout is not None else args.command_timeout,
    )
    if args.verbose and proc.stdout:
        print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    if args.verbose and proc.stderr:
        print(proc.stderr, end="" if proc.stderr.endswith("\n") else "\n", file=sys.stderr)
    if proc.returncode != 0:
        raise CheckError(
            f"{rendered} failed with exit code {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    if args.command_gap > 0:
        time.sleep(args.command_gap)
    return proc


def tool_cmd(args, tool, *extra):
    return [tool, "--device", args.device, *extra]


def sata_cmd(args, *extra):
    return tool_cmd(
        args,
        args.m2sdr_sata,
        "--timeout-ms", str(args.sata_timeout_ms),
        *extra,
    )


def require_in_output(output, needle, context):
    clean = strip_ansi(output)
    if needle not in clean:
        raise CheckError(f"{context}: expected to find {needle!r}\n{clean}")


def require_feature_enabled(output, feature):
    clean = strip_ansi(output)
    pattern = re.compile(rf"^\s*{re.escape(feature)}\s*:\s*Yes\s*$", re.MULTILINE)
    if not pattern.search(clean):
        raise CheckError(f"m2sdr_util info: expected {feature} feature to be enabled\n{clean}")


def check_info(args):
    proc = run_command(args, tool_cmd(args, args.m2sdr_util, "info"))
    clean = strip_ansi(proc.stdout)
    require_in_output(clean, "SATA Disk      : Present", "m2sdr_util info")
    require_in_output(clean, "SATA PHY       : Ready", "m2sdr_util info")
    if args.expect_transport:
        require_feature_enabled(clean, args.expect_transport)


def check_status(args):
    proc = run_command(args, sata_cmd(args, "status"))
    clean = strip_ansi(proc.stdout)
    for needle in (
        "PHY ready          yes",
        "TX ready           yes",
        "RX ready           yes",
        "CTRL ready         yes",
    ):
        require_in_output(clean, needle, "m2sdr_sata status")


def reset_streamers(args):
    run_command(args, sata_cmd(args, "stream-stop", "both"))


def write_and_verify_pattern(args):
    run_command(
        args,
        sata_cmd(
            args,
            "--pattern", args.pattern,
            "write-pattern", hex(args.source_sector), str(args.sectors),
        ),
    )
    run_command(
        args,
        sata_cmd(
            args,
            "--pattern", args.pattern,
            "verify-pattern", hex(args.source_sector), str(args.sectors),
        ),
    )


def read_region(args, sector, nsectors, path):
    run_command(args, sata_cmd(args, "read-file", hex(sector), str(nsectors), str(path)))


def copy_and_compare(args):
    reset_streamers(args)
    run_command(
        args,
        sata_cmd(
            args,
            "copy", hex(args.source_sector), hex(args.copy_sector), str(args.sectors),
        ),
        timeout=args.copy_command_timeout,
    )

    with tempfile.TemporaryDirectory(prefix="m2sdr-sata-check-") as tmpdir:
        src = Path(tmpdir) / "source.bin"
        dst = Path(tmpdir) / "copy.bin"
        read_region(args, args.source_sector, args.sectors, src)
        read_region(args, args.copy_sector, args.sectors, dst)

        src_data = src.read_bytes()
        dst_data = dst.read_bytes()
        if src_data != dst_data:
            for offset, (a, b) in enumerate(zip(src_data, dst_data)):
                if a != b:
                    break
            else:
                offset = min(len(src_data), len(dst_data))
            raise CheckError(
                "SATA copy byte comparison failed at byte "
                f"{offset}: source_len={len(src_data)} copy_len={len(dst_data)}"
            )
        print(f"Copy compare OK: {len(src_data)} bytes")


def configure_rfic(args):
    run_command(
        args,
        tool_cmd(
            args,
            args.m2sdr_rf,
            f"--channel-layout={args.channel_layout}",
            f"--sample-rate={args.rf_sample_rate}",
            f"--bandwidth={args.rf_bandwidth}",
            f"--rx-freq={args.rx_freq}",
            f"--tx-freq={args.tx_freq}",
            f"--rx-gain={args.rx_gain}",
            f"--tx-att={args.tx_att}",
        ),
        timeout=args.rf_timeout,
    )


def rfic_to_sata(args):
    configure_rfic(args)
    reset_streamers(args)
    run_command(
        args,
        sata_cmd(args, "record", hex(args.rfic_sector), str(args.rf_sectors)),
        timeout=args.rf_record_timeout,
    )

    with tempfile.TemporaryDirectory(prefix="m2sdr-sata-rfic-") as tmpdir:
        head = Path(tmpdir) / "rfic-head.bin"
        read_region(args, args.rfic_sector, 4, head)
        size = head.stat().st_size
        if size != 2048:
            raise CheckError(f"RFIC SATA readback size mismatch: expected 2048, got {size}")
        print("RFIC SATA readback OK: 2048 bytes")


def add_args(parser):
    parser.add_argument(
        "--device",
        default="eth:192.168.1.50:1234",
        help="M2SDR device id, for example eth:192.168.1.50:1234 or pcie:/dev/m2sdr0.",
    )
    parser.add_argument("--m2sdr-util", type=Path, default=default_tool("m2sdr_util"))
    parser.add_argument("--m2sdr-sata", type=Path, default=default_tool("m2sdr_sata"))
    parser.add_argument("--m2sdr-rf", type=Path, default=default_tool("m2sdr_rf"))
    parser.add_argument("--user-dir", type=Path, default=default_user_dir())
    parser.add_argument("--command-timeout", type=float, default=120.0)
    parser.add_argument("--command-gap", type=float, default=0.05)
    parser.add_argument("--sata-timeout-ms", type=int, default=30000)
    parser.add_argument("--copy-command-timeout", type=float, default=180.0)
    parser.add_argument("--source-sector", type=parse_u64, default=0x100000)
    parser.add_argument("--copy-sector", type=parse_u64, default=0x110000)
    parser.add_argument("--rfic-sector", type=parse_u64, default=0x120000)
    parser.add_argument("--sectors", type=int, default=256)
    parser.add_argument("--rf-sectors", type=int, default=8192)
    parser.add_argument("--pattern", default="counter", choices=("zero", "counter", "prbs"))
    parser.add_argument("--skip-rfic", action="store_true")
    parser.add_argument("--expect-transport", choices=("PCIe", "Ethernet"))
    parser.add_argument("--channel-layout", default="1t1r")
    parser.add_argument("--rf-sample-rate", default="30.72M")
    parser.add_argument("--rf-bandwidth", default="56M")
    parser.add_argument("--rx-freq", default="2400M")
    parser.add_argument("--tx-freq", default="2400M")
    parser.add_argument("--rx-gain", default="20")
    parser.add_argument("--tx-att", default="20")
    parser.add_argument("--rf-timeout", type=float, default=60.0)
    parser.add_argument("--rf-record-timeout", type=float, default=60.0)
    parser.add_argument("--verbose", action="store_true")


def main():
    parser = argparse.ArgumentParser(description="Validate M2SDR SATA access over a selected transport.")
    add_args(parser)
    args = parser.parse_args()

    if args.sectors <= 0:
        raise CheckError("--sectors must be greater than zero")
    if args.rf_sectors <= 0:
        raise CheckError("--rf-sectors must be greater than zero")

    for tool in (args.m2sdr_util, args.m2sdr_sata):
        if not tool.exists():
            raise CheckError(f"required tool not found: {tool}")
    if not args.skip_rfic and not args.m2sdr_rf.exists():
        raise CheckError(f"required tool not found: {args.m2sdr_rf}")

    print(f"Device: {args.device}")
    check_info(args)
    check_status(args)
    reset_streamers(args)
    write_and_verify_pattern(args)
    copy_and_compare(args)
    if not args.skip_rfic:
        rfic_to_sata(args)
    print("SATA transport check passed")


if __name__ == "__main__":
    try:
        main()
    except (CheckError, subprocess.TimeoutExpired) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
