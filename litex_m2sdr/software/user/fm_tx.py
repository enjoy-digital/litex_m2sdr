#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""
fm_tx.py - FM modulate MP3/WAV audio to I/Q samples.

This script decodes an MP3/WAV file, FM-modulates it, and writes interleaved 16-bit I/Q samples
to a file or stdout. It uses a sine table for efficient modulation and supports configurable
sample rate, deviation, and bit resolution.

Usage Example:
    ./fm_tx.py input.mp3 output.bin --rate 500000 --dev 75000 --bits 12

    ./m2sdr_rf -samplerate 1e6 -tx_freq 100e6 -tx_gain -10 -chan 1t1r
    ./fm_tx.py --rate=1e6 music.mp3 - --bits 12 | ./m2sdr_play -
"""

import argparse
import math
import array
import sys
from pydub import AudioSegment

# Helpers ------------------------------------------------------------------------------------------

def two_complement_encode(value, bits):
    """Encode value in two's complement representation."""
    if value & (1 << (bits - 1)):
        value -= (1 << bits)
    return value % (2**bits)

# FM Modulator -------------------------------------------------------------------------------------

def mp3_to_fm_iq(filename_in, filename_out, samplerate, deviation, bits):
    """Decode MP3/WAV, FM-modulate, and write interleaved 16-bit I/Q samples."""
    # Decode and resample to fs, mono
    samplerate = int(samplerate)
    seg        = AudioSegment.from_file(filename_in).set_channels(1).set_frame_rate(samplerate)
    samp       = seg.get_array_of_samples()  # array('h') 16-bit signed

    # Compute normalization factor
    max_abs = max(abs(min(samp)), max(samp)) or 1
    scale   = (1 << (bits - 1)) - 1

    # Prepare constants
    N          = 4096
    sine_table = array.array('h', [int(math.sin(i * 2 * math.pi / N) * scale) for i in range(N)])
    shift      = 32
    multiplier = int((deviation * N * (1 << shift)) / samplerate)

    # Determine output stream
    f      = sys.stdout.buffer if filename_out == "-" else open(filename_out, "wb")
    buffer = array.array('h')

    # Process samples with initial buffering
    phase_int      = 0
    initial_buffer = 100  # Larger initial buffer to reduce underflows (50,000 I/Q pairs)
    for sample in samp:
        phase_increment = (sample * multiplier) // (max_abs * (1 << shift))
        phase_int       = (phase_int + phase_increment) % N
        index           = phase_int
        i_val           = sine_table[(index + N//4) % N]
        q_val           = sine_table[index]
        buffer.append(i_val)
        buffer.append(q_val)
        if len(buffer) >= initial_buffer * 2:  # Flush larger initial buffer
            f.write(buffer.tobytes())
            if filename_out == "-":
                sys.stdout.flush()
            buffer        = array.array('h')
            initial_buffer = 100 # Revert to original buffer size after initial flush

    # Write remaining buffer
    if buffer:
        f.write(buffer.tobytes())
        if filename_out == "-":
            sys.stdout.flush()

    if filename_out != "-":
        f.close()
    print(f"✓ wrote {'stdout' if filename_out == '-' else filename_out}")

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description     = "FM modulate MP3/WAV audio to I/Q samples.",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("input", help="Input MP3/WAV file")
    parser.add_argument("output", help="Output file for I/Q samples ('-' for stdout)")
    parser.add_argument("--rate", type=float, default=500000, help="Sample rate in Hz")
    parser.add_argument("--dev", type=float, default=75000, help="FM deviation in Hz")
    parser.add_argument("--bits", type=int, default=12, help="Bits per I/Q sample (≤16)")

    args = parser.parse_args()
    if args.bits > 16:
        print("Error: Bits per sample must be <= 16", file=sys.stderr)
        sys.exit(1)

    try:
        mp3_to_fm_iq(
            filename_in  = args.input,
            filename_out = args.output,
            samplerate   = args.rate,
            deviation    = args.dev,
            bits         = args.bits
        )
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()