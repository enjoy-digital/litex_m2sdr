#!/usr/bin/env python3

"""
test_record.py - Record I/Q samples using the LiteXM2SDR SoapySDR driver.

This script uses the SoapySDR driver to interface with the LiteXM2SDR hardware.
It records I/Q samples for a specified duration and writes raw CF32 samples to a file.
Command-line options allow you to configure sample rate, bandwidth, frequency, and gain.

Usage Example:
    ./test_record.py --samplerate 30.72e6 --bandwidth 56e6 --freq 2.4e9 --gain 20 --secs 5 filename.bin
"""

import time
import argparse
import numpy as np

import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

# Constants -----------------------------------------------------------------------------------------

CHUNK_SIZE = 1024

# Main ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Record I/Q samples using the LiteXM2SDR SoapySDR driver.")
    # RF configuration options.
    parser.add_argument("--samplerate", type=float, default=30.72e6, help="RX Sample rate in Hz (default: 30720000)")
    parser.add_argument("--bandwidth",  type=float, default=56e6,    help="RX Filter bandwidth in Hz (default: 56000000)")
    parser.add_argument("--freq",       type=float, default=2.4e9,   help="RX frequency in Hz (default: 2400000000)")
    parser.add_argument("--gain",       type=float, default=20.0,    help="RX gain in dB (default: 20)")
    # Additional options.
    parser.add_argument("--secs",       type=float, default=5.0,    help="Recording duration in seconds (default: 5)")
    parser.add_argument("filename",     type=str,   help="Output file path for raw CF32 samples")
    args = parser.parse_args()

    # Open the LiteXM2SDR device using the SoapySDR driver.
    sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})

    # Basic RF configuration.
    sdr.setSampleRate(SOAPY_SDR_RX, 0, args.samplerate)
    sdr.setFrequency( SOAPY_SDR_RX, 0, args.freq)
    sdr.setGain(      SOAPY_SDR_RX, 0, args.gain)
    sdr.setBandwidth( SOAPY_SDR_RX, 0, args.bandwidth)

    # Create and activate RX stream.
    rx_stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    sdr.activateStream(rx_stream)

    print(f"Recording for {args.secs} seconds at {args.freq/1e6:.3f} MHz...")
    t_start = time.time()
    total_samples = 0

    with open(args.filename, "wb") as f:
        while time.time() - t_start < args.secs:
            buf = np.empty(CHUNK_SIZE, dtype=np.complex64)
            sr = sdr.readStream(rx_stream, [buf], CHUNK_SIZE)
            if sr.ret < 0:
                print(f"readStream error: {sr.ret}")
                break
            if sr.ret > 0:
                f.write(buf[:sr.ret].tobytes())
                total_samples += sr.ret

    # Deactivate and close the RX stream.
    sdr.deactivateStream(rx_stream)
    sdr.closeStream(rx_stream)
    print(f"Done. Recorded {total_samples} samples to '{args.filename}'.")

if __name__ == "__main__":
    main()
