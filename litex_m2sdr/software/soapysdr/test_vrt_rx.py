#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: Apache-2.0

"""
test_vrt_rx.py - Validate Soapy RX over Ethernet VRT mode.

Checks:
- stream opens with eth_mode=vrt
- timestamps are present (optional)
- timestamps are monotonic
- timestamp delta is consistent with returned sample count (continuity proxy)
"""

import argparse
import math
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32, SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME


def main():
    p = argparse.ArgumentParser(
        description="Validate Soapy RX over LiteXM2SDR Ethernet VRT mode",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--eth-ip", default="192.168.1.50", help="Board Etherbone IP")
    p.add_argument("--vrt-port", type=int, default=4991, help="VRT UDP port")
    p.add_argument("--samplerate", type=float, default=30.72e6, help="RX sample rate (Hz)")
    p.add_argument("--bandwidth", type=float, default=30.72e6, help="RX bandwidth (Hz)")
    p.add_argument("--freq", type=float, default=2.4e9, help="RX LO frequency (Hz)")
    p.add_argument("--gain", type=float, default=20.0, help="RX gain (dB)")
    p.add_argument("--channel", type=int, default=0, choices=[0, 1], help="RX channel")
    p.add_argument("--format", default="CF32", choices=["CF32", "CS16"], help="Soapy stream format")
    p.add_argument("--chunks", type=int, default=200, help="Number of chunks to read")
    p.add_argument("--chunk-elems", type=int, default=4096, help="Elements to request per read")
    p.add_argument("--timeout-us", type=int, default=500000, help="readStream timeout (us)")
    p.add_argument("--ts-tol-ns", type=int, default=4000, help="Allowed timestamp delta error (ns)")
    p.add_argument("--allow-missing-ts", action="store_true", help="Do not fail if some chunks have no timestamp")
    args = p.parse_args()

    dev = SoapySDR.Device({
        "driver": "LiteXM2SDR",
        "eth_ip": args.eth_ip,
        "eth_mode": "vrt",
        "vrt_port": str(args.vrt_port),
    })

    dev.setSampleRate(SOAPY_SDR_RX, args.channel, args.samplerate)
    dev.setFrequency(SOAPY_SDR_RX, args.channel, args.freq)
    dev.setGain(SOAPY_SDR_RX, args.channel, args.gain)
    dev.setBandwidth(SOAPY_SDR_RX, args.channel, args.bandwidth)

    fmt = SOAPY_SDR_CF32 if args.format == "CF32" else SOAPY_SDR_CS16
    stream = dev.setupStream(SOAPY_SDR_RX, fmt, [args.channel])
    dev.activateStream(stream)

    if fmt == SOAPY_SDR_CF32:
        buf = np.empty(args.chunk_elems, np.complex64)
    else:
        buf = np.empty(args.chunk_elems * 2, np.int16)

    got = 0
    timeouts = 0
    ts_missing = 0
    ts_nonmono = 0
    ts_delta_bad = 0
    prev_ts = None
    prev_ret = None
    t0 = time.time()

    try:
        while got < args.chunks:
            sr = dev.readStream(stream, [buf], args.chunk_elems, timeoutUs=args.timeout_us)
            if sr.ret == 0:
                continue
            if sr.ret < 0:
                if sr.ret == -1:  # timeout in many Soapy bindings
                    timeouts += 1
                    continue
                print(f"readStream error: {sr.ret}")
                break

            got += 1
            has_ts = (sr.flags & SOAPY_SDR_HAS_TIME) != 0
            if not has_ts:
                ts_missing += 1
                print(f"chunk={got:04d} ret={sr.ret} no-ts")
                prev_ret = sr.ret
                continue

            ts = sr.timeNs
            msg = f"chunk={got:04d} ret={sr.ret} ts={ts}"

            if prev_ts is not None:
                if ts < prev_ts:
                    ts_nonmono += 1
                    msg += " NONMONO"
                expected = int(round(prev_ret * 1e9 / args.samplerate)) if prev_ret is not None else None
                if expected is not None:
                    dt = ts - prev_ts
                    err = dt - expected
                    msg += f" dt={dt}ns exp={expected}ns err={err:+d}ns"
                    if abs(err) > args.ts_tol_ns:
                        ts_delta_bad += 1
                        msg += " DELTA_BAD"
            print(msg)

            prev_ts = ts
            prev_ret = sr.ret

    finally:
        dev.deactivateStream(stream)
        dev.closeStream(stream)

    elapsed = time.time() - t0
    ok = True
    if ts_nonmono:
        ok = False
    if ts_delta_bad:
        ok = False
    if ts_missing and not args.allow_missing_ts:
        ok = False

    print(
        "summary:",
        f"chunks={got}",
        f"timeouts={timeouts}",
        f"ts_missing={ts_missing}",
        f"ts_nonmono={ts_nonmono}",
        f"ts_delta_bad={ts_delta_bad}",
        f"elapsed={elapsed:.2f}s",
    )
    if not ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
