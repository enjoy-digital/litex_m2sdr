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

def build_driver(path, cmake_options=None, prefix="/usr", do_install=False):
    base_dir   = os.path.dirname(os.path.abspath(__file__))
    build_path = os.path.join(base_dir, path, 'build')
    os.makedirs(build_path, exist_ok=True)
    cmake_options = cmake_options or []
    run_command(["cmake", "..", f"-DCMAKE_INSTALL_PREFIX={prefix}", *cmake_options], cwd=build_path)
    run_command(["make", "clean", "all"], cwd=build_path)
    if do_install:
        run_command(["make", "install"], cwd=build_path)

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX-M2SDR Software build.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--interface",   default="litepcie",  help="Control/Data path interface", choices=["litepcie", "liteeth"])
    parser.add_argument("--prefix",      default="/usr",      help="Install prefix for SoapySDR driver.")
    parser.add_argument("--no-sudo",     action="store_true", help="Skip install steps even when running as root.")
    parser.add_argument("--no-install",  action="store_true", help="Build only; do not run install steps.")
    parser.add_argument("--skip-kernel", action="store_true", help="Skip kernel driver build/install.")

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

    # Kernel compilation.
    if (args.interface == "litepcie") and (not args.skip_kernel):
        run_command(["make", "clean", "all"], cwd=os.path.join(base_dir, "kernel"))
        if do_install:
            run_command(["make", "install"], cwd=os.path.join(base_dir, "kernel"))

    # Utilities compilation.
    run_command(["make", "clean", f"INTERFACE={interface}", "all"], cwd=os.path.join(base_dir, "user"))

    # SoapySDR Driver compilation.
    build_driver("soapysdr", cmake_options=flags, prefix=args.prefix, do_install=do_install)

if __name__ == "__main__":
    main()
