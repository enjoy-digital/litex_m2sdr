#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse

from math import pi, cos, sin

# Helpers ------------------------------------------------------------------------------------------

def two_complement_encode(value, bits):
    if (value & (1 << (bits - 1))) != 0:
        value = value - (1 << bits)
    return value % (2**bits)

def insert_header_timestamp(f, header, timestamp):
    f.write(   header.to_bytes(8, byteorder="little"))
    f.write(timestamp.to_bytes(8, byteorder="little"))

# Tone Gen -----------------------------------------------------------------------------------------

def tone_gen(filename, nchannels, nbits, frequency, amplitude, samplerate, nsamples, frame_header, frame_size):
    assert amplitude >= 0.0
    assert amplitude <= 1.0
    if frame_header:
        assert frame_size%8 == 0 # 64-bit
    f = open(filename, "wb")
    omega     = 2*pi*frequency/samplerate
    phi       = 0
    header    = 0x5aa5_5aa5_5aa5_5aa5
    timestamp = 0
    for i in range(int(nsamples)):
        if frame_header and ((8*i)%frame_size == 0):
            insert_header_timestamp(f, header, timestamp)
        re = cos(phi) * amplitude * (2**(nbits - 1))
        im = sin(phi) * amplitude * (2**(nbits - 1))
        re = two_complement_encode(int(re), 16)
        im = two_complement_encode(int(im), 16)
        for j in range(nchannels):
            f.write(re.to_bytes(2, byteorder="little"))
            f.write(im.to_bytes(2, byteorder="little"))
        phi += omega
        if (phi >= pi):
            phi -= 2*pi
        timestamp += 1
    f.close()

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Tone Generator utility.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("filename", help="Ouptut filename.")
    parser.add_argument("--nchannels",    type=int,   default=2,             help="Number of RF channels.")
    parser.add_argument("--nbits",        type=int,   default=12,            help="Number of bits per Sample resolution (in bits per I/Q).")
    parser.add_argument("--frequency",    type=float, default=1e6,           help="Tone frequency.")
    parser.add_argument("--amplitude",    type=float, default=1,             help="Tone amplitude.")
    parser.add_argument("--samplerate",   type=float, default=30.72e6,       help="Sample Rate.")
    parser.add_argument("--nsamples",     type=float, default=1e3,           help="Number of samples.")
    parser.add_argument("--frame-header", action="store_true",               help="Inserter Frame Header.")
    parser.add_argument("--frame-size",   type=int,   default=30.726*8*1e-3, help="Frame Size (Used when Frame Header enabled).")
    args = parser.parse_args()

    tone_gen(
        filename     = args.filename,
        nchannels    = args.nchannels,
        nbits        = args.nbits,
        frequency    = args.frequency,
        amplitude    = args.amplitude,
        samplerate   = args.samplerate,
        nsamples     = int(args.nsamples),
        frame_header = args.frame_header,
        frame_size   = args.frame_size,
    )

if __name__ == "__main__":
    main()
