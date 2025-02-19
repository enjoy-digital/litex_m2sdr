#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""
test_record.py - Record I/Q samples using the LiteXM2SDR SoapySDR driver.

This script uses the SoapySDR driver to interface with the LiteXM2SDR hardware. It records I/Q
samples for a specified duration and writes raw CF32 samples to a file. It optionally checks and
prints timestamp information along with differences between consecutive valid timestamps.
Command-line options allow you to configure sample rate, bandwidth, frequency, gain, and channel.

Usage Example:
    ./test_record.py --samplerate 4e6 --bandwidth 56e6 --freq 2.4e9 --gain 20 --channel 0 --secs 5 --check-ts filename.bin
"""

import time
import argparse
import numpy as np

import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

# Constants ----------------------------------------------------------------------------------------

DMA_BUFFER_SIZE   = 8192
PPS_STARTUP_DELAY = 1.0  # Allow up to 1 second for the internal PPS delay before record starts

# Main ----------------------------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description     = "Record I/Q samples using the LiteXM2SDR SoapySDR driver.",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter,
    )
    # RF configuration options.
    parser.add_argument("--samplerate", type=float, default=4e6,     help="RX Sample rate in Hz")
    parser.add_argument("--bandwidth",  type=float, default=56e6,    help="RX Filter bandwidth in Hz")
    parser.add_argument("--freq",       type=float, default=2.4e9,   help="RX frequency in Hz")
    parser.add_argument("--gain",       type=float, default=20.0,    help="RX gain in dB")
    parser.add_argument("--channel",    type=int,   choices=[0, 1],  default=0, help="RX channel index (0 or 1)")

    # Additional options.
    parser.add_argument("--secs",       type=float, default=5.0,     help="Recording duration in seconds")
    parser.add_argument("--check-ts",   action="store_true",         help="Enable timestamp checking and printing")
    parser.add_argument("filename",     type=str,                    help="Output file path for raw CF32 samples")

    args = parser.parse_args()

    # Open the LiteXM2SDR device using the SoapySDR driver.
    sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})

    # Basic RF configuration using the selected channel.
    sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.samplerate)
    sdr.setFrequency( SOAPY_SDR_RX, args.channel, args.freq)
    sdr.setGain(      SOAPY_SDR_RX, args.channel, args.gain)
    sdr.setBandwidth( SOAPY_SDR_RX, args.channel, args.bandwidth)

    # Create and activate RX stream on the specified channel.
    rx_stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [args.channel])
    sdr.activateStream(rx_stream)

    # If timestamp checking is enabled, print the initial hardware timestamp.
    if args.check_ts:
        try:
            hw_time = sdr.getHardwareTime()
            print(f"Initial hardware time: {hw_time}")
        except Exception as e:
            print(f"Unable to get hardware time: {e}")

    print(f"Recording for {args.secs} seconds at {args.freq/1e6:.3f} MHz on channel {args.channel}...")
    t_start = time.time()
    total_samples = 0
    chunk_index = 0
    prev_ts = None  # Previous valid timestamp

    with open(args.filename, "wb") as f:
        while time.time() - t_start < args.secs:
            # Allocate an array of samples: DMA_BUFFER_SIZE bytes / 4 bytes per sample = sample count.
            buf = np.empty(DMA_BUFFER_SIZE // 4, dtype=np.complex64)
            sr = sdr.readStream(rx_stream, [buf], DMA_BUFFER_SIZE // 4)
            # Handle expected startup error (-1) due to waiting for PPS.
            if sr.ret < 0:
                elapsed = time.time() - t_start
                if sr.ret == -1 and elapsed < PPS_STARTUP_DELAY:
                    print(f"Chunk {chunk_index}: readStream returned -1 (startup, elapsed {elapsed:.3f}s), ignoring")
                    continue
                else:
                    print(f"readStream error: {sr.ret}")
                    break
            if sr.ret > 0:
                f.write(buf[:sr.ret].tobytes())
                total_samples += sr.ret

                # Check and display timestamp info if enabled.
                if args.check_ts:
                    current_ts = sr.timeNs
                    if current_ts:
                        if prev_ts is not None and prev_ts:
                            diff = current_ts - prev_ts
                            print(f"Chunk {chunk_index}: Read {sr.ret} samples, timestamp: {current_ts} (diff: {diff} ns)")
                        else:
                            print(f"Chunk {chunk_index}: Read {sr.ret} samples, timestamp: {current_ts}")
                        prev_ts = current_ts
                    else:
                        print(f"Chunk {chunk_index}: Read {sr.ret} samples, no valid timestamp")
                chunk_index += 1

    # Deactivate and close the RX stream.
    sdr.deactivateStream(rx_stream)
    sdr.closeStream(rx_stream)
    print(f"Done. Recorded {total_samples} samples to '{args.filename}'.")

if __name__ == "__main__":
    main()
