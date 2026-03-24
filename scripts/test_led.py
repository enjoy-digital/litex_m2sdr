#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import time

from litex import RemoteClient

# Constants ----------------------------------------------------------------------------------------

MANUAL_ENABLE_BIT = 0

CONTROL_BITS = {
    "time_running" : 1,
    "time_valid"   : 2,
    "pcie_present" : 3,
    "pcie_link_up" : 4,
    "dma_synced"   : 5,
    "eth_present"  : 6,
    "eth_link_up"  : 7,
    "tx_activity"  : 8,
    "rx_activity"  : 9,
    "pps_level"    : 10,
}

CONTROL_ALIASES = {
    "pps"    : "pps_level",
    "tx"     : "tx_activity",
    "rx"     : "rx_activity",
    "rf-tx"  : "tx_activity",
    "rf-rx"  : "rx_activity",
    "eth-tx" : "tx_activity",
    "eth-rx" : "rx_activity",
}

PULSE_BITS = {
    "tx"  : 0,
    "rx"  : 1,
    "pps" : 2,
}

PULSE_ALIASES = {
    "rf-tx"  : "tx",
    "rf-rx"  : "rx",
    "eth-tx" : "tx",
    "eth-rx" : "rx",
}

PRESETS = {
    "boot"      : {
        "bits"        : [],
        "description" : "Boot animation: time domain still stopped or invalid.",
    },
    "discovery" : {
        "bits"        : ["time_running", "time_valid", "pcie_present"],
        "description" : "Discovery animation while a transport is present but not yet ready.",
    },
    "idle"      : {
        "bits"        : ["time_running", "time_valid"],
        "description" : "Base idle breathing with no transport or traffic accents.",
    },
    "pcie-ready": {
        "bits"        : ["time_running", "time_valid", "pcie_present", "pcie_link_up", "dma_synced"],
        "description" : "PCIe-ready state with DMA time synchronization complete.",
    },
    "eth-ready" : {
        "bits"        : ["time_running", "time_valid", "eth_present", "eth_link_up"],
        "description" : "Ethernet-ready state with link up.",
    },
    "tx"        : {
        "bits"        : ["time_running", "time_valid", "tx_activity"],
        "description" : "Transmit activity accent.",
    },
    "rx"        : {
        "bits"        : ["time_running", "time_valid", "rx_activity"],
        "description" : "Receive activity accent.",
    },
    "duplex"    : {
        "bits"        : ["time_running", "time_valid", "tx_activity", "rx_activity"],
        "description" : "Transmit and receive activity accents together.",
    },
}

PRESET_ALIASES = {
    "rf-tx"      : "tx",
    "rf-rx"      : "rx",
    "rf-duplex"  : "duplex",
    "eth-tx"     : "tx",
    "eth-rx"     : "rx",
    "eth-duplex" : "duplex",
}

# Helpers ------------------------------------------------------------------------------------------

def resolve_name(name, aliases):
    return aliases.get(name, name)


def parse_name_list(value):
    if value is None or value == "":
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def names_to_mask(names, bit_map, aliases=None):
    aliases = aliases or {}
    mask = 0
    for name in names:
        if not name:
            continue
        resolved = resolve_name(name, aliases)
        if resolved not in bit_map:
            raise ValueError(f"unknown name: {name}")
        mask |= (1 << bit_map[resolved])
    return mask


def get_preset(name):
    if name is None:
        return None
    resolved = resolve_name(name, PRESET_ALIASES)
    return PRESETS[resolved]


def format_names(names):
    return ",".join(names) if names else "<none>"

# LED Driver ---------------------------------------------------------------------------------------

class LEDDriver:
    def __init__(self, bus):
        self.bus = bus
        self.control = bus.regs.leds_control
        self.pulse   = bus.regs.leds_pulse
        self.status  = bus.regs.leds_status

    def write_control(self, value):
        self.control.write(value)

    def write_pulse(self, value):
        self.pulse.write(value)

    def read_status(self):
        raw = self.status.read()
        return {
            "level"  : raw & 0xff,
            "output" : (raw >> 8) & 0x1,
        }


def build_control_mask(args):
    if args.raw_control is not None:
        control_mask = int(args.raw_control, 0)
    else:
        preset = get_preset(args.preset)
        preset_bits = [] if preset is None else preset["bits"]
        control_mask = names_to_mask(preset_bits, CONTROL_BITS, CONTROL_ALIASES)
        control_mask |= names_to_mask(parse_name_list(args.set), CONTROL_BITS, CONTROL_ALIASES)
        control_mask &= ~names_to_mask(parse_name_list(args.clear), CONTROL_BITS, CONTROL_ALIASES)

    pulse_mask = names_to_mask(parse_name_list(args.pulse), PULSE_BITS, PULSE_ALIASES)
    manual_requested = any([
        args.preset is not None,
        args.set,
        args.clear,
        args.raw_control is not None,
        pulse_mask != 0,
    ])

    if args.release:
        control_mask = 0
    elif manual_requested:
        control_mask |= (1 << MANUAL_ENABLE_BIT)

    return control_mask, pulse_mask


def list_presets():
    print("Presets:")
    for name, preset in PRESETS.items():
        print(f"  {name:10s} {preset['description']}")
        print(f"             bits={format_names(preset['bits'])}")
    if PRESET_ALIASES:
        aliases = ", ".join(f"{alias}->{target}" for alias, target in sorted(PRESET_ALIASES.items()))
        print(f"Aliases: {aliases}")


def list_controls():
    print("Control names:")
    for name in CONTROL_BITS:
        print(f"  {name}")
    if CONTROL_ALIASES:
        print("Control aliases:")
        for alias, target in sorted(CONTROL_ALIASES.items()):
            print(f"  {alias:10s} -> {target}")

    print("Pulse names:")
    for name in PULSE_BITS:
        print(f"  {name}")
    if PULSE_ALIASES:
        print("Pulse aliases:")
        for alias, target in sorted(PULSE_ALIASES.items()):
            print(f"  {alias:10s} -> {target}")

# Main ---------------------------------------------------------------------------------------------

def main():
    default_csr_csv = os.path.join(os.path.dirname(__file__), "csr.csv")
    parser = argparse.ArgumentParser(
        description="Control the LiteX-M2SDR status LED over Etherbone/RemoteClient.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--csr-csv",       default=default_csr_csv, help="CSR definition file")
    parser.add_argument("--host",          default="localhost",     help="Remote host")
    parser.add_argument("--port",          default=1234, type=int,  help="Remote port")
    parser.add_argument("--list-presets",  action="store_true",     help="List named presets and exit")
    parser.add_argument("--list-controls", action="store_true",     help="List control and pulse names and exit")
    parser.add_argument("--preset",        default=None,            help="Apply a named LED preset in manual mode (canonical names from --list-presets; RF/Ethernet aliases are still accepted)")
    parser.add_argument("--set",           default="",              help="Comma-separated control names to set in manual mode")
    parser.add_argument("--clear",         default="",              help="Comma-separated control names to clear from the preset in manual mode")
    parser.add_argument("--raw-control",   default=None,            help="Write raw LED control bits; manual mode is added automatically")
    parser.add_argument("--pulse",         default="",              help="Comma-separated one-shot pulse names: tx,rx,pps")
    parser.add_argument("--count",         default=1, type=int,     help="Pulse repetition count")
    parser.add_argument("--period",        default=0.5, type=float, help="Delay between repeated pulses in seconds")
    parser.add_argument("--release",       action="store_true",     help="Disable manual override and return LED control to the design")
    parser.add_argument("--read-status",   action="store_true",     help="Read back LED level/output after applying changes")
    args = parser.parse_args()

    if args.list_presets:
        list_presets()
        return

    if args.list_controls:
        list_controls()
        return

    if args.preset is not None and resolve_name(args.preset, PRESET_ALIASES) not in PRESETS:
        parser.error(f"unknown preset: {args.preset}")

    if args.release and any([args.preset is not None, args.set, args.clear, args.raw_control is not None, args.pulse]):
        parser.error("--release cannot be combined with manual control options")

    bus = RemoteClient(host=args.host, port=args.port, csr_csv=args.csr_csv)
    bus.open()

    try:
        led = LEDDriver(bus)
        control_mask, pulse_mask = build_control_mask(args)

        led.write_control(control_mask)
        print(f"control=0x{control_mask:08x}")

        for i in range(args.count):
            if pulse_mask:
                led.write_pulse(pulse_mask)
                print(f"pulse[{i + 1}/{args.count}]=0x{pulse_mask:08x}")
            if i + 1 != args.count:
                time.sleep(args.period)

        if args.read_status:
            status = led.read_status()
            print(f"status: level={status['level']} output={status['output']}")
    finally:
        bus.close()


if __name__ == "__main__":
    main()
