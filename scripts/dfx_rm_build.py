#!/usr/bin/env python3
"""Build a partial bitstream for a new DFX RM without rebuilding the static design.

Takes the build directory of a full ``--with-dfx`` image and one RM (Verilog/SystemVerilog,
top module ``m2sdr_dfx_datapath`` with a ``DATA_WIDTH`` parameter). Only the RM is
synthesized and placed/routed, inside the locked ``*_static_routed.dcp`` of that build.
The partial is accepted only if the Vivado version matches the image, timing is met and
``pr_verify -full_check`` proves the static part identical to the full build.

    source /opt/Xilinx/Vivado/2024.2/settings64.sh
    ./scripts/dfx_rm_build.py build/litex_m2sdr_m2_pcie_x4_rfic_oversampling_dfx \\
        --name mydsp my_dsp.v
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from litex_m2sdr.gateware.dfx import dfx_rm_tcl


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def image_contract(build_dir):
    """What the static design fixes: name, part, datapath width, Vivado version."""
    build_name = os.path.basename(os.path.normpath(build_dir))
    with open(os.path.join(build_dir, "build_info.json")) as f:
        info = json.load(f)
    argv = info["command"]["argv"]
    if "--with-dfx" not in argv:
        raise SystemExit(f"{build_dir} is not a --with-dfx build")
    gateware = os.path.abspath(os.path.join(build_dir, "gateware"))
    for suffix in ("static_routed.dcp", "passthrough_routed.dcp"):
        path = os.path.join(gateware, f"{build_name}_{suffix}")
        if not os.path.exists(path):
            raise SystemExit(f"missing {path}: rebuild the full DFX image first")
    with open(os.path.join(gateware, f"{build_name}.bit"), "rb") as f:
        header = f.read(256).decode("latin-1")
    version = re.search(r"Version=([0-9.]+)", header)
    return {
        "build_name": build_name,
        "gateware":   gateware,
        "part":       info["platform"]["device"],
        # Same rule as litex_m2sdr.py: the oversampling datapath is 128-bit.
        "data_width": 128 if "--with-rfic-oversampling" in argv else 64,
        "vivado":     version.group(1) if version else None,
    }


def vivado_version():
    try:
        out = subprocess.run(["vivado", "-version"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit("vivado not found: source the Vivado settings64.sh used for the image")
    match = re.search(r"v(\d+\.\d+(?:\.\d+)?)", out)
    return match.group(1) if match else None


def accepted(log, timing_report):
    """Vivado ran the gates; double-check their outcome in the outputs."""
    problems = []
    if "are compatible" not in log:
        problems.append("pr_verify did not report the configurations compatible")
    if not os.path.exists(timing_report) or \
            "All user specified timing constraints are met." not in open(timing_report).read():
        problems.append("timing constraints are not all met")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", help="build directory of the full --with-dfx image")
    parser.add_argument("--name", required=True, help="RM name, used in output files ([a-z][a-z0-9_]*)")
    parser.add_argument("sources", nargs="+", help="RM Verilog/SystemVerilog sources")
    args = parser.parse_args(argv)

    image = image_contract(args.build_dir)
    if args.name in ("passthrough", "static"):
        raise SystemExit(f"--name {args.name} would overwrite an output of the full build")
    tool = vivado_version()
    if tool != image["vivado"]:
        raise SystemExit(f"image was built with Vivado {image['vivado']}, this is {tool}")

    build_name, gateware = image["build_name"], image["gateware"]
    rm = f"{build_name}_{args.name}"
    tcl = os.path.join(gateware, f"{rm}_rm.tcl")
    with open(tcl, "w") as f:
        f.write("\n".join(dfx_rm_tcl(build_name, args.name, args.sources, image["part"], image["data_width"])) + "\n")
    partial = os.path.join(gateware, f"{rm}_partial.bit")
    for path in (partial, partial[:-4] + ".bin"):
        if os.path.exists(path):
            os.remove(path)

    print(f"Implementing RM {args.name} ({image['data_width']}-bit) in locked static {build_name}...")
    log_path = os.path.join(gateware, f"{rm}_rm.log")
    result = subprocess.run(["vivado", "-mode", "batch", "-nojournal", "-log", log_path, "-source", tcl],
        cwd=gateware, stdout=subprocess.DEVNULL)
    log = open(log_path).read() if os.path.exists(log_path) else ""
    problems = [] if result.returncode == 0 else [f"Vivado failed (exit {result.returncode}), see {log_path}"]
    problems += accepted(log, os.path.join(gateware, f"{rm}_timing.rpt"))
    if problems or not os.path.exists(partial):
        for path in (partial, partial[:-4] + ".bin"):
            if os.path.exists(path):
                os.remove(path)
        raise SystemExit("REJECTED, no partial written:\n  " + "\n  ".join(problems or ["no partial bitstream"]))

    manifest = {
        "rm": args.name,
        "data_width": image["data_width"],
        "vivado": tool,
        "partial_bit": {"file": os.path.basename(partial), "sha256": sha256(partial)},
        # Load this partial only on the image whose full bitstream has this hash.
        "image_bit": {"file": f"{build_name}.bit", "sha256": sha256(os.path.join(gateware, f"{build_name}.bit"))},
        "static_routed_dcp_sha256": sha256(os.path.join(gateware, f"{build_name}_static_routed.dcp")),
        "sources": {os.path.abspath(s): sha256(s) for s in args.sources},
        "pr_verify": "compatible",
        "timing": "met",
    }
    with open(os.path.join(gateware, f"{rm}_partial.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"OK: {partial}")


if __name__ == "__main__":
    main()
