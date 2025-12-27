#!/usr/bin/env python3
"""
Test program for timestamp-based transmission in LiteX M2SDR SoapySDR driver.
This demonstrates the LimeSuiteNG-style timestamp-based transmission where
samples are transmitted at specific timestamps based on sample count.

Usage:
    python3 test_timestamp_tx.py [options]

Options:
    -f, --freq FREQ        Frequency in Hz (default: 2.4e9)
    -r, --rate RATE        Sample rate in Hz (default: 30.72e6)
    -g, --gain GAIN        TX gain in dB (default: 0)
    -t, --time TIME        Transmission time in seconds (default: 5)
    -c, --channel CHANNEL  Channel number (default: 0)
    -v, --verbose          Enable verbose logging
"""

import argparse
import time
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_TX, SOAPY_SDR_CF32

def main():
    parser = argparse.ArgumentParser(description='Test timestamp-based transmission')
    parser.add_argument('-f', '--freq', type=float, default=2.4e9, help='Frequency in Hz')
    parser.add_argument('-r', '--rate', type=float, default=30.72e6, help='Sample rate in Hz')
    parser.add_argument('-g', '--gain', type=float, default=0, help='TX gain in dB')
    parser.add_argument('-t', '--time', type=float, default=5, help='Transmission time in seconds')
    parser.add_argument('-c', '--channel', type=int, default=0, help='Channel number')
    parser.add_argument('-v', '--verbose', action='store_true', help='Enable verbose logging')
    
    args = parser.parse_args()
    
    # Initialize SDR
    try:
        sdr = SoapySDR.Device({"driver": "LiteXM2SDR"})
        print(f"Found device: {sdr.getHardwareKey()}")
    except Exception as e:
        print(f"Failed to open device: {e}")
        return 1
    
    # Configure device
    sdr.setSampleRate(SOAPY_SDR_TX, args.channel, args.rate)
    sdr.setFrequency(SOAPY_SDR_TX, args.channel, args.freq)
    sdr.setGain(SOAPY_SDR_TX, args.channel, args.gain)
    
    print(f"Configured: freq={args.freq/1e6:.3f} MHz, rate={args.rate/1e6:.3f} MSps, gain={args.gain} dB")
    
    # Setup TX stream with timestamp mode enabled
    tx_stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [args.channel], 
                               dict(timestamp_mode="1"))
    
    # Get stream MTU
    mtu = sdr.getStreamMTU(tx_stream)
    print(f"Stream MTU: {mtu} samples")
    
    # Calculate buffer size and timing
    buffer_size = mtu
    samples_per_buffer = buffer_size
    buffer_time_ns = int((samples_per_buffer / args.rate) * 1e9)
    
    print(f"Buffer timing: {buffer_time_ns} ns per buffer ({samples_per_buffer} samples)")
    
    # Activate stream
    sdr.activateStream(tx_stream)
    
    # Get initial hardware time
    try:
        hw_time = sdr.getHardwareTime()
        print(f"Initial hardware time: {hw_time}")
    except Exception as e:
        print(f"Unable to get hardware time: {e}")
        hw_time = 0
    
    # Generate test signal (tone)
    freq_tone = 1e6  # 1 MHz tone
    t = np.arange(buffer_size) / args.rate
    tone = np.exp(2j * np.pi * freq_tone * t).astype(np.complex64)
    
    # Transmission loop
    print(f"Starting timestamp-based transmission for {args.time} seconds...")
    start_time = time.time()
    total_samples = 0
    buffer_count = 0
    
    while time.time() - start_time < args.time:
        # Calculate target timestamp for this buffer
        target_time = hw_time + (buffer_count * buffer_time_ns)

        # Prepare buffer
        buffs = [tone]
        
        # Write with timestamp
        SoapyStreamResult = sdr.writeStream(tx_stream, buffs, buffer_size, flags=0, timeNs=target_time, timeoutUs=1000000)
        if SoapyStreamResult.ret < 0:
            if SoapyStreamResult.ret == SoapySDR.SOAPY_SDR_TIMEOUT:
                if args.verbose:
                    print(f"Buffer {buffer_count}: timeout waiting for timestamp {target_time}")
                continue
            else:
                print(f"Write error: {SoapyStreamResult}")
                break
        
        if SoapyStreamResult.ret > 0:
            total_samples += SoapyStreamResult.ret
            buffer_count += 1
            
            if args.verbose and buffer_count % 5000 == 0:
                current_hw_time = sdr.getHardwareTime() if hw_time > 0 else 0
                print(f"Buffer {buffer_count}: transmitted {SoapyStreamResult.ret} samples at timestamp {target_time}, "
                      f"hw_time={current_hw_time}, total_samples={total_samples}")
    
    # Deactivate and close stream
    sdr.deactivateStream(tx_stream)
    sdr.closeStream(tx_stream)
    
    # Print statistics
    elapsed_time = time.time() - start_time
    actual_rate = total_samples / elapsed_time
    print(f"\nTransmission complete:")
    print(f"  Total samples: {total_samples}")
    print(f"  Total buffers: {buffer_count}")
    print(f"  Elapsed time: {elapsed_time:.3f} seconds")
    print(f"  Actual rate: {actual_rate/1e6:.3f} MSps")
    print(f"  Expected rate: {args.rate/1e6:.3f} MSps")
    
    return 0

if __name__ == "__main__":
    exit(main())
