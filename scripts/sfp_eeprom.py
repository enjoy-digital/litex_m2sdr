#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Read and identify an SFP module through the baseboard management bus."""

import argparse
import time

from litex import RemoteClient


TX_READY = 1 << 0
RX_READY = 1 << 1
NACK = 1 << 8
BUS_ERROR = 1 << 9


def wait_status(regs, mask, timeout=0.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = regs.sfp_i2c_master_status.read()
        # Error flags accompany a valid RX FIFO reply. They can otherwise be
        # stale, so only inspect them while RX_READY is asserted.
        if (status & RX_READY) and (status & (NACK | BUS_ERROR)):
            raise OSError(f"I2C transfer failed (status=0x{status:08x})")
        if status & mask:
            return status
    raise TimeoutError(f"I2C timeout waiting for status mask 0x{mask:x}")


def reset_master(regs):
    regs.sfp_i2c_master_active.write(0)
    regs.sfp_i2c_master_settings.write(0)
    for _ in range(16):
        if not (regs.sfp_i2c_master_status.read() & RX_READY):
            return
        regs.sfp_i2c_master_rxtx.read()
    raise OSError("could not drain the I2C RX FIFO")


def read_block(regs, slave, offset, length):
    """Perform the offset write/repeated-start read required by SFP EEPROMs."""
    if not 1 <= length <= 4:
        raise ValueError("LiteI2C transaction length must be 1..4 bytes")
    reset_master(regs)
    regs.sfp_i2c_master_settings.write(1 | (length << 8))
    regs.sfp_i2c_master_addr.write(slave)
    regs.sfp_i2c_master_active.write(1)
    wait_status(regs, TX_READY)
    regs.sfp_i2c_master_rxtx.write(offset)
    wait_status(regs, RX_READY)
    status = regs.sfp_i2c_master_status.read()
    if status & (NACK | BUS_ERROR):
        raise OSError(f"I2C transfer failed (status=0x{status:08x})")
    word = regs.sfp_i2c_master_rxtx.read()
    return word.to_bytes(4, "big")[-length:]


def recover_bus(regs):
    reset_master(regs)
    regs.sfp_i2c_master_settings.write(1 << 16)
    regs.sfp_i2c_master_addr.write(0)
    regs.sfp_i2c_master_active.write(1)
    wait_status(regs, TX_READY)
    regs.sfp_i2c_master_rxtx.write(0)
    wait_status(regs, RX_READY)
    status = regs.sfp_i2c_master_status.read()
    regs.sfp_i2c_master_rxtx.read()
    return status


def read_eeprom(regs, slave=0x50, size=256):
    data = bytearray()
    for offset in range(0, size, 4):
        count = min(4, size - offset)
        data.extend(read_block(regs, slave, offset, count))
    return bytes(data)


def text_field(data, start, end):
    return "".join(
        chr(value) if 32 <= value < 127 else "."
        for value in data[start:end]
    ).strip()


def checksum(data, first, last, check):
    expected = sum(data[first:last + 1]) & 0xff
    return expected == data[check], expected, data[check]


def identity(data):
    return {
        "identifier": data[0],
        "connector": data[2],
        "encoding": data[11],
        "nominal_bitrate_mbd": data[12] * 100,
        "vendor": text_field(data, 20, 36),
        "vendor_oui": data[37:40].hex(":"),
        "part_number": text_field(data, 40, 56),
        "revision": text_field(data, 56, 60),
        "serial": text_field(data, 68, 84),
        "date_code": text_field(data, 84, 92),
    }


def hex_dump(data):
    for offset in range(0, len(data), 16):
        row = data[offset:offset + 16]
        ascii_row = "".join(chr(value) if 32 <= value < 127 else "." for value in row)
        hex_row = " ".join(f"{value:02x}" for value in row)
        print(f"{offset:02x}: {hex_row:47s}  {ascii_row}")


def print_identity(data, prefix=""):
    fields = identity(data)
    print(f"{prefix}identifier       : 0x{fields['identifier']:02x}")
    print(f"{prefix}connector        : 0x{fields['connector']:02x}")
    print(f"{prefix}encoding         : 0x{fields['encoding']:02x}")
    print(f"{prefix}nominal bitrate  : {fields['nominal_bitrate_mbd']} MBd")
    print(f"{prefix}vendor           : {fields['vendor']!r}")
    print(f"{prefix}vendor OUI       : {fields['vendor_oui']}")
    print(f"{prefix}part number      : {fields['part_number']!r}")
    print(f"{prefix}revision         : {fields['revision']!r}")
    print(f"{prefix}serial           : {fields['serial']!r}")
    print(f"{prefix}date code        : {fields['date_code']!r}")
    for label, first, last, check_byte in (
        ("CC_BASE", 0, 62, 63),
        ("CC_EXT", 64, 94, 95),
    ):
        valid, expected, stored = checksum(data, first, last, check_byte)
        print(f"{prefix}{label:16s}: {'valid' if valid else 'INVALID'} "
              f"(calculated 0x{expected:02x}, stored 0x{stored:02x})")


def require_registers(regs):
    required = (
        "sfp_i2c_phy_speed_mode",
        "sfp_i2c_master_active",
        "sfp_i2c_master_settings",
        "sfp_i2c_master_addr",
        "sfp_i2c_master_rxtx",
        "sfp_i2c_master_status",
    )
    missing = [name for name in required if not hasattr(regs, name)]
    if missing:
        raise RuntimeError(
            "gateware has no SFP I2C master; rebuild with --with-sfp-i2c "
            f"(missing {', '.join(missing)})"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--csr-csv", default="scripts/csr.csv")
    parser.add_argument("--size", type=int, default=256, choices=(96, 128, 256))
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="repeat the complete A0 identity read (use 10 for a stability check)",
    )
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument(
        "--scan",
        action="store_true",
        help="scan addresses with read-only register-zero probes if A0 is absent",
    )
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")

    with RemoteClient(
        host=args.host,
        port=args.port,
        csr_csv=args.csr_csv,
        timeout=3.0,
        raise_on_timeout=True,
    ) as bus:
        regs = bus.regs
        require_registers(regs)
        regs.sfp_i2c_phy_speed_mode.write(0)  # 100kHz standard mode.
        print(f"I2C bus recovery status: 0x{recover_bus(regs):08x}")

        probes = {}
        for slave in (0x50, 0x51):
            try:
                probes[slave] = read_block(regs, slave, 0, 1)[0]
                print(f"I2C address 0x{slave:02x}: ACK, byte 0 = 0x{probes[slave]:02x}")
            except (OSError, TimeoutError) as error:
                print(f"I2C address 0x{slave:02x}: no response ({error})")

        if 0x50 not in probes:
            if args.scan:
                print(
                    "Scanning usable 7-bit addresses "
                    "(read-only offset-zero probes)..."
                )
                responders = []
                for slave in range(0x03, 0x78):
                    try:
                        responders.append((slave, read_block(regs, slave, 0, 1)[0]))
                    except (OSError, TimeoutError):
                        pass
                if responders:
                    for slave, value in responders:
                        print(f"  0x{slave:02x}: ACK, byte 0 = 0x{value:02x}")
                else:
                    print("  no I2C device acknowledged")
            raise SystemExit("SFP A0 EEPROM is not reachable")

        reference = None
        for index in range(args.repeat):
            data = read_eeprom(regs, 0x50, args.size)
            if len(data) < 96:
                raise RuntimeError(
                    "at least 96 EEPROM bytes are needed for identification"
                )
            current = identity(data)
            if reference is None:
                reference = current
                print("\nSFP A0 EEPROM:")
                hex_dump(data)
                print("\nDecoded identification:")
                print_identity(data, prefix="  ")
            else:
                state = "stable" if current == reference else "CHANGED"
                print(
                    f"identity read {index + 1}/{args.repeat}: {state}; "
                    f"{current['vendor']} {current['part_number']} "
                    f"{current['serial']}"
                )
            if index + 1 < args.repeat:
                time.sleep(args.interval)


if __name__ == "__main__":
    main()
