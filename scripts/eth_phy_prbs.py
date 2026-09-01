#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Exercise and inspect the experimental Ethernet GTP PRBS diagnostics."""

import argparse
import time

from litex import RemoteClient


PRBS_MODES = {
    "disabled": 0b000,
    "prbs7": 0b001,
    "prbs15": 0b010,
    "prbs23": 0b011,
    "prbs31": 0b100,
}
LOOPBACK_MODES = {
    "normal": 0b000,
    "near-pcs": 0b001,
    "near-pma": 0b010,
    "far-pma": 0b100,
    "far-pcs": 0b110,
}
STATUS_BITS = (
    ("qpll_lock", 0),
    ("rx_cdr_lock", 1),
    ("tx_reset_done", 2),
    ("rx_reset_done", 3),
    ("byte_aligned", 4),
    ("pcs_link_up", 5),
    ("rx_overflow", 6),
    ("rx_prbs_error", 7),
)


def control_word(loopback="normal", tx="disabled", rx="disabled",
                 force_error=False, counter_reset=False):
    value = LOOPBACK_MODES[loopback]
    value |= PRBS_MODES[tx] << 4
    value |= PRBS_MODES[rx] << 8
    value |= int(force_error) << 12
    value |= int(counter_reset) << 13
    return value


def decode_status(value):
    return {name: bool(value & (1 << bit)) for name, bit in STATUS_BITS}


def require_registers(regs):
    required = (
        "eth_phy_prbs_control",
        "eth_phy_prbs_bits",
        "eth_phy_prbs_errors",
        "eth_phy_pcs_code_errors",
        "eth_phy_pcs_disp_errors",
        "eth_phy_cdr_lock_losses",
        "eth_phy_debug_status",
    )
    missing = [name for name in required if not hasattr(regs, name)]
    if missing:
        raise RuntimeError(
            "gateware has no Ethernet diagnostics; rebuild with --with-eth-phy-probe "
            f"(missing {', '.join(missing)})"
        )


def read_counters(regs):
    return {
        "bits": regs.eth_phy_prbs_bits.read(),
        "prbs_errors": regs.eth_phy_prbs_errors.read(),
        "code_errors": regs.eth_phy_pcs_code_errors.read(),
        "disparity_errors": regs.eth_phy_pcs_disp_errors.read(),
        "cdr_lock_losses": regs.eth_phy_cdr_lock_losses.read(),
        "status": decode_status(regs.eth_phy_debug_status.read()),
    }


def print_sample(elapsed, counters):
    bits = counters["bits"]
    events = counters["prbs_errors"]
    minimum_ber = events / bits if bits else float("nan")
    flags = " ".join(
        f"{name}={int(value)}" for name, value in counters["status"].items()
    )
    print(
        f"{elapsed:7.2f}s bits={bits:16d} prbs_error_indications={events:10d} "
        f"minimum_BER={minimum_ber:.3e} code={counters['code_errors']} "
        f"disp={counters['disparity_errors']} cdr_losses={counters['cdr_lock_losses']} "
        f"{flags}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--csr-csv", default="scripts/csr.csv")
    parser.add_argument("--mode", choices=tuple(PRBS_MODES)[1:], default="prbs31")
    parser.add_argument("--loopback", choices=LOOPBACK_MODES, default="near-pma")
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--inject-error", action="store_true",
                        help="inject one TX PRBS error after the checker settles")
    parser.add_argument("--leave-running", action="store_true")
    args = parser.parse_args()
    if args.duration <= 0 or args.interval <= 0:
        parser.error("--duration and --interval must be positive")

    with RemoteClient(
        host=args.host,
        port=args.port,
        csr_csv=args.csr_csv,
        timeout=3.0,
        raise_on_timeout=True,
    ) as bus:
        regs = bus.regs
        require_registers(regs)
        running = control_word(args.loopback, args.mode, args.mode)
        regs.eth_phy_prbs_control.write(running)
        time.sleep(0.05)
        regs.eth_phy_prbs_control.write(running | (1 << 13))
        time.sleep(0.05)

        start = time.monotonic()
        next_sample = start
        error_injected = False
        try:
            while True:
                now = time.monotonic()
                elapsed = now - start
                if (
                    args.inject_error
                    and not error_injected
                    and elapsed >= min(1.0, args.duration / 2)
                ):
                    regs.eth_phy_prbs_control.write(running | (1 << 12))
                    error_injected = True
                if now >= next_sample:
                    print_sample(elapsed, read_counters(regs))
                    next_sample += args.interval
                if elapsed >= args.duration:
                    break
                time.sleep(min(0.05, max(0.0, next_sample - now)))
        finally:
            if not args.leave_running:
                regs.eth_phy_prbs_control.write(control_word())

    print(
        "Note: RXPRBSERR is a per-word error indication; minimum_BER is not "
        "an exact bit-error count."
    )


if __name__ == "__main__":
    main()
