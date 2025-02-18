#!/usr/bin/env python3

"""
test_play.py - Transmit I/Q samples using the LiteXM2SDR SoapySDR driver.

This script uses the SoapySDR driver to interface with the LiteXM2SDR hardware.
It can either generate a continuous tone or play back a file of raw CF32 samples.
Command-line options allow you to configure sample rate, bandwidth, frequency, and gain.
In file playback mode, the file can be looped a specified number of times.

Usage Examples:

Tone Generation:
    ./test_play.py --samplerate 30.72e6 --bandwidth 56e6 --freq 2.4e9 --gain -20 --tone-freq 1e6 --ampl 0.8 --secs 5

File Playback:
    ./test_play.py --samplerate 30.72e6 --bandwidth 56e6 --freq 2.4e9 --gain -20 path/to/file.bin 10
"""

import time
import argparse
import numpy as np

import SoapySDR
from SoapySDR import SOAPY_SDR_TX, SOAPY_SDR_CF32

# Generate Tone ------------------------------------------------------------------------------------

def generate_tone(freq_hz, sample_rate, amplitude=0.7, length=2048):
    t    = np.arange(length, dtype=np.float32) / sample_rate
    tone = amplitude * np.exp(1j * 2.0 * np.pi * freq_hz * t)
    return tone.astype(np.complex64)

# Read File (CF32) ---------------------------------------------------------------------------------

def read_file_cf32(path, amplitude=0.7, chunk_len=1024):
    with open(path, "rb") as f:
        while True:
            # 8 bytes per complex64 sample (I and Q).
            data = f.read(chunk_len * 8)
            if not data:
                break
            buf = np.frombuffer(data, dtype=np.complex64)
            yield buf * amplitude

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Transmit I/Q samples using the LiteXM2SDR SoapySDR driver.")
    # RF configuration options.
    parser.add_argument("--samplerate", type=float, default=30.72e6, help="TX Sample rate in Hz (default: 30720000)")
    parser.add_argument("--bandwidth",  type=float, default=56e6,    help="TX Filter bandwidth in Hz (default: 56000000)")
    parser.add_argument("--freq",       type=float, default=2.4e9,   help="TX frequency in Hz (default: 2400000000)")
    parser.add_argument("--gain",       type=float, default=-20.0,   help="TX gain in dB (default: -20)")

    # Transmission mode: tone generation or file playback.
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--tone-freq",   type=float, help="Generate a tone at this frequency in Hz")
    group.add_argument("--filename",    nargs="?",  help="Path to a raw CF32 file to play (file mode)")

    # Additional options.
    parser.add_argument("--ampl", type=float, default=0.8,            help="Amplitude (0..1) (default: 0.7)")
    parser.add_argument("--secs", type=float, default=5.0,            help="Transmit duration in seconds (tone mode only, default: 5)")
    parser.add_argument("loops",  type=int,   nargs="?",   default=1, help="Number of times to loop file playback (file mode only, default: 1)")

    args = parser.parse_args()

    # Open the LiteXM2SDR device using the SoapySDR driver.
    sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})

    # Basic RF configuration
    sdr.setSampleRate(SOAPY_SDR_TX, 0, args.samplerate)
    sdr.setFrequency( SOAPY_SDR_TX, 0, args.freq)
    sdr.setGain(      SOAPY_SDR_TX, 0, args.gain)
    sdr.setBandwidth( SOAPY_SDR_TX, 0, args.bandwidth)

    # Determine the data source based on mode.
    if args.tone_freq is not None:
        tone_buf = generate_tone(args.tone_freq, args.samplerate, args.ampl)
        def get_samples():
            while True:
                yield tone_buf
        mode = "tone"
    else:
        def get_samples():
            for _ in range(args.loops):
                for chunk in read_file_cf32(args.filename, args.ampl):
                    yield chunk
        mode = "file"

    # Create and activate TX stream.
    tx_stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0])
    sdr.activateStream(tx_stream)

    # Play Tone.
    if mode == "tone":
        print(f"Playing tone for {args.secs} seconds at {args.freq/1e6:.3f} MHz...")
        t_start = time.time()
        for samples in get_samples():
            if time.time() - t_start > args.secs:
                break
            sr = sdr.writeStream(tx_stream, [samples], len(samples))
            if sr.ret < 0:
                print(f"writeStream error: {sr.ret}")
                break

    # Play File.
    else:
        print(f"Playing file '{args.filename}' for {args.loops} loop(s) at {args.freq/1e6:.3f} MHz...")
        for samples in get_samples():
            sr = sdr.writeStream(tx_stream, [samples], len(samples))
            if sr.ret < 0:
                print(f"writeStream error: {sr.ret}")
                break

    # Deactivate TX stream and close.
    sdr.deactivateStream(tx_stream)
    sdr.closeStream(tx_stream)
    print("Done.")

if __name__ == "__main__":
    main()
