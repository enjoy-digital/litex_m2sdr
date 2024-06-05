#!/usr/bin/env python3

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

run_command("cd kernel && make clean all")
run_command("cd user   && make clean all")
build_driver("soapysdr", "-DCMAKE_INSTALL_PREFIX=/usr")
