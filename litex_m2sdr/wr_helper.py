#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import re
import subprocess
from pathlib import Path


def resolve_wr_paths(root_dir, wr_nic_dir=None, wr_firmware=None):
    # Auto-discover local/sibling litex_wr_nic checkout when not explicitly provided.
    if wr_nic_dir is None:
        parent_dir = os.path.dirname(root_dir)
        wr_nic_candidates = [
            os.path.join(root_dir,   "litex_wr_nic"),
            os.path.join(parent_dir, "litex_wr_nic"),
            os.path.join(root_dir,   "litex_wr_nic", "litex_wr_nic"),
            os.path.join(parent_dir, "litex_wr_nic", "litex_wr_nic"),
        ]
        for candidate in wr_nic_candidates:
            if os.path.isfile(os.path.join(candidate, "firmware", "build.py")):
                wr_nic_dir = candidate
                break

    if wr_firmware is None and wr_nic_dir:
        candidate = os.path.join(wr_nic_dir, "firmware", "spec_a7_wrc.bram")
        if os.path.isfile(candidate):
            wr_firmware = candidate

    return wr_nic_dir, wr_firmware


def build_wr_firmware(wr_nic_dir, wr_firmware, wr_firmware_target):
    if wr_nic_dir is None and wr_firmware is None:
        raise ValueError("White Rabbit build requested but no WR firmware path found. Use --wr-firmware or --wr-nic-dir.")

    firmware_dir = os.path.dirname(os.path.abspath(wr_firmware)) if wr_firmware else os.path.join(wr_nic_dir, "firmware")
    build_script = os.path.join(firmware_dir, "build.py")
    print(f"Building White Rabbit firmware in {firmware_dir}...")
    r = subprocess.run([build_script, "--target", wr_firmware_target], cwd=firmware_dir)
    if r.returncode != 0:
        raise RuntimeError("White Rabbit Firmware build failed.")


def _get_expected_wr_cores_sha(wr_nic_dir):
    if wr_nic_dir is None:
        return None

    wr_common = os.path.join(wr_nic_dir, "gateware", "wr_common.py")
    if not os.path.isfile(wr_common):
        return None

    wr_common_txt = Path(wr_common).read_text()
    m = re.search(r'WR_CORES_SHA1\s*=\s*"([0-9a-fA-F]+)"', wr_common_txt)
    return m.group(1).lower() if m else None


def preflight_wr_cores(root_dir, wr_nic_dir):
    wr_cores_dir = os.path.join(root_dir, "wr-cores")
    wr_subsystem_vhd = os.path.join(wr_cores_dir, "modules", "wrc_core", "xwr_subsystem.vhd")
    wr_patch_file = os.path.join(root_dir, "patches", "wr-cores", "0001-xwr_subsystem-mux-class.patch")

    if not os.path.isdir(wr_cores_dir):
        return

    if not os.path.isfile(wr_subsystem_vhd):
        msg = "Incompatible local 'wr-cores' tree detected (missing modules/wrc_core/xwr_subsystem.vhd). "
        msg += "This usually means an older WR-cores checkout is present in the litex_m2sdr directory. "
        msg += "Rename/remove it (for example: 'mv wr-cores wr-cores.old') and rerun so the expected WR-cores can be initialized."
        raise ValueError(msg)

    expected_wr_cores_sha = _get_expected_wr_cores_sha(wr_nic_dir)
    if expected_wr_cores_sha is not None:
        r = subprocess.run(["git", "-C", wr_cores_dir, "rev-parse", "HEAD"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("Failed to query local wr-cores revision with git rev-parse.")
        local_wr_cores_sha = r.stdout.strip().lower()
        if local_wr_cores_sha != expected_wr_cores_sha:
            msg = f"Local wr-cores SHA mismatch: found {local_wr_cores_sha}, expected {expected_wr_cores_sha}. "
            msg += "Rename/remove local wr-cores (for example: 'mv wr-cores wr-cores.old') and rerun."
            raise ValueError(msg)

    wr_subsystem_txt = Path(wr_subsystem_vhd).read_text()
    if 'mux_class_i(1) => x"ff");' in wr_subsystem_txt:
        return

    if not os.path.isfile(wr_patch_file):
        raise RuntimeError(f"Required WR patch file not found: {wr_patch_file}")
    r = subprocess.run(["git", "-C", wr_cores_dir, "apply", wr_patch_file], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"Failed to apply WR patch file {wr_patch_file}: {r.stderr.strip()}")
    print(f"Applied WR patch: {wr_patch_file}")
