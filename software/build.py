#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import subprocess

def run_command(command):
    try:
        subprocess.run(command, shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"run_command error: {e}")

def build_driver(path, cmake_options=""):
    base_dir   = os.path.dirname(os.path.abspath(__file__))
    build_path = os.path.join(base_dir, path, 'build')
    os.makedirs(build_path, exist_ok=True)
    commands = [
        f"cd {build_path} && cmake ../ {cmake_options}",
        f"cd {build_path} && make clean all",
        f"cd {build_path} && sudo make install"
    ]
    for command in commands:
        run_command(command)

parser = argparse.ArgumentParser(description="LiteX-M2SDR Software build.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument("--interface", default="litepcie", help="Control/Data path interface", choices=["litepcie", "liteeth"])

args = parser.parse_args()

# Control/Data path flags.
if args.interface == "litepcie":
    flags = "-DUSE_LITEETH=OFF"
else:
    flags = "-DUSE_LITEETH=ON"

# Kernel compilation.
if (args.interface == "litepcie"):
    run_command("cd kernel && make clean all")

# Utilities compilation.
run_command("cd user   && make clean all")

# SoapySDR Driver compilation.
build_driver("soapysdr", f"-DCMAKE_INSTALL_PREFIX=/usr {flags}")
