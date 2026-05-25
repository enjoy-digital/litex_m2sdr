#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import csv
import math
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


def bfp_bytes_per_complex(mantissa_bits, block_complex_samples, header_bytes):
    payload_bytes = 2.0 * mantissa_bits / 8.0
    return payload_bytes + header_bytes / block_complex_samples


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
            bfp_bytes_per_complex(8, args.block_complex_samples, args.header_bytes),
            lambda samples: quantize_bfp(samples, 8, block_components),
        ),
        (
            "BFP6",
            bfp_bytes_per_complex(6, args.block_complex_samples, args.header_bytes),
            lambda samples: quantize_bfp(samples, 6, block_components),
        ),
        (
            "BFP4",
            bfp_bytes_per_complex(4, args.block_complex_samples, args.header_bytes),
            lambda samples: quantize_bfp(samples, 4, block_components),
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
        default=256,
        help="Complex samples covered by one BFP exponent/header.",
    )
    parser.add_argument(
        "--header-bytes",
        type=int,
        default=8,
        help="BFP metadata/header bytes per block.",
    )
    parser.add_argument("--csv", action="store_true", help="Emit CSV instead of Markdown.")
    args = parser.parse_args()

    if args.samples <= 0:
        parser.error("--samples must be positive")
    if args.block_complex_samples <= 0:
        parser.error("--block-complex-samples must be positive")
    if args.header_bytes < 0:
        parser.error("--header-bytes must be non-negative")

    rows = format_rows(args)
    if args.csv:
        print_csv(rows, args.amplitudes)
    else:
        print_markdown(rows, args.amplitudes)


if __name__ == "__main__":
    main()
