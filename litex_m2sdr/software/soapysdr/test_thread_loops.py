#!/usr/bin/env python3
"""
Test program for thread-based RX and TX loops in LiteX M2SDR SoapySDR driver.
This demonstrates the LimeSuiteNG-style thread-based operation where RX and TX
run in separate threads for continuous operation.

Usage:
    python3 test_thread_loops.py [options]

Options:
    -f, --freq FREQ        Frequency in Hz (default: 2.4e9)
    -r, --rate RATE        Sample rate in Hz (default: 30.72e6)
    -g, --gain GAIN        TX gain in dB (default: 0)
    -t, --time TIME        Operation time in seconds (default: 10)
    -c, --channel CHANNEL  Channel number (default: 0)
    -v, --verbose          Enable verbose logging
"""

import argparse
import time
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_TX, SOAPY_SDR_CF32
import threading

def rx_monitor(sdr, stop_event, verbose=False):
    """Monitor RX thread statistics"""
    while not stop_event.is_set():
        try:
            rx_count = sdr.getRXSampleCount()
            rx_timestamp = sdr.getRXTimestamp()
            if verbose:
                print(f"RX: samples={rx_count}, timestamp={rx_timestamp}")
            time.sleep(0.1)
        except Exception as e:
            print(f"RX monitor error: {e}")
            break

def tx_monitor(sdr, stop_event, verbose=False):
    """Monitor TX thread statistics"""
    while not stop_event.is_set():
        try:
            tx_count = sdr.getTXThreadSampleCount()
            tx_timestamp = sdr.getTXThreadTimestamp()
            queue_size = sdr.getTXQueueSize()
            if verbose:
                print(f"TX: samples={tx_count}, timestamp={tx_timestamp}, queue={queue_size}")
            time.sleep(0.1)
        except Exception as e:
            print(f"TX monitor error: {e}")
            break

def rx_reader(sdr, rx_stream, stop_event, rx_data_received, verbose=False):
    """Thread to read samples from the RX stream."""
    while not stop_event.is_set():
        try:
            # Read samples from the RX stream
            # readStream returns (ret, flags, timeNs)
            ret = sdr.readStream(rx_stream, [np.empty(1024, dtype=np.complex64)], 1024)
            
            if ret.ret > 0:
                # Successfully read samples
                rx_data_received.extend(ret.ret)
                if verbose:
                    print(f"RX reader: read {ret.ret} samples, flags={ret.flags}, time={ret.timeNs}")
            elif ret.ret == 0:
                # No samples available
                if verbose:
                    print("RX reader: no samples available")
                time.sleep(0.001)  # Small delay
            else:
                # Error or timeout
                if verbose:
                    print(f"RX reader: readStream returned {ret.ret}")
                time.sleep(0.001)  # Small delay
                
        except Exception as e:
            print(f"RX reader error: {e}")
            break

def main():
    parser = argparse.ArgumentParser(description='Test thread-based RX and TX loops')
    parser.add_argument('-f', '--freq', type=float, default=2.4e9, help='Frequency in Hz')
    parser.add_argument('-r', '--rate', type=float, default=30.72e6, help='Sample rate in Hz')
    parser.add_argument('-g', '--gain', type=float, default=0, help='TX gain in dB')
    parser.add_argument('-t', '--time', type=float, default=10, help='Operation time in seconds')
    parser.add_argument('-c', '--channel', type=int, default=0, help='Channel number')
    parser.add_argument('-v', '--verbose', action='store_true', help='Enable verbose logging')
    
    args = parser.parse_args()
    
    # Initialize SDR
    try:
        sdr = SoapySDR.Device(dict(driver="litexm2sdr"))
        print(f"Found device: {sdr.getHardwareKey()}")
    except Exception as e:
        print(f"Failed to open device: {e}")
        return 1
    
    # Configure device
    sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
    sdr.setSampleRate(SOAPY_SDR_TX, args.channel, args.rate)
    sdr.setFrequency(SOAPY_SDR_RX, args.channel, args.freq)
    sdr.setFrequency(SOAPY_SDR_TX, args.channel, args.freq)
    sdr.setGain(SOAPY_SDR_TX, args.channel, args.gain)
    
    print(f"Configured: freq={args.freq/1e6:.3f} MHz, rate={args.rate/1e6:.3f} MSps, gain={args.gain} dB")
    
    # Setup streams
    rx_stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [args.channel])
    tx_stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [args.channel])
    
    # Get stream MTU
    rx_mtu = sdr.getStreamMTU(rx_stream)
    tx_mtu = sdr.getStreamMTU(tx_stream)
    print(f"Stream MTU: RX={rx_mtu}, TX={tx_mtu} samples")
    
    # Activate streams
    sdr.activateStream(rx_stream)
    sdr.activateStream(tx_stream)
    
    # Start thread loops
    print("Starting thread loops...")
    sdr.startThreadLoops()
    
    # Wait a moment for threads to start
    time.sleep(0.5)
    
    if not sdr.areThreadsRunning():
        print("Error: Threads failed to start")
        return 1
    
    print("Thread loops started successfully")
    
    # Start monitoring threads
    stop_event = threading.Event()
    rx_monitor_thread = threading.Thread(target=rx_monitor, args=(sdr, stop_event, args.verbose))
    tx_monitor_thread = threading.Thread(target=tx_monitor, args=(sdr, stop_event, args.verbose))
    
    rx_monitor_thread.start()
    tx_monitor_thread.start()
    
    # Start a separate thread to read from RX stream (demonstrates queue-based reading)
    rx_data_received = []
    rx_read_thread = threading.Thread(target=rx_reader, args=(sdr, rx_stream, stop_event, rx_data_received, args.verbose))
    rx_read_thread.start()
    
    # Generate and queue test data for TX
    print(f"Generating and queuing test data for {args.time} seconds...")
    start_time = time.time()
    
    # Generate test signal (tone)
    freq_tone = 1e6  # 1 MHz tone
    samples_per_buffer = tx_mtu
    t = np.arange(samples_per_buffer) / args.rate
    tone = np.exp(2j * np.pi * freq_tone * t).astype(np.complex64)
    
    # Calculate timing
    buffer_time_ns = int((samples_per_buffer / args.rate) * 1e9)
    
    # Get initial hardware time
    try:
        hw_time = sdr.getHardwareTime()
        print(f"Initial hardware time: {hw_time}")
    except Exception as e:
        print(f"Unable to get hardware time: {e}")
        hw_time = 0
    
    buffer_count = 0
    total_queued = 0
    
    while time.time() - start_time < args.time:
        # Calculate target timestamp for this buffer
        target_time = hw_time + (buffer_count * buffer_time_ns)
        
        # Queue data for TX thread
        if sdr.queueTXData(tone.tolist(), target_time):
            total_queued += samples_per_buffer
            buffer_count += 1
            
            if args.verbose and buffer_count % 100 == 0:
                print(f"Queued buffer {buffer_count}: {samples_per_buffer} samples at timestamp {target_time}")
        else:
            print(f"Failed to queue buffer {buffer_count}")
        
        # Small delay to prevent overwhelming the queue
        time.sleep(0.001)
    
    print(f"Data generation complete: queued {buffer_count} buffers, {total_queued} total samples")
    
    # Wait for TX queue to empty
    print("Waiting for TX queue to empty...")
    while sdr.getTXQueueSize() > 0:
        time.sleep(0.1)
        if args.verbose:
            print(f"TX queue size: {sdr.getTXQueueSize()}")
    
    print("TX queue emptied")
    
    # Stop monitoring threads
    stop_event.set()
    rx_monitor_thread.join()
    tx_monitor_thread.join()
    rx_read_thread.join() # Wait for the reader thread to finish
    
    # Stop thread loops
    print("Stopping thread loops...")
    sdr.stopThreadLoops()
    
    # Deactivate and close streams
    sdr.deactivateStream(rx_stream)
    sdr.deactivateStream(tx_stream)
    sdr.closeStream(rx_stream)
    sdr.closeStream(tx_stream)
    
    # Print final statistics
    elapsed_time = time.time() - start_time
    print(f"\nThread loop test complete:")
    print(f"  Operation time: {elapsed_time:.3f} seconds")
    print(f"  Final RX samples: {sdr.getRXSampleCount()}")
    print(f"  Final TX samples: {sdr.getTXThreadSampleCount()}")
    print(f"  Threads running: {sdr.areThreadsRunning()}")
    print(f"  RX data received: {len(rx_data_received)} samples")
    
    return 0

if __name__ == "__main__":
    exit(main())
