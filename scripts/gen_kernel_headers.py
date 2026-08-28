#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Generate the tracked kernel CSR headers from every supported build configuration.

litex_m2sdr/software/kernel/{csr,soc,mem}.h describe the CSR interface for *all* supported
images, not for one of them: every configuration in litex_m2sdr/build_configs.py places every
CSR it shares with the others at the same address, so the union of their CSR maps is a valid
description of each one. Host software built once therefore works with any flashed image, and
which image is flashed can no longer silently mean a different register.

Usage:
    ./scripts/gen_kernel_headers.py            # regenerate the tracked headers
    ./scripts/gen_kernel_headers.py --check    # fail if they are out of date (CI)
"""

import argparse
import contextlib
import difflib
import logging
import filecmp
import importlib.util
import io
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from litex_m2sdr.build_configs import (
    PER_IMAGE_CONSTANTS,
    REFERENCE_CONFIG,
    SUPPORTED_CONFIGS,
    UNION_IDENTIFIER,
    config_kwargs,
    missing_requirements,
)

HEADERS     = ("csr.h", "soc.h", "mem.h")
KERNEL_DIR  = ROOT / "litex_m2sdr" / "software" / "kernel"


def load_soc_module():
    """Import litex_m2sdr.py (a script, not a module) as litex_m2sdr_soc."""
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", ROOT / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def parse_csr_csv(path):
    """CSR map of one configuration, as {"bases"|"regs"|"constants"|"mems": {name: value}}."""
    out = {"bases": {}, "regs": {}, "constants": {}, "mems": {}}
    for line in Path(path).read_text().splitlines():
        fields = [f.strip() for f in line.split(",")]
        if   fields[0] == "csr_base":      out["bases"][fields[1]]     = int(fields[2], 0)
        elif fields[0] == "csr_register":  out["regs"][fields[1]]      = (int(fields[2], 0), int(fields[3]), fields[4])
        elif fields[0] == "constant":      out["constants"][fields[1]] = fields[2]
        elif fields[0] == "memory_region": out["mems"][fields[1]]      = (int(fields[2], 0), int(fields[3]), fields[4])
    return out


def elaborate(soc_module, name, config, output_dir):
    """Elaborate one configuration; return (soc, csr map). No FPGA toolchain needed."""
    from litex.soc.integration.builder import Builder

    build_dir = Path(output_dir, name)
    csr_csv   = Path(build_dir, "csr.csv")
    # LiteX prints and logs a full SoC summary per configuration; keep it out of the report.
    logging.disable(logging.INFO)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            soc = soc_module.BaseSoC(**config_kwargs(config))
            Builder(soc, output_dir=str(build_dir), compile_software=False,
                csr_csv=str(csr_csv)).build(run=False)
    finally:
        logging.disable(logging.NOTSET)
    return soc, parse_csr_csv(csr_csv)


def elaborate_supported(soc_module, output_dir, configs=None):
    """Elaborate every supported configuration whose requirements are available.

    Returns (elaborated, skipped): {name: (soc, csr map)} and {name: [missing packages]}.
    """
    elaborated = {}
    skipped    = {}
    for name, config in (configs or SUPPORTED_CONFIGS).items():
        missing = missing_requirements(config)
        if missing:
            skipped[name] = missing
            continue
        elaborated[name] = elaborate(soc_module, name, config, output_dir)
    return elaborated, skipped


def region_registers(csr_map, region):
    """Registers of one region, in address order, as [(suffix, size, access)]."""
    prefix = f"{region}_"
    regs   = [(n, v) for n, v in csr_map["regs"].items() if n.startswith(prefix)]
    regs.sort(key=lambda item: item[1][0])
    return [(n[len(prefix):], v[1], v[2]) for n, v in regs]


def check_agreement(elaborated):
    """Every CSR shared by two configurations must live at the same address in both.

    Returns the list of disagreements, each a human-readable line.
    """
    errors = []

    def compare(key, label, ignore=()):
        names = sorted({n for _, csr_map in elaborated.values() for n in csr_map[key]})
        for name in names:
            if name in ignore:
                continue
            values = {}
            for config, (_, csr_map) in elaborated.items():
                if name in csr_map[key]:
                    values.setdefault(repr(csr_map[key][name]), []).append(config)
            if len(values) > 1:
                detail = "; ".join(f"{v} in {', '.join(c)}" for v, c in values.items())
                errors.append(f"{label} {name} differs between configurations: {detail}")

    compare("bases",     "CSR region")
    compare("regs",      "CSR register")
    compare("constants", "Constant", ignore=PER_IMAGE_CONSTANTS)
    compare("mems",      "Memory region")

    # Optional CSR blocks have to be appended to the end of their region: an optional register
    # declared before an unconditional one shifts it, which "CSR register ... differs" above
    # reports but does not explain. Report the region-level cause too.
    regions = sorted({r for _, csr_map in elaborated.values() for r in csr_map["bases"]})
    for region in regions:
        layouts = {c: region_registers(m, region) for c, (_, m) in elaborated.items()
                   if region in m["bases"]}
        longest = max(layouts, key=lambda c: len(layouts[c]))
        for config, layout in layouts.items():
            if layout != layouts[longest][:len(layout)]:
                errors.append(
                    f"CSR region {region} in {config} is not a prefix of the longest layout "
                    f"({longest}): optional CSRs must be declared after the unconditional ones "
                    f"so they only append to the region.")
    return errors


def register_count(region):
    """Number of registers in a CSR region (0 for a CSR-mapped memory)."""
    return len(region.obj) if isinstance(region.obj, (list, tuple)) else 0


def build_union(elaborated):
    """Merge the elaborated configurations into one SoC-like CSR description."""
    csr_regions, mem_regions, constants = {}, {}, {}

    for config, (soc, csr_map) in elaborated.items():
        for region, obj in soc.csr_regions.items():
            # Keep the longest layout of each region; check_agreement() has already established
            # that the shorter ones are prefixes of it. A CSR-mapped memory (identifier_mem)
            # carries a Memory rather than a register list and is identical in every build.
            known = csr_regions.get(region)
            if known is None or register_count(obj) > register_count(known):
                csr_regions[region] = obj
        mem_regions.update(soc.mem_regions)
        # soc.constants is keyed in upper case; PER_IMAGE_CONSTANTS follows the csr.csv spelling.
        for name, value in soc.constants.items():
            if name.lower() not in PER_IMAGE_CONSTANTS:
                constants[name] = value

    # The per-image constants describe one image rather than the CSR interface; take them from the
    # reference configuration, and say so in the identifier instead of stamping a build time.
    reference = elaborated[REFERENCE_CONFIG][0]
    for name, value in reference.constants.items():
        if name.lower() in PER_IMAGE_CONSTANTS:
            constants[name] = value
    constants["CONFIG_IDENTIFIER"] = UNION_IDENTIFIER

    # csr_regions is emitted in insertion order; sort by address so the header reads like a map.
    csr_regions = dict(sorted(csr_regions.items(), key=lambda item: item[1].origin))
    return SimpleNamespace(csr_regions=csr_regions, mem_regions=mem_regions, constants=constants)


BANNER = re.compile(r"^// Auto-generated by LiteX .*$", re.MULTILINE)

BANNER_REPLACEMENT = (
    "// Auto-generated by scripts/gen_kernel_headers.py: the CSR map shared by every supported\n"
    "// build configuration (see litex_m2sdr/build_configs.py). Do not edit -- re-run the script."
)


def write_headers(union, dst):
    """Write the union headers, with a build-independent banner.

    LiteX stamps its banner with the generation time, which would make these tracked files
    change on every regeneration and hide the changes that matter in the diff.
    """
    from litex_m2sdr.software import generate_litepcie_software_headers

    dst = Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    generate_litepcie_software_headers(union, str(dst))
    for header in HEADERS:
        path = dst / header
        path.write_text(BANNER.sub(BANNER_REPLACEMENT, path.read_text()))


def carry_over_missing_regions(staging_csr, tracked_csr, pinned_regions, provided_regions):
    """Re-emit tracked defines for regions no elaborated configuration provides.

    A configuration whose requirements are missing here still has a pinned csr_map location and a
    section in the tracked header (the PTM requester is built by litex_wr_nic, which is not a
    dependency of this repository). Regenerating without it would drop those defines and silently
    compile the feature out of software that is otherwise unaffected, so they are carried over
    verbatim -- the location is pinned, so the addresses stay valid -- and reported to the caller.

    Returns the names of the regions carried over.
    """
    if not tracked_csr.exists():
        return []

    candidates = sorted(set(pinned_regions) - set(provided_regions), key=len, reverse=True)
    carried    = {}
    for line in tracked_csr.read_text().splitlines():
        if not line.startswith("#define CSR_"):
            continue
        for region in candidates:
            if line.startswith(f"#define CSR_{region.upper()}_"):
                carried.setdefault(region, []).append(line)
                break
    if not carried:
        return []

    lines = [""]
    for region, defines in carried.items():
        lines.append(f"/* {region.upper()} Registers/Fields (carried over) */")
        lines += defines
        lines.append("")
    text = staging_csr.read_text()
    marker = "#endif /* ! __GENERATED_CSR_H */"
    assert text.count(marker) == 1, "unexpected csr.h layout"
    staging_csr.write_text(text.replace(marker, "\n".join(lines) + marker))
    return list(carried)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
        help="Do not write; fail if the tracked headers are out of date.")
    args = parser.parse_args()

    soc_module = load_soc_module()
    with tempfile.TemporaryDirectory() as tmp:
        elaborated, skipped = elaborate_supported(soc_module, tmp)
        for name, missing in skipped.items():
            print(f"skipped {name}: requires {', '.join(missing)}")
        print(f"elaborated {len(elaborated)} configurations: {', '.join(elaborated)}")

        errors = check_agreement(elaborated)
        if errors:
            print("\nThe supported configurations do not agree on the CSR map:", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
            return 1

        staging = Path(tmp, "headers")
        union   = build_union(elaborated)
        write_headers(union, staging)

        if skipped:
            carried = carry_over_missing_regions(staging / "csr.h", KERNEL_DIR / "csr.h",
                soc_module.BaseSoC.csr_map, union.csr_regions)
            if carried:
                print(f"carried over from the previous header: {', '.join(carried)} "
                      f"(provided by a configuration that could not be elaborated here)")

        stale = [h for h in HEADERS
                 if not (KERNEL_DIR / h).exists()
                 or not filecmp.cmp(staging / h, KERNEL_DIR / h, shallow=False)]
        if not stale:
            print(f"{KERNEL_DIR} is up to date.")
            return 0

        if args.check:
            print(f"\n{', '.join(stale)} out of date; run ./scripts/gen_kernel_headers.py",
                file=sys.stderr)
            for header in stale:
                current = (KERNEL_DIR / header).read_text().splitlines(keepends=True) \
                    if (KERNEL_DIR / header).exists() else []
                diff = difflib.unified_diff(current,
                    (staging / header).read_text().splitlines(keepends=True),
                    fromfile=f"tracked/{header}", tofile=f"generated/{header}")
                sys.stderr.writelines(diff)
            return 1

        for header in stale:
            shutil.copyfile(staging / header, KERNEL_DIR / header)
        print(f"updated {', '.join(stale)} in {KERNEL_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
