#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import time
import argparse

from litex import RemoteClient


MANUAL_ENABLE_BIT = 0

CONTROL_BITS = {
    "time_running"    : 1,
    "time_valid"      : 2,
    "pcie_present"    : 3,
    "pcie_link_up"    : 4,
    "dma_synced"      : 5,
    "eth_present"     : 6,
    "eth_link_up"     : 7,
    "tx_activity"     : 8,
    "rx_activity"     : 9,
    "eth_tx_activity" : 10,
    "eth_rx_activity" : 11,
    "pps_level"       : 12,
}

PULSE_BITS = {
    "tx"     : 0,
    "rx"     : 1,
    "eth-tx" : 2,
    "eth-rx" : 3,
    "pps"    : 4,
}

PRESETS = {
    "off"        : [],
    "boot"       : [],
    "discovery"  : ["time_running", "time_valid", "pcie_present"],
    "idle"       : ["time_running", "time_valid"],
    "pcie-ready" : ["time_running", "time_valid", "pcie_present", "pcie_link_up", "dma_synced"],
    "eth-ready"  : ["time_running", "time_valid", "eth_present", "eth_link_up"],
    "tx"         : ["time_running", "time_valid", "tx_activity"],
    "rx"         : ["time_running", "time_valid", "rx_activity"],
    "duplex"     : ["time_running", "time_valid", "tx_activity", "rx_activity"],
    "eth-tx"     : ["time_running", "time_valid", "eth_tx_activity"],
    "eth-rx"     : ["time_running", "time_valid", "eth_rx_activity"],
}


def names_to_mask(names, bit_map):
    mask = 0
    for name in names:
        if not name:
            continue
        if name not in bit_map:
            raise ValueError(f"unknown control name: {name}")
        mask |= (1 << bit_map[name])
    return mask


def parse_name_list(value):
    if value is None or value == "":
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def read_status(bus):
    raw = bus.regs.leds_status.read()
    level = raw & 0xff
    output = (raw >> 8) & 0x1
    print(f"status: level={level} output={output}")


def build_control_mask(args):
    if args.raw_control is not None:
        control_mask = int(args.raw_control, 0)
    else:
        control_mask = names_to_mask(PRESETS.get(args.preset, []), CONTROL_BITS)
        control_mask |= names_to_mask(parse_name_list(args.set), CONTROL_BITS)
        control_mask &= ~names_to_mask(parse_name_list(args.clear), CONTROL_BITS)

    pulse_mask = names_to_mask(parse_name_list(args.pulse), PULSE_BITS)
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


def main():
    default_csr_csv = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "test", "csr.csv"))

    parser = argparse.ArgumentParser(
        description="Control the regular LiteX-M2SDR LED effect over Etherbone/RemoteClient.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--csr-csv",      default=default_csr_csv,        help="CSR definition file")
    parser.add_argument("--host",         default="localhost",            help="Remote host")
    parser.add_argument("--port",         default=1234, type=int,         help="Remote port")
    parser.add_argument("--list-presets", action="store_true",           help="List named presets and exit")
    parser.add_argument("--preset",       choices=sorted(PRESETS.keys()), help="Apply a named control preset in manual mode")
    parser.add_argument("--set",          default="",                    help="Comma-separated control bits to set in manual mode")
    parser.add_argument("--clear",        default="",                    help="Comma-separated control bits to clear from the preset in manual mode")
    parser.add_argument("--raw-control",  default=None,                   help="Write raw control bits for the LED inputs; manual mode is added automatically")
    parser.add_argument("--pulse",        default="",                    help="Comma-separated one-shot pulse names: tx,rx,eth-tx,eth-rx,pps")
    parser.add_argument("--count",        default=1, type=int,            help="Pulse repetition count")
    parser.add_argument("--period",       default=0.5, type=float,        help="Delay between repeated pulses in seconds")
    parser.add_argument("--release",      action="store_true",           help="Disable manual override and return LED control to the design")
    parser.add_argument("--read-status",  action="store_true",           help="Read back LED level/output after applying changes")
    args = parser.parse_args()

    if args.list_presets:
        for name, bits in PRESETS.items():
            rendered = ",".join(bits) if bits else "<none>"
            print(f"{name:10s} {rendered}")
        return

    if args.release and any([args.preset is not None, args.set, args.clear, args.raw_control is not None, args.pulse]):
        parser.error("--release cannot be combined with manual control options")

    bus = RemoteClient(host=args.host, port=args.port, csr_csv=args.csr_csv)
    bus.open()

    try:
        control_mask, pulse_mask = build_control_mask(args)

        bus.regs.leds_control.write(control_mask)
        print(f"control=0x{control_mask:08x}")

        for i in range(args.count):
            if pulse_mask:
                bus.regs.leds_pulse.write(pulse_mask)
                print(f"pulse[{i + 1}/{args.count}]=0x{pulse_mask:08x}")
            if i + 1 != args.count:
                time.sleep(args.period)

        if args.read_status:
            read_status(bus)
    finally:
        bus.close()


if __name__ == "__main__":
    main()
