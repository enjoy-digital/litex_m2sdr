#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse
import subprocess

# Build Utilities ----------------------------------------------------------------------------------

def run_command(command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True)

def build_driver(path, cmake_options=None, prefix="/usr", do_install=False, clean_first=False, interface=None):
    base_dir   = os.path.dirname(os.path.abspath(__file__))
    build_path = os.path.join(base_dir, path, 'build')
    os.makedirs(build_path, exist_ok=True)
    cmake_options = cmake_options or []
    run_command(["cmake", "..", f"-DCMAKE_INSTALL_PREFIX={prefix}", *cmake_options], cwd=build_path)
    marker_path = os.path.join(build_path, ".build-interface")
    if interface is not None:
        try:
            previous_interface = open(marker_path, "r", encoding="utf-8").read().strip()
        except FileNotFoundError:
            previous_interface = None
        if previous_interface and previous_interface != interface:
            clean_first = True
    if clean_first:
        run_command(["make", "clean"], cwd=build_path)
    run_command(["make", "all"], cwd=build_path)
    if interface is not None:
        with open(marker_path, "w", encoding="utf-8") as f:
            f.write(interface + "\n")
    if do_install:
        run_command(["make", "install"], cwd=build_path)

def fetch_cimgui(base_dir):
    run_command(["./fetch_cimgui.py"], cwd=base_dir)


def install_user_software(base_dir, prefix, interface):
    user_dir = os.path.join(base_dir, "user")
    run_command(["make", f"INTERFACE={interface}", f"PREFIX={prefix}", "install"], cwd=user_dir)
    run_command(["make", f"INTERFACE={interface}", f"PREFIX={prefix}", "install_dev"], cwd=user_dir)
    if prefix == "/usr":
        run_command(["ldconfig"])

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX-M2SDR Software build.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--interface",   default="litepcie",  help="Control/Data path interface", choices=["litepcie", "liteeth"])
    parser.add_argument("--prefix",      default="/usr",      help="Install prefix for SoapySDR driver.")
    parser.add_argument("--fetch-cimgui", action="store_true", help="Populate software/user/cimgui at the pinned revision before building.")
    parser.add_argument("--no-sudo",     action="store_true", help="Skip install steps even when running as root.")
    parser.add_argument("--no-install",  action="store_true", help="Build only; do not run install steps.")
    parser.add_argument("--skip-kernel", action="store_true", help="Skip kernel driver build/install.")
    parser.add_argument("--clean",       action="store_true", help="Run clean targets before building.")

    args = parser.parse_args()
    base_dir = os.path.dirname(os.path.abspath(__file__))

    # Control/Data path flags.
    if args.interface == "litepcie":
        flags     = ["-DUSE_LITEETH=OFF"]
        interface = "USE_LITEPCIE"
    else:
        flags     = ["-DUSE_LITEETH=ON"]
        interface = "USE_LITEETH"

    is_root    = (os.geteuid() == 0)
    do_install = (not args.no_install) and is_root and (not args.no_sudo)
    if args.no_install:
        print("Install steps skipped (--no-install).")
    elif not is_root:
        print("Install steps skipped (run as root to install).")

    if args.fetch_cimgui:
        fetch_cimgui(base_dir)

    # Kernel compilation.
    if (args.interface == "litepcie") and (not args.skip_kernel):
        kernel_dir = os.path.join(base_dir, "kernel")
        if args.clean:
            run_command(["make", "clean"], cwd=kernel_dir)
        run_command(["make", "all"], cwd=kernel_dir)
        if do_install:
            run_command(["make", "install"], cwd=kernel_dir)

    # Utilities compilation.
    user_dir = os.path.join(base_dir, "user")
    if args.clean:
        run_command(["make", f"INTERFACE={interface}", "clean"], cwd=user_dir)
    run_command(["make", f"INTERFACE={interface}", "all"], cwd=user_dir)
    if do_install:
        install_user_software(base_dir, args.prefix, interface)

    # SoapySDR Driver compilation.
    build_driver("soapysdr", cmake_options=flags, prefix=args.prefix, do_install=do_install, clean_first=args.clean, interface=interface)

if __name__ == "__main__":
    main()
