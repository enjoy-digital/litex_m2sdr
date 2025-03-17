#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import numpy as np
from math import pi, cos, sin

# Helpers ------------------------------------------------------------------------------------------

def two_complement_decode(value, bits):
    if value >= (1 << (bits - 1)):
        value -= (1 << (bits))
    return value

def extract_header_timestamp(f):
    header    = int.from_bytes(f.read(8), "little")
    timestamp = int.from_bytes(f.read(8), "little")
    return header, timestamp

def calculate_rms(samples):
    return np.sqrt(np.mean(np.square(samples)))

# Tone Check ---------------------------------------------------------------------------------------

def tone_check(filename, nchannels, nbits, samplerate, frame_header, frame_size, plot):
    if frame_header:
        assert frame_size%8 == 0 # 64-bit
    # Extract samples from file.
    f = open(filename, "rb")
    re = [[] for _ in range(nchannels)]
    im = [[] for _ in range(nchannels)]
    i = 0
    while True:
        done = False
        if frame_header and ((8*i)%frame_size == 0):
            header, timestamp = extract_header_timestamp(f)
        for j in range(nchannels):
            _bytes = f.read(4)
            if _bytes == b"":
                done = True
                break
            _re = int.from_bytes(_bytes[0:2], "little")
            _im = int.from_bytes(_bytes[2:4], "little")
            re[j].append(two_complement_decode(_re, 16))
            im[j].append(two_complement_decode(_im, 16))
        i += 1
        if done:
            break
    f.close()

    # Calculate and print RMS values
    for j in range(nchannels):
        rms_re = calculate_rms(re[j])
        rms_im = calculate_rms(im[j])
        print(f"RMS of Re{j}: {rms_re}")
        print(f"RMS of Im{j}: {rms_im}")

    # Plot Channel samples.
    if plot:
        import matplotlib.pyplot as plt
        for j in range(nchannels):
            plt.plot(re[j])
            plt.plot(im[j])
        plt.show()

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Tone Checker utility.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("filename", help="Ouptut filename.")
    parser.add_argument("--nchannels",    type=int,   default=2,               help="Number of RF channels.")
    parser.add_argument("--nbits",        type=int,   default=12,              help="Number of bits per Sample resolution (in bits per I/Q).")
    parser.add_argument("--samplerate",   type=float, default=30.72e6,         help="Sample Rate.")
    parser.add_argument("--frame-header", action="store_true",                 help="Extract Frame Header.")
    parser.add_argument("--frame-size",   type=int,   default=30.72e6*8*1e-3,  help="Frame Size default 1ms (Used when Frame Header enabled).")
    parser.add_argument("--plot",         action= "store_true",                help="Enable Plot.")
    args = parser.parse_args()

    tone_check(
        filename     = args.filename,
        nchannels    = args.nchannels,
        nbits        = args.nbits,
        samplerate   = args.samplerate,
        frame_header = args.frame_header,
        frame_size   = args.frame_size,
        plot         = args.plot,
    )

if __name__ == "__main__":
    main()
