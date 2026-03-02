#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import hashlib
import os
import re
import subprocess
from pathlib import Path

# Constants ----------------------------------------------------------------------------------------

WR_NIC_DIR_CANDIDATE_PATTERNS = (
    ("root",   "litex_wr_nic"),
    ("parent", "litex_wr_nic"),
    ("root",   "litex_wr_nic", "litex_wr_nic"),
    ("parent", "litex_wr_nic", "litex_wr_nic"),
)

WR_FIRMWARE_BUILD_SCRIPT_REL = os.path.join("firmware", "build.py")
WR_FIRMWARE_IMAGE_REL        = os.path.join("firmware", "spec_a7_wrc.bram")
WR_COMMON_REL                = os.path.join("gateware", "wr_common.py")

WR_CORES_DIRNAME        = "wr-cores"
WR_SUBSYSTEM_VHD_REL    = os.path.join("modules", "wrc_core", "xwr_subsystem.vhd")
WR_PATCH_FILE_REL       = os.path.join("patches", "wr-cores", "0001-xwr_subsystem-mux-class.patch")
WR_PATCHED_SIGNATURE    = 'mux_class_i(1) => x"ff");'
WR_PATCH_MODE_VALUES    = ("auto", "check", "off")

WR_CORES_RENAME_HINT      = "Rename/remove local wr-cores (for example: 'mv wr-cores wr-cores.old') and rerun."
WR_CORES_INIT_RENAME_HINT = "Rename/remove it (for example: 'mv wr-cores wr-cores.old') and rerun so the expected WR-cores can be initialized."

ERR_NO_WR_FIRMWARE_PATH    = "White Rabbit build requested but no WR firmware path found. Use --wr-firmware or --wr-nic-dir."
ERR_WR_FIRMWARE_BUILD_FAIL = "White Rabbit Firmware build failed."
ERR_WR_CORES_STALE_TREE    = "Incompatible local 'wr-cores' tree detected (missing modules/wrc_core/xwr_subsystem.vhd). "
ERR_WR_CORES_STALE_TREE   += "This usually means an older WR-cores checkout is present in the litex_m2sdr directory. "
ERR_GIT_REV_PARSE_FAILED   = "Failed to query local wr-cores revision with git rev-parse."
ERR_WR_PATCH_MODE          = f"Invalid WR patch mode '{{patch_mode}}'. Expected one of: {', '.join(WR_PATCH_MODE_VALUES)}."
ERR_WR_PATCH_MISSING_CHECK = "WR patch not applied in xwr_subsystem.vhd. Re-run with --wr-patch-mode=auto to apply it automatically."

# Internal Helpers ---------------------------------------------------------------------------------


def _fingerprint_file(path):
    if not os.path.isfile(path):
        return {
            "exists"   : False,
            "mtime_ns" : None,
            "sha256"   : None,
        }

    sha = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            sha.update(chunk)

    return {
        "exists"   : True,
        "mtime_ns" : os.stat(path).st_mtime_ns,
        "sha256"   : sha.hexdigest(),
    }


def _iter_wr_nic_candidates(root_dir):
    parent_dir = os.path.dirname(root_dir)
    bases = {
        "root"   : root_dir,
        "parent" : parent_dir,
    }
    for pattern in WR_NIC_DIR_CANDIDATE_PATTERNS:
        yield os.path.join(bases[pattern[0]], *pattern[1:])


def _get_available_sfps_for_variant(variant, baseboard_io):
    if variant != "baseboard":
        return []
    return sorted({
        number for (name, number, *_rest) in baseboard_io
        if name == "sfp"
    })


def _resolve_firmware_output_path(wr_nic_dir, wr_firmware):
    if wr_firmware:
        return os.path.abspath(wr_firmware)
    if wr_nic_dir:
        return os.path.join(wr_nic_dir, WR_FIRMWARE_IMAGE_REL)
    return None


def _get_expected_wr_cores_sha(wr_nic_dir):
    if wr_nic_dir is None:
        return None

    wr_common = os.path.join(wr_nic_dir, WR_COMMON_REL)
    if not os.path.isfile(wr_common):
        return None

    wr_common_txt = Path(wr_common).read_text()
    match = re.search(r'WR_CORES_SHA1\s*=\s*"([0-9a-fA-F]+)"', wr_common_txt)
    return match.group(1).lower() if match else None

# Public API ---------------------------------------------------------------------------------------


def resolve_wr_paths(root_dir, wr_nic_dir=None, wr_firmware=None):
    if wr_nic_dir is None:
        for candidate in _iter_wr_nic_candidates(root_dir):
            build_script = os.path.join(candidate, WR_FIRMWARE_BUILD_SCRIPT_REL)
            if os.path.isfile(build_script):
                wr_nic_dir = candidate
                break

    if wr_firmware is None and wr_nic_dir:
        candidate = os.path.join(wr_nic_dir, WR_FIRMWARE_IMAGE_REL)
        if os.path.isfile(candidate):
            wr_firmware = candidate

    return wr_nic_dir, wr_firmware


def build_wr_firmware(wr_nic_dir, wr_firmware, wr_firmware_target, enforce_fresh=True):
    if wr_nic_dir is None and wr_firmware is None:
        raise ValueError(ERR_NO_WR_FIRMWARE_PATH)

    firmware_dir = os.path.dirname(os.path.abspath(wr_firmware)) if wr_firmware else os.path.join(wr_nic_dir, "firmware")
    build_script = os.path.join(firmware_dir, os.path.basename(WR_FIRMWARE_BUILD_SCRIPT_REL))
    firmware_out = _resolve_firmware_output_path(wr_nic_dir, wr_firmware)

    before = _fingerprint_file(firmware_out) if firmware_out else _fingerprint_file("")

    print(f"Building White Rabbit firmware in {firmware_dir}...")
    result = subprocess.run([build_script, "--target", wr_firmware_target], cwd=firmware_dir)
    if result.returncode != 0:
        raise RuntimeError(ERR_WR_FIRMWARE_BUILD_FAIL)

    if firmware_out is None:
        return

    after = _fingerprint_file(firmware_out)
    if not after["exists"]:
        raise RuntimeError(f"White Rabbit firmware output not found after build: {firmware_out}")

    if enforce_fresh and before["exists"]:
        unchanged_mtime = (before["mtime_ns"] == after["mtime_ns"])
        unchanged_hash  = (before["sha256"]   == after["sha256"])
        if unchanged_mtime and unchanged_hash:
            msg  = f"White Rabbit firmware output did not change after build: {firmware_out}. "
            msg += "This usually indicates a stale/no-op firmware build."
            raise RuntimeError(msg)


def inspect_wr_cores(root_dir, wr_nic_dir):
    wr_cores_dir     = os.path.join(root_dir, WR_CORES_DIRNAME)
    wr_subsystem_vhd = os.path.join(wr_cores_dir, WR_SUBSYSTEM_VHD_REL)
    wr_patch_file    = os.path.join(root_dir, WR_PATCH_FILE_REL)

    state = {
        "wr_cores_dir"     : wr_cores_dir,
        "wr_subsystem_vhd" : wr_subsystem_vhd,
        "wr_patch_file"    : wr_patch_file,
        "exists"           : os.path.isdir(wr_cores_dir),
        "valid_layout"     : False,
        "expected_sha"     : _get_expected_wr_cores_sha(wr_nic_dir),
        "local_sha"        : None,
        "sha_match"        : None,
        "patched"          : None,
    }

    if not state["exists"]:
        return state

    state["valid_layout"] = os.path.isfile(wr_subsystem_vhd)
    if not state["valid_layout"]:
        return state

    if state["expected_sha"] is not None:
        result = subprocess.run(["git", "-C", wr_cores_dir, "rev-parse", "HEAD"], capture_output=True, text=True)
        if result.returncode == 0:
            state["local_sha"] = result.stdout.strip().lower()
            state["sha_match"] = (state["local_sha"] == state["expected_sha"])
        else:
            state["local_sha"] = "<rev-parse failed>"
            state["sha_match"] = False

    wr_subsystem_txt = Path(wr_subsystem_vhd).read_text()
    state["patched"] = WR_PATCHED_SIGNATURE in wr_subsystem_txt
    return state


def preflight_wr_cores(root_dir, wr_nic_dir, patch_mode="auto"):
    if patch_mode not in WR_PATCH_MODE_VALUES:
        raise ValueError(ERR_WR_PATCH_MODE.format(patch_mode=patch_mode))

    state = inspect_wr_cores(root_dir, wr_nic_dir)

    if not state["exists"]:
        return state

    if not state["valid_layout"]:
        raise ValueError(ERR_WR_CORES_STALE_TREE + WR_CORES_INIT_RENAME_HINT)

    if state["expected_sha"] is not None:
        if state["local_sha"] is None:
            raise RuntimeError(ERR_GIT_REV_PARSE_FAILED)
        if state["sha_match"] is False:
            msg  = f"Local wr-cores SHA mismatch: found {state['local_sha']}, expected {state['expected_sha']}. "
            msg += WR_CORES_RENAME_HINT
            raise ValueError(msg)

    if patch_mode == "off":
        return state

    if state["patched"]:
        return state

    if patch_mode == "check":
        raise ValueError(ERR_WR_PATCH_MISSING_CHECK)

    wr_patch_file = state["wr_patch_file"]
    if not os.path.isfile(wr_patch_file):
        raise RuntimeError(f"Required WR patch file not found: {wr_patch_file}")

    result = subprocess.run(["git", "-C", state["wr_cores_dir"], "apply", wr_patch_file], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Failed to apply WR patch file {wr_patch_file}: {result.stderr.strip()}")

    print(f"Applied WR patch: {wr_patch_file}")
    state["patched"] = True
    return state


def validate_wr_platform(variant, wr_sfp, baseboard_io):
    errors = []
    available_sfps = _get_available_sfps_for_variant(variant, baseboard_io)

    if variant != "baseboard":
        errors.append("White Rabbit is only supported with --variant=baseboard (requires baseboard SFP resources).")

    if not available_sfps:
        errors.append("No SFP resources available for White Rabbit on this variant.")

    resolved_wr_sfp = wr_sfp
    auto_selected = False

    if available_sfps:
        if resolved_wr_sfp is None:
            resolved_wr_sfp = available_sfps[0]
            auto_selected = True
        elif resolved_wr_sfp not in available_sfps:
            msg = f"White Rabbit SFP sfp:{resolved_wr_sfp} not available on this variant. "
            msg += f"Available SFPs: {available_sfps}"
            errors.append(msg)

    if errors:
        msg = "White Rabbit platform validation failed:\n"
        msg += "\n".join(f"- {e}" for e in errors)
        raise ValueError(msg)

    return {
        "wr_sfp"        : resolved_wr_sfp,
        "available_sfps": available_sfps,
        "auto_selected" : auto_selected,
    }


def print_wr_status(*, variant, wr_sfp, available_sfps, wr_nic_dir, wr_firmware, wr_cores_state, patch_mode):
    print("White Rabbit status:")
    print(f"- variant: {variant}")
    print(f"- requested wr_sfp: {wr_sfp}")
    print(f"- available sfp indices: {available_sfps}")
    print(f"- wr_nic_dir: {wr_nic_dir}")
    print(f"- wr_firmware: {wr_firmware}")
    print(f"- wr_patch_mode: {patch_mode}")

    if not wr_cores_state["exists"]:
        print("- wr_cores: not present in repo (will be initialized by build flow when needed)")
        return

    if not wr_cores_state["valid_layout"]:
        print("- wr_cores: present but incompatible layout (missing modules/wrc_core/xwr_subsystem.vhd)")
        return

    if wr_cores_state["expected_sha"] is not None:
        print(f"- wr_cores expected sha: {wr_cores_state['expected_sha']}")
        print(f"- wr_cores local sha: {wr_cores_state['local_sha']}")
        print(f"- wr_cores sha match: {wr_cores_state['sha_match']}")

    print(f"- wr_cores patch applied: {wr_cores_state['patched']}")


def prepare_wr_environment(*,
    root_dir,
    variant,
    baseboard_io,
    with_white_rabbit,
    wr_sfp,
    wr_nic_dir,
    wr_firmware,
    wr_firmware_target,
    build,
    patch_mode,
    status,
):
    wr_nic_dir, wr_firmware = resolve_wr_paths(
        root_dir    = root_dir,
        wr_nic_dir  = wr_nic_dir,
        wr_firmware = wr_firmware,
    )

    available_sfps = _get_available_sfps_for_variant(variant, baseboard_io)
    resolved_wr_sfp = wr_sfp

    if with_white_rabbit:
        validation = validate_wr_platform(
            variant      = variant,
            wr_sfp       = wr_sfp,
            baseboard_io = baseboard_io,
        )
        resolved_wr_sfp = validation["wr_sfp"]
        available_sfps  = validation["available_sfps"]

        if validation["auto_selected"]:
            print(f"White Rabbit SFP auto-selected: sfp:{resolved_wr_sfp} (available: {available_sfps})")

        # WR integration needs a BRAM image even for elaboration-only runs.
        # If not available yet, build it once from wr_nic_dir.
        if wr_firmware is None and wr_nic_dir is not None:
            build_wr_firmware(
                wr_nic_dir         = wr_nic_dir,
                wr_firmware        = wr_firmware,
                wr_firmware_target = wr_firmware_target,
                enforce_fresh      = False,
            )
            _, wr_firmware = resolve_wr_paths(
                root_dir    = root_dir,
                wr_nic_dir  = wr_nic_dir,
                wr_firmware = wr_firmware,
            )

        if build:
            build_wr_firmware(
                wr_nic_dir         = wr_nic_dir,
                wr_firmware        = wr_firmware,
                wr_firmware_target = wr_firmware_target,
                enforce_fresh      = True,
            )

        if wr_firmware is None:
            raise ValueError(ERR_NO_WR_FIRMWARE_PATH)

        preflight_wr_cores(
            root_dir   = root_dir,
            wr_nic_dir = wr_nic_dir,
            patch_mode = patch_mode,
        )

    wr_cores_state = inspect_wr_cores(root_dir, wr_nic_dir)

    if with_white_rabbit:
        print(
            "White Rabbit config: "
            f"variant={variant}, "
            f"wr_sfp={resolved_wr_sfp}, "
            f"wr_nic_dir={wr_nic_dir}, "
            f"wr_firmware={wr_firmware}, "
            f"patch_mode={patch_mode}, "
            f"wr_cores_exists={wr_cores_state['exists']}, "
            f"wr_cores_patched={wr_cores_state['patched']}"
        )

    if status:
        print_wr_status(
            variant        = variant,
            wr_sfp         = resolved_wr_sfp,
            available_sfps = available_sfps,
            wr_nic_dir     = wr_nic_dir,
            wr_firmware    = wr_firmware,
            wr_cores_state = wr_cores_state,
            patch_mode     = patch_mode,
        )

    return {
        "wr_nic_dir"     : wr_nic_dir,
        "wr_firmware"    : wr_firmware,
        "wr_sfp"         : resolved_wr_sfp,
        "available_sfps" : available_sfps,
        "wr_cores_state" : wr_cores_state,
    }
