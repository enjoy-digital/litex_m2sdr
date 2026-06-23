#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import subprocess
import sys
from pathlib import Path

from fake_liteeth import FakeLiteEthTarget


def run_checked(cmd, env=None, expect=None):
    print("+", " ".join(str(part) for part in cmd), flush=True)
    completed = subprocess.run(
        [str(part) for part in cmd],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    print(completed.stdout, end="")
    if completed.returncode != 0:
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {cmd}")
    if expect and expect not in completed.stdout:
        raise RuntimeError(f"expected output not found: {expect!r}")
    return completed.stdout


def soapy_env(plugin_dir):
    env = os.environ.copy()
    plugin_dir = str(Path(plugin_dir).resolve())
    env["SOAPY_SDR_ROOT"] = str(Path(plugin_dir, ".soapysdr-empty-root"))
    env["SOAPY_SDR_PLUGIN_PATH"] = plugin_dir
    return env


def main():
    parser = argparse.ArgumentParser(description="Exercise libm2sdr against a fake LiteEth/Etherbone target.")
    parser.add_argument("--m2sdr-util", required=True, help="Path to the m2sdr_util executable.")
    parser.add_argument("--soapy-util", help="Path to SoapySDRUtil for optional plugin probing.")
    parser.add_argument("--soapy-plugin-dir", help="Directory containing the built SoapyLiteXM2SDR module.")
    args = parser.parse_args()

    with FakeLiteEthTarget() as target:
        dev_id = f"eth:{target.host}:{target.port}"
        util = Path(args.m2sdr_util)

        run_checked([util, "--device", dev_id, "reg-read", "0x4"], expect="Reg 0x00000004")
        run_checked([util, "--device", dev_id, "reg-write", "0x4", "0x13579bdf"], expect="Wrote 0x13579bdf")
        run_checked([util, "--device", dev_id, "reg-read", "0x4"], expect="0x13579bdf")
        run_checked([util, "--device", dev_id, "scratch-test"], expect="0xdeadbeef")
        run_checked([util, "--device", dev_id, "info"], expect="LiteX-M2SDR Fake Ethernet Target")

        if args.soapy_util and args.soapy_plugin_dir:
            probe = (
                f"driver=LiteXM2SDR,eth_ip={target.host},eth_port={target.port},"
                "bypass_init=1,eth_discovery=0"
            )
            output = run_checked(
                [args.soapy_util, f"--probe={probe}"],
                env=soapy_env(args.soapy_plugin_dir),
                expect="LiteX-M2SDR",
            )
            if "driver=LiteXM2SDR" not in output and "Driver Key: LiteXM2SDR" not in output:
                raise RuntimeError("SoapySDR probe did not report the LiteXM2SDR driver")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
