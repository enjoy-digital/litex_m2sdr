#!/usr/bin/env python3
"""
Simple test program to demonstrate queue-based reading in thread mode.
This shows how readStream() reads from the RX queue instead of directly from DMA
when thread mode is enabled.
"""

import time
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

def main():
    print("Testing queue-based reading in thread mode...")
    
    # Initialize SDR
    try:
        sdr = SoapySDR.Device(dict(driver="litexm2sdr"))
        print(f"Found device: {sdr.getHardwareKey()}")
    except Exception as e:
        print(f"Failed to open device: {e}")
        return 1
    
    # Configure device
    sdr.setSampleRate(SOAPY_SDR_RX, 0, 30.72e6)
    sdr.setFrequency(SOAPY_SDR_RX, 0, 2.4e9)
    
    # Setup RX stream
    rx_stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    
    # Activate stream
    sdr.activateStream(rx_stream)
    
    print("Stream activated, starting thread loops...")
    
    # Start thread loops
    sdr.startThreadLoops()
    
    # Wait for threads to start
    time.sleep(1)
    
    if not sdr.areThreadsRunning():
        print("Error: Threads failed to start")
        return 1
    
    print("Thread loops started successfully")
    print(f"Thread mode enabled: {sdr.isThreadModeEnabled()}")
    
    # Test reading from queue
    print("\nTesting readStream() in thread mode...")
    print("This should read from the RX queue, not directly from DMA")
    
    # Read samples multiple times to see queue behavior
    for i in range(5):
        print(f"\nRead attempt {i+1}:")
        
        # Check queue status
        rx_queue_size = sdr.getRXQueueSize()
        print(f"  RX queue size before read: {rx_queue_size}")
        
        # Try to read samples
        try:
            # Create buffer for samples
            buffer = np.empty(1024, dtype=np.complex64)
            ret = sdr.readStream(rx_stream, [buffer], 1024, timeoutUs=1000000)
            
            if ret.ret > 0:
                print(f"  Successfully read {ret.ret} samples")
                print(f"  Flags: {ret.flags}")
                print(f"  Timestamp: {ret.timeNs}")
                
                # Check queue size after read
                rx_queue_size_after = sdr.getRXQueueSize()
                print(f"  RX queue size after read: {rx_queue_size_after}")
                
                # Show some sample values
                if ret.ret > 0:
                    print(f"  First few samples: {buffer[:min(5, ret.ret)]}")
            else:
                print(f"  readStream returned: {ret.ret}")
                
        except Exception as e:
            print(f"  Error reading: {e}")
        
        # Small delay between reads
        time.sleep(0.5)
    
    # Stop thread loops
    print("\nStopping thread loops...")
    sdr.stopThreadLoops()
    
    # Deactivate and close stream
    sdr.deactivateStream(rx_stream)
    sdr.closeStream(rx_stream)
    
    print("Test complete!")
    return 0

if __name__ == "__main__":
    exit(main())







