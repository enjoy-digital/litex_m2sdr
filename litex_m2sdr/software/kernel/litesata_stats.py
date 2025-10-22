#!/usr/bin/env python3

import argparse
import os
import time

DEFAULT_PATH = "/sys/kernel/debug/litesata"

FILES = [
    "io_cnt",
    "sectors",
    "ns_copy_out",
    "ns_copy_in",
    "ns_prog_dma",
    "ns_wait_dma",
]

def read_counters(base):
    vals = {}
    for f in FILES:
        p = os.path.join(base, f)
        with open(p, "r") as fh:
            s = fh.read().strip()
        vals[f] = int(s) if s else 0
    return vals

def fmt_mibps(bytes_delta, seconds):
    if seconds <= 0:
        return 0.0
    return (bytes_delta / (1024.0 * 1024.0)) / seconds

def main():
    ap = argparse.ArgumentParser(description="Live LiteSATA stats (debugfs).")
    ap.add_argument("--path", default=DEFAULT_PATH, help="debugfs dir (default: %(default)s)")
    ap.add_argument("-i", "--interval", type=float, default=0.5, help="poll interval seconds")
    ap.add_argument("--once", action="store_true", help="print one line and exit")
    args = ap.parse_args()

    if not os.path.isdir(args.path):
        print(f"error: {args.path} not found. Did you build with -DLITESATA_STATS and mount debugfs?")
        return 1

    prev = read_counters(args.path)
    prev_t = time.monotonic()

    print(" time   IO    MiB/s   copy_out%  copy_in%   prog%     wait%   (per-interval)")
    while True:
        time.sleep(args.interval)
        now = read_counters(args.path)
        now_t = time.monotonic()

        dt = max(now_t - prev_t, 1e-9)

        dsec   = now["sectors"]    - prev["sectors"]
        dio    = now["io_cnt"]     - prev["io_cnt"]
        dcout  = now["ns_copy_out"]- prev["ns_copy_out"]
        dcin   = now["ns_copy_in"] - prev["ns_copy_in"]
        dprog  = now["ns_prog_dma"]- prev["ns_prog_dma"]
        dwait  = now["ns_wait_dma"]- prev["ns_wait_dma"]

        bytes_moved = dsec * 512
        mibps = fmt_mibps(bytes_moved, dt)

        # time shares over the interval (nanoseconds)
        total_ns = max(dcout + dcin + dprog + dwait, 1)
        pct = lambda x: (100.0 * x / total_ns)

        print(f"{time.strftime('%H:%M:%S')}  "
              f"{dio:6d}  {mibps:7.1f}   "
              f"{pct(dcout):7.1f}   {pct(dcin):7.1f}   {pct(dprog):7.1f}   {pct(dwait):7.1f}")

        prev, prev_t = now, now_t
        if args.once:
            break

if __name__ == "__main__":
    raise SystemExit(main())
