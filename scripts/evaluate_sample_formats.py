#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import csv
import math
import os
import sys


Q11_SCALE = 2048


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def q11_from_float(value):
    return clamp(int(round(value * Q11_SCALE)), -2048, 2047)


def arithmetic_shift_right(value, shift):
    if shift <= 0:
        return value
    if value >= 0:
        return value >> shift
    return -(((-value) + ((1 << shift) - 1)) >> shift)


def round_shift_signed(value, shift):
    if shift <= 0:
        return value
    offset = (1 << (shift - 1)) - 1 if value < 0 else 1 << (shift - 1)
    return arithmetic_shift_right(value + offset, shift)


def quantize_sc16(samples):
    return [q11_from_float(sample) for sample in samples]


def quantize_sc8_trunc(samples):
    decoded = []
    for sample in samples:
        q11 = q11_from_float(sample)
        mantissa = clamp(arithmetic_shift_right(q11, 4), -128, 127)
        decoded.append(mantissa << 4)
    return decoded


def quantize_sc8_round(samples):
    decoded = []
    for sample in samples:
        q11 = q11_from_float(sample)
        mantissa = clamp(round_shift_signed(q11, 4), -128, 127)
        decoded.append(mantissa << 4)
    return decoded


def bfp_exponent(max_abs, mantissa_bits, source_bits=12):
    if max_abs == 0:
        return 0
    max_mantissa = (1 << (mantissa_bits - 1)) - 1
    max_exponent = source_bits - mantissa_bits
    exponent = 0
    while exponent < max_exponent and max_abs > (max_mantissa << exponent):
        exponent += 1
    return exponent


def quantize_bfp(samples, mantissa_bits, block_components):
    decoded = []
    mantissa_min = -(1 << (mantissa_bits - 1))
    mantissa_max = (1 << (mantissa_bits - 1)) - 1

    q11_samples = [q11_from_float(sample) for sample in samples]
    for offset in range(0, len(q11_samples), block_components):
        block = q11_samples[offset:offset + block_components]
        exponent = bfp_exponent(max(abs(sample) for sample in block), mantissa_bits)
        for sample in block:
            mantissa = clamp(round_shift_signed(sample, exponent), mantissa_min, mantissa_max)
            decoded.append(mantissa << exponent)
    return decoded


def sine_samples(amplitude_dbfs, count, cycles):
    amplitude = 10 ** (amplitude_dbfs / 20.0)
    return [
        amplitude * math.sin((2.0 * math.pi * cycles * n) / count)
        for n in range(count)
    ]


def snr_db(reference, decoded_q11):
    signal_power = 0.0
    noise_power = 0.0
    for ref, decoded in zip(reference, decoded_q11):
        decoded_float = decoded / Q11_SCALE
        signal_power += ref * ref
        error = ref - decoded_float
        noise_power += error * error
    if noise_power == 0:
        return float("inf")
    return 10.0 * math.log10(signal_power / noise_power)


def bfp_bytes_per_complex(mantissa_bits, block_complex_samples, header_bytes, channels):
    payload_bytes = 2.0 * mantissa_bits / 8.0
    return payload_bytes + header_bytes / (block_complex_samples * channels)


def format_rows(args):
    amplitudes = args.amplitudes
    cycles = args.cycles
    count = args.samples
    block_components = args.block_complex_samples * 2

    formats = [
        ("SC16/Q11", 4.0, lambda samples: quantize_sc16(samples)),
        ("SC8 trunc", 2.0, lambda samples: quantize_sc8_trunc(samples)),
        ("SC8 rounded", 2.0, lambda samples: quantize_sc8_round(samples)),
        (
            "BFP8",
            bfp_bytes_per_complex(8, args.block_complex_samples, args.header_bytes, args.channels),
            lambda samples: quantize_bfp(samples, 8, block_components),
        ),
    ]

    rows = []
    references = {
        amplitude: sine_samples(amplitude, count, cycles)
        for amplitude in amplitudes
    }

    for name, bytes_per_complex, quantizer in formats:
        row = {
            "format": name,
            "bytes_per_complex": bytes_per_complex,
        }
        for amplitude in amplitudes:
            reference = references[amplitude]
            row[f"snr_{amplitude:g}dbfs"] = snr_db(reference, quantizer(reference))
        rows.append(row)

    baseline = {
        amplitude: rows[0][f"snr_{amplitude:g}dbfs"]
        for amplitude in amplitudes
    }
    for row in rows:
        for amplitude in amplitudes:
            key = f"loss_{amplitude:g}db"
            row[key] = baseline[amplitude] - row[f"snr_{amplitude:g}dbfs"]

    return rows


def print_markdown(rows, amplitudes):
    headers = ["Format", "B/complex"]
    headers += [f"SNR {amplitude:g} dBFS" for amplitude in amplitudes]
    headers += [f"Loss {amplitude:g} dB" for amplitude in amplitudes]

    print("| " + " | ".join(headers) + " |")
    print("|" + "|".join(["---", "---:"] + ["---:"] * (len(headers) - 2)) + "|")
    for row in rows:
        cells = [
            row["format"],
            f"{row['bytes_per_complex']:.3f}",
        ]
        cells += [f"{row[f'snr_{amplitude:g}dbfs']:.1f}" for amplitude in amplitudes]
        cells += [f"{row[f'loss_{amplitude:g}db']:.1f}" for amplitude in amplitudes]
        print("| " + " | ".join(cells) + " |")


def print_csv(rows, amplitudes):
    fields = ["format", "bytes_per_complex"]
    fields += [f"snr_{amplitude:g}dbfs" for amplitude in amplitudes]
    fields += [f"loss_{amplitude:g}db" for amplitude in amplitudes]
    writer = csv.DictWriter(sys.stdout, fieldnames=fields)
    writer.writeheader()
    for row in rows:
        writer.writerow(row)


def plot_rows(rows, amplitudes, output_path):
    mplconfig = os.environ.setdefault("MPLCONFIGDIR", "/tmp/litex_m2sdr_matplotlib")
    os.makedirs(mplconfig, exist_ok=True)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    plot_amplitudes = sorted(amplitudes)
    colors = {
        "SC16/Q11": "#4c78a8",
        "SC8 trunc": "#f58518",
        "SC8 rounded": "#54a24b",
        "BFP8": "#b279a2",
    }

    fig = plt.figure(figsize=(13.5, 7.2), constrained_layout=True)
    grid = fig.add_gridspec(2, 2, width_ratios=[1.45, 1.0])
    ax_snr = fig.add_subplot(grid[0, 0])
    ax_loss = fig.add_subplot(grid[1, 0], sharex=ax_snr)
    ax_bytes = fig.add_subplot(grid[:, 1])

    for row in rows:
        name = row["format"]
        snr = [row[f"snr_{amplitude:g}dbfs"] for amplitude in plot_amplitudes]
        loss = [row[f"loss_{amplitude:g}db"] for amplitude in plot_amplitudes]
        ax_snr.plot(plot_amplitudes, snr, marker="o", linewidth=2.0, label=name, color=colors.get(name))
        ax_loss.plot(plot_amplitudes, loss, marker="o", linewidth=2.0, label=name, color=colors.get(name))

    ax_snr.set_title("Quantized SNR vs Input Level")
    ax_snr.set_ylabel("SNR (dB)")
    ax_snr.grid(True, alpha=0.25)
    ax_snr.legend(loc="upper left", ncol=2, fontsize=9)

    ax_loss.set_title("SNR Loss vs SC16/Q11")
    ax_loss.set_xlabel("Tone amplitude (dBFS)")
    ax_loss.set_ylabel("Loss (dB)")
    ax_loss.grid(True, alpha=0.25)
    ax_loss.axhline(0.0, color="#333333", linewidth=0.8)

    names = [row["format"] for row in rows]
    bytes_per_complex = [row["bytes_per_complex"] for row in rows]
    baseline_bytes = bytes_per_complex[0]
    bar_colors = [colors.get(name, "#777777") for name in names]
    bars = ax_bytes.bar(names, bytes_per_complex, color=bar_colors)
    ax_bytes.set_title("Transport Bandwidth")
    ax_bytes.set_ylabel("Bytes / complex / channel")
    ax_bytes.set_ylim(0.0, baseline_bytes * 1.18)
    ax_bytes.grid(True, axis="y", alpha=0.25)
    ax_bytes.tick_params(axis="x", rotation=25)

    for bar, value in zip(bars, bytes_per_complex):
        pct = 100.0 * value / baseline_bytes
        ax_bytes.text(
            bar.get_x() + bar.get_width() / 2.0,
            value + baseline_bytes * 0.025,
            f"{value:.3f} B\n{pct:.1f}% of SC16",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    fig.suptitle("RFIC Transport Format Compression/Loss Model", fontsize=15, weight="bold")
    fig.savefig(output_path, dpi=160)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Evaluate SC16/SC8/BFP transport quantization loss."
    )
    parser.add_argument("--samples", type=int, default=65536, help="Sine sample count.")
    parser.add_argument("--cycles", type=float, default=123.45, help="Sine cycles in the window.")
    parser.add_argument(
        "--amplitudes",
        type=float,
        nargs="+",
        default=[0.0, -6.0, -20.0, -40.0],
        help="Tone amplitudes to test, in dBFS.",
    )
    parser.add_argument(
        "--block-complex-samples",
        type=int,
        default=254,
        help="Complex samples covered by one BFP exponent/header.",
    )
    parser.add_argument(
        "--header-bytes",
        type=int,
        default=8,
        help="BFP metadata/header bytes per block.",
    )
    parser.add_argument("--channels", type=int, default=2, help="Channels sharing one BFP block header.")
    parser.add_argument("--csv", action="store_true", help="Emit CSV instead of Markdown.")
    parser.add_argument("--plot", help="Write a PNG/SVG/PDF plot of compression and SNR loss.")
    args = parser.parse_args()

    if args.samples <= 0:
        parser.error("--samples must be positive")
    if args.block_complex_samples <= 0:
        parser.error("--block-complex-samples must be positive")
    if args.header_bytes < 0:
        parser.error("--header-bytes must be non-negative")
    if args.channels <= 0:
        parser.error("--channels must be positive")

    rows = format_rows(args)
    if args.csv:
        print_csv(rows, args.amplitudes)
    else:
        print_markdown(rows, args.amplitudes)
    if args.plot:
        plot_rows(rows, args.amplitudes, args.plot)
        print(f"Wrote plot to {args.plot}", file=sys.stderr)


if __name__ == "__main__":
    main()
