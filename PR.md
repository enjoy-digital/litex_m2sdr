## Overview

This PR introduces a timestamp-driven TX scheduling subsystem inside the AD9361 gateware.
It also adds a complete simulation workflow and testbench infrastructure to verify the scheduler behavior.

This PR is currently submitted as a Draft for architectural and design review.

## Motivation

The objective is to create a timestamp-aware pipeline inside the AD9361 data path that can be integrated for SoapySDR Software.

## Main Contributions

### 1. Timestamp-Based TX Scheduler (Gateware)
A new module under the AD9361 gateware hierarchy:
    
    - Reads incoming TX frames and metadata from the SyncFIFO
    
    - Extracts header + timestamp from the packet
    
    - Buffers up to 8 packets
    
    - Compares packet timestamp with an RFIC-domain time counter
    
    - Releases packets when the scheduled time is reached
    
    - Handles backpressure and supports continuous streaming
    
    - Integrates cleanly with LiteX Stream (Endpoint) interfaces
    
    - Provides CSR access for debug/monitoring
    
    - CDC-safe interaction with RFIC time domain (MultiReg, PulseSynchronizer)

###  2. Scheduler Simulation & Testbench Framework
A standalone simulation environment was added to allow:
    
     - cycle-accurate Migen/LiteX simulations
    
     - Extracts header + timestamp from the packet
    
     - Buffers up to 8 packets
    
     - cycle-accurate Migen/LiteX simulations
     
     - controlled timestamp generation
     
     - packet injection with configurable timing offsets

     - VCD waveform dumping (GTKWave)
 
     - YAML-driven experiment descriptions (reusable test scenarios)
      
      - stimulus generators and result monitors

This greatly simplifies validating scheduling logic without hardware. And allowed to validate the scheduler's implementation


## Issues

To test the TX timestamp feature we created a test file: software/soapysdr/test_timestamp.py:
This script uses soapysdr writeStream function (enabling _TX_DMA_HEADER_TEST to write faketimestamps). 

The issue we encounter is when we try reading from the DMA HeaderExtractor CSR LAST_TX_TIMESTAMP, it is always 0. 
Here is an example of the output of test_timestamp.py:

```
Writing packet 256: packet_timestamp=584270721900486
[FROM CSR] Last TX Timestamp = 0
--------------------------------------------------------

Transmission complete:
  Total samples: 523264
  Total buffers: 256
  Elapsed time: 5.011 seconds
  Actual rate: 0.104 MSps
  Expected rate: 30.720 MSps
```

It seems like there is an issue in the way soapysdr inserts packets to the FGPA. 

## Requests to Reviewers:

1. Review the scheduler implementation
2. Comment on the simulation workflow (that seemed missing in the gateware as a whole)
3. soapysdr issue

## Conclusion

We hope that this contribution to enabling the use of LitexM2SDR with soapysdr and in srs5g stack is appreciated and will be reviewed. 