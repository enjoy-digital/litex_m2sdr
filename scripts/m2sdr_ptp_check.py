#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import csv
import json
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path


PTP_FILTER = "udp port 319 or udp port 320"
TCPDUMP_MESSAGES = {
    "sync":       "msg type : sync msg",
    "follow_up":  "msg type : follow up msg",
    "delay_req":  "msg type : delay req msg",
    "delay_resp": "msg type : delay resp msg",
}
DISCIPLINE_COUNTERS = (
    "coarse_steps",
    "phase_steps",
    "rate_updates",
    "ptp_lock_losses",
    "time_lock_misses",
    "time_lock_losses",
)
CLOCK10_COUNTERS = (
    "sample_count",
    "reference_count",
    "clk10_count",
    "missing_count",
    "align_count",
    "lock_loss_count",
    "rate_update_count",
    "saturation_count",
)
ETHERBONE_PROCESS_SETTLE_SECONDS = 0.05


class CheckError(RuntimeError):
    pass


def repo_root():
    return Path(__file__).resolve().parents[1]


def default_m2sdr_util():
    return repo_root() / "litex_m2sdr" / "software" / "user" / "m2sdr_util"


def run_command(cmd, timeout=None, check=True):
    proc = subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        rendered = " ".join(shlex.quote(str(part)) for part in cmd)
        raise CheckError(
            f"{rendered} failed with exit code {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return proc


def util_cmd(args, *extra):
    cmd = [str(args.m2sdr_util)]
    if args.device:
        cmd += ["--device", args.device]
    else:
        cmd += ["--ip", args.ip, "--port", str(args.port)]
    cmd += list(extra)
    return cmd


def settle_after_util_process(args):
    # Direct LiteEth Etherbone uses a fixed UDP reply port and the packets do
    # not carry a transaction ID.  Leave a short gap between short-lived utility
    # processes so any late duplicate reply is dropped before the next process
    # binds the same local port.
    if not args.device:
        time.sleep(ETHERBONE_PROCESS_SETTLE_SECONDS)


def read_ptp_status(args):
    proc = run_command(util_cmd(args, "--json", "ptp-status"), timeout=args.command_timeout)
    settle_after_util_process(args)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise CheckError(f"m2sdr_util returned invalid JSON: {e}\n{proc.stdout}") from e


def read_clock10_status(args):
    proc = run_command(util_cmd(args, "--json", "ptp-clock10-status"), timeout=args.command_timeout)
    settle_after_util_process(args)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise CheckError(f"m2sdr_util returned invalid PTP clk10 JSON: {e}\n{proc.stdout}") from e


def counter_delta(newer, older):
    return (int(newer) - int(older)) & 0xffffffff


def signed_error_ns(status):
    raw = int(status["last_error_ns"])
    if "last_ptp_time_ns" in status and "last_local_time_ns" in status:
        derived = int(status["last_ptp_time_ns"]) - int(status["last_local_time_ns"])
        sign_extended_low = raw - (1 << 32)
        # Older CSR paths could expose a negative 32-bit error as zero-extended
        # unsigned data. Trust the corrected value only when timestamps prove it.
        if (1 << 31) <= raw <= ((1 << 32) - 1) and derived == sign_extended_low:
            return sign_extended_low
    return raw


def abs_error_ns(status):
    return abs(signed_error_ns(status))


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    if percent <= 0:
        return ordered[0]
    if percent >= 100:
        return ordered[-1]
    position = (len(ordered) - 1) * (percent / 100.0)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] + ((ordered[upper] - ordered[lower]) * fraction)


def numeric_stats(values):
    if not values:
        return {
            "count": 0,
            "min": None,
            "max": None,
            "mean": None,
            "stdev": None,
            "p50": None,
            "p95": None,
            "p99": None,
        }
    return {
        "count": len(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
    }


def sample_quality(samples, max_error_ns):
    return {
        "ptp_unlocked_count": sum(not sample.get("ptp_locked", False) for sample in samples),
        "time_unlocked_count": sum(not sample.get("time_locked", False) for sample in samples),
        "holdover_count": sum(sample.get("holdover", False) for sample in samples),
        "over_error_limit_count": sum(abs_error_ns(sample) > max_error_ns for sample in samples),
    }


def calibration_error_samples(samples, max_error_ns):
    # Calibration bias should use only samples where both the PTP source and
    # local TimeGenerator are locked; outliers would bake transient loss into
    # the suggested static compensation.
    return [
        signed_error_ns(sample)
        for sample in samples
        if (
            sample.get("ptp_locked", False)
            and sample.get("time_locked", False)
            and not sample.get("holdover", False)
            and abs_error_ns(sample) <= max_error_ns
        )
    ]


def evaluate_clock10_status(status, require_lock=False):
    failures = []
    if not status.get("reference_locked", False):
        failures.append("PTP clk10 reference is not locked")
    if require_lock and not status.get("clock_locked", False):
        failures.append("PTP clk10 marker is not locked")
    if status.get("missing_count", 0) != 0:
        failures.append("PTP clk10 phase detector has missing windows")
    return failures


def clock10_report(first, samples):
    if not samples:
        return None
    last = samples[-1]
    # Keep clk10 discipline metrics separate from board-time metrics; the two
    # loops share the PTP reference but have independent lock and rate counters.
    signed_errors = [int(sample.get("last_error_ns", 0)) for sample in samples]
    abs_errors = [abs(value) for value in signed_errors]
    return {
        "samples": {
            "count": len(samples),
            "first_refresh_time_unix": samples[0].get("refresh_time_unix"),
            "last_refresh_time_unix": last.get("refresh_time_unix"),
        },
        "lock": {
            "enabled": last.get("enabled", False),
            "active": last.get("active", False),
            "reference_locked": last.get("reference_locked", False),
            "clock_locked": last.get("clock_locked", False),
            "holdover": last.get("holdover", False),
            "aligned": last.get("aligned", False),
            "rate_limited": last.get("rate_limited", False),
        },
        "error_ns": {
            "signed": numeric_stats(signed_errors),
            "absolute": numeric_stats(abs_errors),
            "last": signed_errors[-1],
        },
        "counter_deltas": optional_counter_deltas(first, last, CLOCK10_COUNTERS),
        "status": last,
    }


def evaluate_status(status, max_error_ns):
    failures = []
    if not status.get("enabled", False):
        failures.append("PTP discipline is disabled")
    if not status.get("active", False):
        failures.append("PTP does not own the board time")
    if not status.get("ptp_locked", False):
        failures.append("PTP servo is not locked")
    if not status.get("time_locked", False):
        failures.append("board time is not locked")
    if status.get("holdover", False):
        failures.append("board is in holdover")
    error_ns = signed_error_ns(status)
    if abs(error_ns) > max_error_ns:
        failures.append(f"last error {error_ns} ns exceeds {max_error_ns} ns")
    return failures


def optional_counter_deltas(first, last, names):
    return {
        name: counter_delta(last[name], first[name])
        for name in names
        if name in first and name in last
    }


def evaluate_counter_deltas(first, last):
    return [], optional_counter_deltas(first, last, DISCIPLINE_COUNTERS)


def run_ping(args):
    target = args.device if args.device else args.ip
    proc = run_command(
        ["ping", "-c", str(args.ping_count), "-W", str(args.ping_timeout), target],
        timeout=args.ping_count * (args.ping_timeout + 1) + 2,
    )
    print(proc.stdout.strip())


def analyze_tcpdump_output(output):
    # A useful protocol capture must show both event and general PTP traffic;
    # seeing only Sync packets can still mean delay-response traffic is filtered.
    present = {
        name: (needle in output)
        for name, needle in TCPDUMP_MESSAGES.items()
    }
    missing = [name for name, ok in present.items() if not ok]
    return present, missing


def run_tcpdump(args):
    if not args.iface:
        return None
    cmd = [
        "tcpdump",
        "-i", args.iface,
        "-nn",
        "-e",
        "-ttt",
        "-vv",
        "-c", str(args.tcpdump_count),
        PTP_FILTER,
    ]
    try:
        proc = run_command(cmd, timeout=args.tcpdump_timeout, check=False)
    except subprocess.TimeoutExpired as e:
        output = (e.stdout or "") + "\n" + (e.stderr or "")
        present, missing = analyze_tcpdump_output(output)
        if missing:
            raise CheckError(
                f"tcpdump timed out before seeing all PTP message types; missing {missing}\n"
                f"{output}"
            ) from e
        return present

    output = proc.stdout + "\n" + proc.stderr
    if proc.returncode not in (0, 124):
        raise CheckError(f"tcpdump failed with exit code {proc.returncode}\n{output}")
    present, missing = analyze_tcpdump_output(output)
    if missing:
        raise CheckError(f"tcpdump did not see PTP message types {missing}\n{output}")
    print(output.strip())
    return present


def run_board_smoke(args):
    run_command(
        util_cmd(
            args,
            "--duration", str(args.duration),
            "--watch-interval", str(args.interval),
            "--max-error-ns", str(args.max_error_ns),
            "ptp-smoke",
        ),
        timeout=args.duration + args.command_timeout + 5,
    )
    settle_after_util_process(args)


def build_report(command, args, first, samples, discipline_deltas, tcpdump_seen, failures, extra=None, clock10=None):
    last = samples[-1]
    signed_errors = [signed_error_ns(sample) for sample in samples]
    abs_errors = [abs(value) for value in signed_errors]
    report = {
        "command": command,
        "target": {
            "device": args.device,
            "ip": args.ip,
            "port": args.port,
            "iface": args.iface,
        },
        "limits": {
            "max_error_ns": args.max_error_ns,
        },
        "samples": {
            "count": len(samples),
            "first_refresh_time_unix": samples[0].get("refresh_time_unix"),
            "last_refresh_time_unix": last.get("refresh_time_unix"),
        },
        "lock": {
            "enabled": last["enabled"],
            "active": last["active"],
            "ptp_locked": last["ptp_locked"],
            "time_locked": last["time_locked"],
            "holdover": last["holdover"],
            "state": last["state"],
            "state_name": last["state_name"],
        },
        "master": {
            "ip": last["master_ip"],
            "port": last["master_port"]["identity"],
        },
        "error_ns": {
            "signed": numeric_stats(signed_errors),
            "absolute": numeric_stats(abs_errors),
            "last": signed_error_ns(last),
        },
        "sample_quality": sample_quality(samples, args.max_error_ns),
        "counter_deltas": {
            "discipline": discipline_deltas,
        },
        "tcpdump": tcpdump_seen,
        "result": "FAIL" if failures else "PASS",
        "failures": sorted(set(failures)),
        "status": last,
    }
    if clock10 is not None:
        report["clock10"] = clock10
    if extra:
        report.update(extra)
    return report


def write_json_report(path, report, samples, clock10_samples=None):
    if not path:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = dict(report)
    payload["raw_samples"] = samples
    if clock10_samples is not None:
        payload["raw_clock10_samples"] = clock10_samples
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_csv_samples(path, samples):
    if not path:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "refresh_time_unix",
        "state_name",
        "ptp_locked",
        "time_locked",
        "holdover",
        "last_error_ns",
        "ptp_lock_losses",
        "time_lock_misses",
        "time_lock_miss_count",
        "time_lock_losses",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for sample in samples:
            writer.writerow({field: sample.get(field) for field in fields})


def write_clock10_csv_samples(path, samples):
    if not path or not samples:
        return
    # Use a sidecar CSV so existing PTP time-discipline analysis keeps a stable schema.
    clock10_path = path.with_name(path.stem + ".clock10" + path.suffix)
    fields = [
        "refresh_time_unix",
        "enabled",
        "active",
        "reference_locked",
        "clock_locked",
        "holdover",
        "aligned",
        "rate_limited",
        "last_error_ns",
        "last_error_ticks",
        "last_rate",
        "sample_count",
        "missing_count",
        "lock_loss_count",
        "rate_update_count",
        "saturation_count",
    ]
    with clock10_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for sample in samples:
            writer.writerow({field: sample.get(field) for field in fields})


def smoke(args):
    if args.ping:
        run_ping(args)

    first = read_ptp_status(args)
    clock10_first = read_clock10_status(args) if args.with_clock10 else None
    run_board_smoke(args)
    last = read_ptp_status(args)
    clock10_samples = [read_clock10_status(args)] if args.with_clock10 else []

    failures = evaluate_status(last, args.max_error_ns)
    counter_failures, discipline_deltas = evaluate_counter_deltas(first, last)
    failures.extend(counter_failures)

    tcpdump_seen = run_tcpdump(args) if args.iface else None
    if args.with_clock10:
        failures.extend(evaluate_clock10_status(clock10_samples[-1], args.require_clock10_lock))

    samples = [last]
    report = build_report(
        "smoke",
        args,
        first,
        samples,
        discipline_deltas,
        tcpdump_seen,
        failures,
        clock10=clock10_report(clock10_first, clock10_samples) if args.with_clock10 else None,
    )
    write_json_report(args.json_out, report, samples, clock10_samples)
    write_csv_samples(args.csv_out, samples)
    write_clock10_csv_samples(args.csv_out, clock10_samples)
    print_summary(report)
    if failures:
        raise CheckError("; ".join(failures))


def soak(args):
    if args.duration <= 0:
        raise CheckError("soak duration must be greater than 0")
    if args.interval <= 0:
        raise CheckError("soak interval must be greater than 0")

    first = read_ptp_status(args)
    clock10_first = read_clock10_status(args) if args.with_clock10 else None
    samples = []
    clock10_samples = []
    failures = []
    deadline = time.monotonic() + args.duration

    while True:
        status = read_ptp_status(args)
        samples.append(status)
        failures.extend(evaluate_status(status, args.max_error_ns))
        if args.with_clock10:
            clock10_status = read_clock10_status(args)
            clock10_samples.append(clock10_status)
            failures.extend(evaluate_clock10_status(clock10_status, args.require_clock10_lock))
        if time.monotonic() >= deadline:
            break
        time.sleep(args.interval)

    last = samples[-1]
    counter_failures, discipline_deltas = evaluate_counter_deltas(first, last)
    failures.extend(counter_failures)
    report = build_report(
        "soak",
        args,
        first,
        samples,
        discipline_deltas,
        None,
        failures,
        clock10=clock10_report(clock10_first, clock10_samples) if args.with_clock10 else None,
    )
    write_json_report(args.json_out, report, samples, clock10_samples)
    write_csv_samples(args.csv_out, samples)
    write_clock10_csv_samples(args.csv_out, clock10_samples)
    print_summary(report)

    if failures:
        unique_failures = sorted(set(failures))
        raise CheckError("; ".join(unique_failures))


def calibrate(args):
    if args.duration <= 0:
        raise CheckError("calibration duration must be greater than 0")
    if args.interval <= 0:
        raise CheckError("calibration interval must be greater than 0")

    first = read_ptp_status(args)
    clock10_first = read_clock10_status(args) if args.with_clock10 else None
    samples = []
    clock10_samples = []
    failures = []
    deadline = time.monotonic() + args.duration

    while True:
        status = read_ptp_status(args)
        samples.append(status)
        failures.extend(evaluate_status(status, args.max_error_ns))
        if args.with_clock10:
            clock10_status = read_clock10_status(args)
            clock10_samples.append(clock10_status)
            failures.extend(evaluate_clock10_status(clock10_status, args.require_clock10_lock))
        if time.monotonic() >= deadline:
            break
        time.sleep(args.interval)

    last = samples[-1]
    counter_failures, discipline_deltas = evaluate_counter_deltas(first, last)
    failures.extend(counter_failures)

    signed_errors = calibration_error_samples(samples, args.max_error_ns)
    reference_offset = args.reference_offset_ns if args.reference_offset_ns is not None else 0.0
    observed_bias = percentile(signed_errors, 50)
    suggested_compensation = None if observed_bias is None else reference_offset - observed_bias
    if not signed_errors:
        failures.append("no locked in-limit samples for calibration")
    extra = {
        "calibration": {
            "samples_used": len(signed_errors),
            "reference_offset_ns": reference_offset,
            "observed_signed_error_median_ns": observed_bias,
            "suggested_compensation_ns": suggested_compensation,
            "external_reference_required": args.reference_offset_ns is None,
        },
    }
    report = build_report(
        "calibrate",
        args,
        first,
        samples,
        discipline_deltas,
        None,
        failures,
        extra=extra,
        clock10=clock10_report(clock10_first, clock10_samples) if args.with_clock10 else None,
    )
    write_json_report(args.json_out, report, samples, clock10_samples)
    write_csv_samples(args.csv_out, samples)
    write_clock10_csv_samples(args.csv_out, clock10_samples)
    print_summary(report)
    print("Calibration")
    print("-----------")
    print(f"Samples Used     : {len(signed_errors)}")
    print(f"Reference Offset : {reference_offset:.1f} ns")
    if observed_bias is not None:
        print(f"Median Error     : {observed_bias:.1f} ns")
        print(f"Suggested Comp.  : {suggested_compensation:.1f} ns")
    else:
        print("Median Error     : n/a")
        print("Suggested Comp.  : n/a")
    if args.reference_offset_ns is None:
        print(
            "Note             : no external reference offset was supplied; "
            "this is residual servo bias, not an absolute PHY/PPS calibration."
        )

    if failures:
        unique_failures = sorted(set(failures))
        raise CheckError("; ".join(unique_failures))


def print_summary(report):
    signed_stats = report["error_ns"]["signed"]
    abs_stats = report["error_ns"]["absolute"]
    tcpdump_seen = report["tcpdump"]

    print("PTP Check Summary")
    print("-----------------")
    print(f"Samples          : {report['samples']['count']}")
    print(f"State            : {report['lock']['state_name']} ({report['lock']['state']})")
    print(f"PTP Locked       : {report['lock']['ptp_locked']}")
    print(f"Time Locked      : {report['lock']['time_locked']}")
    print(f"Holdover         : {report['lock']['holdover']}")
    print(f"Master IP        : {report['master']['ip']}")
    print(f"Master Port      : {report['master']['port']}")
    print(f"Last Error       : {report['error_ns']['last']} ns")
    print(f"Signed Error Min : {signed_stats['min']} ns")
    print(f"Signed Error Max : {signed_stats['max']} ns")
    print(f"Signed Error Mean: {signed_stats['mean']:.1f} ns")
    print(f"Signed Error P95 : {signed_stats['p95']:.1f} ns")
    print(f"Signed Error P99 : {signed_stats['p99']:.1f} ns")
    print(f"Abs Error Max    : {abs_stats['max']} ns")
    print(f"Abs Error Mean   : {abs_stats['mean']:.1f} ns")
    print(f"Abs Error P95    : {abs_stats['p95']:.1f} ns")
    print(f"Abs Error P99    : {abs_stats['p99']:.1f} ns")
    sample_quality = report["sample_quality"]
    print(
        "Sample Failures  : "
        f"ptp={sample_quality['ptp_unlocked_count']}, "
        f"time={sample_quality['time_unlocked_count']}, "
        f"holdover={sample_quality['holdover_count']}, "
        f"error={sample_quality['over_error_limit_count']}"
    )
    status = report["status"]
    if "time_lock_misses" in status:
        discipline = report["counter_deltas"].get("discipline", {})
        print(f"Time Lock Misses : {status['time_lock_misses']} (+{discipline.get('time_lock_misses', 0)})")
        print(f"Time Miss Count  : {status.get('time_lock_miss_count', 0)}")
        print(f"Time Lock Losses : {status.get('time_lock_losses', 0)} (+{discipline.get('time_lock_losses', 0)})")
        if "coarse_steps" in discipline:
            print(f"Coarse Steps     : {status.get('coarse_steps', 0)} (+{discipline['coarse_steps']})")
    clock10 = report.get("clock10")
    if clock10:
        clock10_abs = clock10["error_ns"]["absolute"]
        clock10_deltas = clock10["counter_deltas"]
        print("PTP Clk10")
        print(f"Clk10 Ref Locked : {clock10['lock']['reference_locked']}")
        print(f"Clk10 Locked     : {clock10['lock']['clock_locked']}")
        print(f"Clk10 Last Error : {clock10['error_ns']['last']} ns")
        print(f"Clk10 Abs Max    : {clock10_abs['max']} ns")
        print(f"Clk10 Abs P95    : {clock10_abs['p95']:.1f} ns")
        print(f"Clk10 Samples    : {clock10['samples']['count']}")
        print(f"Clk10 Missing    : +{clock10_deltas.get('missing_count', 0)}")
        print(f"Clk10 Saturations: +{clock10_deltas.get('saturation_count', 0)}")
    if tcpdump_seen is not None:
        seen = ", ".join(name for name, ok in tcpdump_seen.items() if ok)
        print(f"tcpdump PTP Msgs : {seen}")
    print(f"Result           : {report['result']}")


def add_common_args(parser):
    parser.add_argument("--m2sdr-util",      type=Path,  default=default_m2sdr_util(), help="m2sdr_util path")
    parser.add_argument(
        "--device",
        default=None,
        help="libm2sdr device string, such as eth:192.168.1.50:1234",
    )
    parser.add_argument("--ip",              default="192.168.1.50",                   help="Etherbone IP address")
    parser.add_argument("--port",            type=int,   default=1234,                 help="Etherbone UDP port")
    parser.add_argument(
        "--iface",
        default=None,
        help="host interface for optional tcpdump validation",
    )
    parser.add_argument("--duration",        type=int,   default=10,                   help="test duration in seconds")
    parser.add_argument("--interval",        type=float, default=1.0,                  help="poll interval in seconds")
    parser.add_argument("--max-error-ns",    type=int,   default=1000000,              help="maximum allowed absolute time error")
    parser.add_argument("--command-timeout", type=int,   default=5,                    help="m2sdr_util command timeout in seconds")
    parser.add_argument("--no-ping",         dest="ping", action="store_false",        help="skip board ping")
    parser.add_argument("--ping-count",      type=int,   default=3,                    help="ping packet count")
    parser.add_argument("--ping-timeout",    type=int,   default=1,                    help="ping per-packet timeout")
    parser.add_argument("--tcpdump-count",   type=int,   default=20,                   help="PTP packets to capture when --iface is set")
    parser.add_argument("--tcpdump-timeout", type=int,   default=12,                   help="tcpdump timeout in seconds")
    parser.add_argument("--json-out",        type=Path,  default=None,                 help="write full JSON report and raw samples")
    parser.add_argument("--csv-out",         type=Path,  default=None,                 help="write sampled status rows as CSV")
    parser.add_argument("--with-clock10",    action="store_true",                     help="also sample/report ptp-clock10-status")
    parser.add_argument(
        "--require-clock10-lock",
        action="store_true",
        help="fail when --with-clock10 is set and the clk10 marker is not locked",
    )
    parser.set_defaults(ping=True)


def main():
    parser = argparse.ArgumentParser(
        description="Validate LiteX-M2SDR Ethernet PTP lock and packet exchange.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    smoke_parser = subparsers.add_parser("smoke", help="short board/packet PTP validation")
    add_common_args(smoke_parser)
    smoke_parser.set_defaults(func=smoke)

    soak_parser = subparsers.add_parser("soak", help="longer PTP lock/error soak")
    add_common_args(soak_parser)
    soak_parser.set_defaults(func=soak)

    calibrate_parser = subparsers.add_parser(
        "calibrate",
        help="collect signed-error statistics for offset calibration",
    )
    add_common_args(calibrate_parser)
    calibrate_parser.add_argument(
        "--reference-offset-ns",
        type=float,
        default=None,
        help="external measured board-vs-reference offset; without it, output is residual servo bias only",
    )
    calibrate_parser.set_defaults(func=calibrate)

    args = parser.parse_args()

    try:
        args.func(args)
    except (CheckError, subprocess.TimeoutExpired) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
