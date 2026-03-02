#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import re
import subprocess
from pathlib import Path

WR_NIC_DIR_CANDIDATE_PATTERNS = (
    ("root",   "litex_wr_nic"),
    ("parent", "litex_wr_nic"),
    ("root",   "litex_wr_nic", "litex_wr_nic"),
    ("parent", "litex_wr_nic", "litex_wr_nic"),
)

WR_FIRMWARE_BUILD_SCRIPT_REL = os.path.join("firmware", "build.py")
WR_FIRMWARE_IMAGE_REL        = os.path.join("firmware", "spec_a7_wrc.bram")
WR_COMMON_REL                = os.path.join("gateware", "wr_common.py")

WR_CORES_DIRNAME          = "wr-cores"
WR_SUBSYSTEM_VHD_REL      = os.path.join("modules", "wrc_core", "xwr_subsystem.vhd")
WR_PATCH_FILE_REL         = os.path.join("patches", "wr-cores", "0001-xwr_subsystem-mux-class.patch")
WR_PATCHED_SIGNATURE      = 'mux_class_i(1) => x"ff");'
WR_CORES_RENAME_HINT      = "Rename/remove local wr-cores (for example: 'mv wr-cores wr-cores.old') and rerun."
WR_CORES_INIT_RENAME_HINT = "Rename/remove it (for example: 'mv wr-cores wr-cores.old') and rerun so the expected WR-cores can be initialized."

ERR_NO_WR_FIRMWARE_PATH = "White Rabbit build requested but no WR firmware path found. Use --wr-firmware or --wr-nic-dir."
ERR_WR_FIRMWARE_BUILD_FAILED = "White Rabbit Firmware build failed."
ERR_WR_CORES_STALE_TREE = "Incompatible local 'wr-cores' tree detected (missing modules/wrc_core/xwr_subsystem.vhd). "
ERR_WR_CORES_STALE_TREE += "This usually means an older WR-cores checkout is present in the litex_m2sdr directory. "
ERR_GIT_REV_PARSE_FAILED = "Failed to query local wr-cores revision with git rev-parse."


def _iter_wr_nic_candidates(root_dir):
    parent_dir = os.path.dirname(root_dir)
    bases = {
        "root": root_dir,
        "parent": parent_dir,
    }
    for pattern in WR_NIC_DIR_CANDIDATE_PATTERNS:
        yield os.path.join(bases[pattern[0]], *pattern[1:])


def resolve_wr_paths(root_dir, wr_nic_dir=None, wr_firmware=None):
    # Auto-discover local/sibling litex_wr_nic checkout when not explicitly provided.
    if wr_nic_dir is None:
        for candidate in _iter_wr_nic_candidates(root_dir):
            if os.path.isfile(os.path.join(candidate, WR_FIRMWARE_BUILD_SCRIPT_REL)):
                wr_nic_dir = candidate
                break

    if wr_firmware is None and wr_nic_dir:
        candidate = os.path.join(wr_nic_dir, WR_FIRMWARE_IMAGE_REL)
        if os.path.isfile(candidate):
            wr_firmware = candidate

    return wr_nic_dir, wr_firmware


def build_wr_firmware(wr_nic_dir, wr_firmware, wr_firmware_target):
    if wr_nic_dir is None and wr_firmware is None:
        raise ValueError(ERR_NO_WR_FIRMWARE_PATH)

    firmware_dir = os.path.dirname(os.path.abspath(wr_firmware)) if wr_firmware else os.path.join(wr_nic_dir, "firmware")
    build_script = os.path.join(firmware_dir, os.path.basename(WR_FIRMWARE_BUILD_SCRIPT_REL))
    print(f"Building White Rabbit firmware in {firmware_dir}...")
    r = subprocess.run([build_script, "--target", wr_firmware_target], cwd=firmware_dir)
    if r.returncode != 0:
        raise RuntimeError(ERR_WR_FIRMWARE_BUILD_FAILED)


def _get_expected_wr_cores_sha(wr_nic_dir):
    if wr_nic_dir is None:
        return None

    wr_common = os.path.join(wr_nic_dir, WR_COMMON_REL)
    if not os.path.isfile(wr_common):
        return None

    wr_common_txt = Path(wr_common).read_text()
    m = re.search(r'WR_CORES_SHA1\s*=\s*"([0-9a-fA-F]+)"', wr_common_txt)
    return m.group(1).lower() if m else None


def preflight_wr_cores(root_dir, wr_nic_dir):
    wr_cores_dir = os.path.join(root_dir, WR_CORES_DIRNAME)
    wr_subsystem_vhd = os.path.join(wr_cores_dir, WR_SUBSYSTEM_VHD_REL)
    wr_patch_file = os.path.join(root_dir, WR_PATCH_FILE_REL)

    if not os.path.isdir(wr_cores_dir):
        return

    if not os.path.isfile(wr_subsystem_vhd):
        msg = ERR_WR_CORES_STALE_TREE + WR_CORES_INIT_RENAME_HINT
        raise ValueError(msg)

    expected_wr_cores_sha = _get_expected_wr_cores_sha(wr_nic_dir)
    if expected_wr_cores_sha is not None:
        r = subprocess.run(["git", "-C", wr_cores_dir, "rev-parse", "HEAD"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(ERR_GIT_REV_PARSE_FAILED)
        local_wr_cores_sha = r.stdout.strip().lower()
        if local_wr_cores_sha != expected_wr_cores_sha:
            msg = f"Local wr-cores SHA mismatch: found {local_wr_cores_sha}, expected {expected_wr_cores_sha}. "
            msg += WR_CORES_RENAME_HINT
            raise ValueError(msg)

    wr_subsystem_txt = Path(wr_subsystem_vhd).read_text()
    if WR_PATCHED_SIGNATURE in wr_subsystem_txt:
        return

    if not os.path.isfile(wr_patch_file):
        raise RuntimeError(f"Required WR patch file not found: {wr_patch_file}")
    r = subprocess.run(["git", "-C", wr_cores_dir, "apply", wr_patch_file], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"Failed to apply WR patch file {wr_patch_file}: {r.stderr.strip()}")
    print(f"Applied WR patch: {wr_patch_file}")
