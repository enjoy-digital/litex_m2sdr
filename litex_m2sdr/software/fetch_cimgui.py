#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import subprocess
import sys

CIMGUI_REPO   = "https://github.com/cimgui/cimgui.git"
CIMGUI_COMMIT = "d61baefa0ce2a9db938ffdeb29e64f90f44cc037"
IMGUI_COMMIT  = "44aa9a4b3a6f27d09a4eb5770d095cbd376dfc4b"


def run_command(command, cwd=None, capture=False):
    if capture:
        return subprocess.run(command, cwd=cwd, check=True, text=True, capture_output=True)
    return subprocess.run(command, cwd=cwd, check=True)


def repo_dirty(path):
    result = run_command(["git", "status", "--porcelain"], cwd=path, capture=True)
    return bool(result.stdout.strip())


def require_clean_repo(path, force):
    if repo_dirty(path) and (not force):
        raise RuntimeError(
            f"{path} has local changes.\n"
            "Refusing to modify it automatically. Re-run with --force if you want to reuse this checkout."
        )


def ensure_cimgui(path, force=False):
    if not os.path.exists(path):
        run_command(["git", "clone", CIMGUI_REPO, path])
    elif not os.path.isdir(os.path.join(path, ".git")):
        raise RuntimeError(
            f"{path} exists but is not a git checkout.\n"
            "Move it away or remove it, then rerun this script."
        )

    require_clean_repo(path, force)
    run_command(["git", "fetch", "origin"], cwd=path)
    run_command(["git", "checkout", CIMGUI_COMMIT], cwd=path)
    run_command(["git", "submodule", "update", "--init", "--recursive"], cwd=path)

    imgui_path = os.path.join(path, "imgui")
    imgui_head = run_command(["git", "rev-parse", "HEAD"], cwd=imgui_path, capture=True).stdout.strip()
    if imgui_head != IMGUI_COMMIT:
        raise RuntimeError(
            f"Unexpected imgui revision: {imgui_head} (expected {IMGUI_COMMIT})."
        )


def main():
    parser = argparse.ArgumentParser(
        description="Populate software/user/cimgui at the pinned LiteX-M2SDR revision."
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Allow reuse of an existing cimgui checkout after forcing it to the pinned revision.",
    )
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    cimgui_dir = os.path.join(base_dir, "user", "cimgui")

    try:
        ensure_cimgui(cimgui_dir, force=args.force)
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f"fetch_cimgui.py: {error}", file=sys.stderr)
        return 1

    print(f"cimgui ready in {cimgui_dir}")
    print(f"  cimgui: {CIMGUI_COMMIT}")
    print(f"  imgui : {IMGUI_COMMIT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
