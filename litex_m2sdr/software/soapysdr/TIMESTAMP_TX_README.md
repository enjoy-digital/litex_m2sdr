# Timestamp-Based Transmission in LiteX M2SDR SoapySDR Driver

This document describes the implementation of timestamp-based transmission in the LiteX M2SDR SoapySDR driver, following the pattern used in LimeSuiteNG.

## Overview

The timestamp-based transmission feature allows precise control over when samples are transmitted, similar to the TRXLooper implementation in LimeSuiteNG. Instead of transmitting samples immediately, the driver can be configured to transmit samples at specific timestamps based on sample count and sample rate.

## Key Features

- **Sample Count Tracking**: Maintains a running count of transmitted samples
- **Timestamp Calculation**: Calculates transmission timestamps based on sample count and sample rate
- **Synchronized Transmission**: Ensures samples are transmitted at the correct time relative to hardware clock
- **LimeSuiteNG Compatibility**: Follows the same pattern as LimeSuiteNG's TRXLooper

## Architecture

### Stream Structure

The `TXStream` struct has been extended with timestamp tracking fields:

```cpp
struct TXStream: Stream {
    // ... existing fields ...
    
    // Timestamp tracking for LimeSuiteNG-style operation
    uint64_t sample_count;           // Total samples transmitted
    uint64_t base_timestamp;         // Base timestamp when stream was activated
    uint64_t next_timestamp;      // Next timestamp to transmit at
    bool timestamp_mode;             // Enable timestamp-based transmission
};
```

### Timestamp Calculation

Timestamps are calculated using the formula:
```
timestamp = base_timestamp + (sample_count * 1e9) / sample_rate
```

Where:
- `base_timestamp` is the hardware timestamp when the stream was activated
- `sample_count` is the total number of samples transmitted
- `sample_rate` is the configured sample rate in samples per second
- The result is in nanoseconds

## Usage

### Enabling Timestamp Mode

Timestamp mode can be enabled in two ways:

1. **Stream Setup**: Pass `timestamp_mode="1"` in the stream arguments
```cpp
tx_stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], 
                           dict(timestamp_mode="1"))
```

2. **Runtime Control**: Use the timestamp control API
```cpp
sdr.setTXTimestampMode(true);  // Enable timestamp mode
```

### Writing with Timestamps

When timestamp mode is enabled, the `writeStream` function respects the `timeNs` parameter:

```cpp
// Calculate target timestamp for this buffer
uint64_t target_time = hw_time + (buffer_count * buffer_time_ns);

// Write with timestamp
ret = sdr.writeStream(tx_stream, buffs, buffer_size, 
                     flags=0, timeNs=target_time, timeoutUs=1000000);
```

### Timestamp Control API

The driver provides several methods to control and monitor timestamp operation:

```cpp
// Enable/disable timestamp mode
void setTXTimestampMode(bool enable);

// Get current timestamp mode status
bool getTXTimestampMode() const;

// Get current sample count and timestamp information
uint64_t getTXSampleCount() const;
uint64_t getTXBaseTimestamp() const;
uint64_t getTXNextTimestamp() const;
```

## Implementation Details

### DMA Header Timestamps

When timestamp mode is enabled, the DMA header contains the calculated timestamp instead of a fake incrementing timestamp:

```cpp
if (_tx_stream.timestamp_mode) {
    // Use the calculated timestamp based on sample count
    timestamp_to_write = _tx_stream.base_timestamp + 
        static_cast<uint64_t>((_tx_stream.sample_count * 1e9) / _tx_stream.samplerate);
    
    // Update sample count
    _tx_stream.sample_count += samples_per_buffer;
} else {
    // Fallback to the original fake timestamp logic
    static uint64_t fakeTimestamp = 0;
    fakeTimestamp += time_increment;
    timestamp_to_write = fakeTimestamp;
}
```

### Sample Count Synchronization

The sample count is updated each time a DMA buffer is released, ensuring accurate timestamp calculation:

```cpp
// Update sample count when releasing write buffer
_tx_stream.sample_count += samples_per_buffer;
```

## Testing

A test program `test_timestamp_tx.py` is provided to demonstrate the functionality:

```bash
# Basic test with default parameters
python3 test_timestamp_tx.py

# Custom frequency and sample rate
python3 test_timestamp_tx.py -f 1.5e9 -r 15.36e6

# Verbose output to see timestamp details
python3 test_timestamp_tx.py -v -t 10
```

## Comparison with LimeSuiteNG

The implementation follows the same principles as LimeSuiteNG's TRXLooper:

1. **Sample Count Tracking**: Both maintain a running count of transmitted samples
2. **Timestamp Calculation**: Both calculate timestamps based on sample count and sample rate
3. **Synchronized Transmission**: Both ensure samples are transmitted at the correct time
4. **Hardware Integration**: Both integrate with hardware timestamps for accurate timing

## Limitations and Considerations

1. **Sample Rate Dependency**: Timestamp accuracy depends on the configured sample rate
2. **Buffer Alignment**: DMA buffer sizes must be considered for accurate timing
3. **Hardware Clock**: Requires a stable hardware clock reference for accurate timestamps
4. **Overflow Handling**: Large sample counts may require 64-bit arithmetic

## Future Enhancements

Potential improvements to consider:

1. **PPS Synchronization**: Integration with PPS signals for absolute time reference
2. **Multiple Stream Coordination**: Synchronized timestamps across multiple TX streams
3. **Advanced Timing Modes**: Support for burst transmission and complex timing patterns
4. **Timestamp Validation**: Hardware validation of transmission timestamps

## Thread-Based Operation (LimeSuiteNG Style)

The driver now supports thread-based RX and TX operation following the LimeSuiteNG TRXLooper pattern, where separate threads handle continuous RX and TX operations.

### Thread Architecture

Similar to LimeSuiteNG's TRXLooper:
```cpp
// RX thread
mRx.thread = std::thread(RxLoopFunction);

// TX thread  
mTx.thread = std::thread(TxLoopFunction);
```

The implementation provides:
- **RX Thread**: Continuously processes incoming data and updates sample counts
- **TX Thread**: Processes queued transmission data with timestamp control
- **Thread Management**: Start/stop control with proper cleanup
- **Data Queuing**: Thread-safe TX data queuing system

### Thread Control API

```cpp
// Start RX and TX threads
void startThreadLoops();

// Stop RX and TX threads
void stopThreadLoops();

// Check thread status
bool areThreadsRunning() const;

// Get thread statistics
uint64_t getRXSampleCount() const;
uint64_t getRXTimestamp() const;
uint64_t getTXThreadSampleCount() const;
uint64_t getTXThreadTimestamp() const;

// Queue TX data for thread processing
bool queueTXData(const std::vector<std::complex<float>>& data, uint64_t timestamp);
size_t getTXQueueSize() const;
```

### Thread Loop Functions

#### RX Loop Function
```cpp
void SoapyLiteXM2SDR::rxLoopFunction() {
    while (!_rx_stream.thread_stop_requested.load()) {
        // Acquire read buffer
        // Process received data
        // Update sample count and timestamp
        // Release buffer
    }
}
```

#### TX Loop Function
```cpp
void SoapyLiteXM2SDR::txLoopFunction() {
    while (!_tx_stream.thread_stop_requested.load()) {
        // Check TX queue for data
        // Process TX data with timestamp
        // Update sample count and timestamp
    }
}
```

### Thread Safety Features

- **Atomic Operations**: Sample counts and timestamps use atomic variables
- **Mutex Protection**: TX queue access is protected by mutex
- **Condition Variables**: Thread synchronization and signaling
- **Graceful Shutdown**: Proper thread cleanup on stop

### Usage Example

```cpp
// Start thread loops
sdr.startThreadLoops();

// Queue TX data
std::vector<std::complex<float>> data = generateTone(1000);
sdr.queueTXData(data, target_timestamp);

// Monitor thread operation
while (sdr.areThreadsRunning()) {
    uint64_t rx_count = sdr.getRXSampleCount();
    uint64_t tx_count = sdr.getTXThreadSampleCount();
    // Process statistics...
}

// Stop thread loops
sdr.stopThreadLoops();
```

### Testing Thread Loops

A dedicated test program `test_thread_loops.py` demonstrates:
- Starting/stopping RX and TX threads
- Queuing TX data for thread processing
- Monitoring thread statistics
- Continuous operation with timestamp control

```bash
# Test thread-based operation
python3 test_thread_loops.py -v -t 10

# Monitor thread statistics
python3 test_thread_loops.py -v -t 30
```

### Benefits of Thread-Based Operation

1. **Continuous Operation**: RX and TX run independently without blocking
2. **Real-time Processing**: Dedicated threads for time-critical operations
3. **Scalability**: Easy to add custom processing logic in thread functions
4. **LimeSuiteNG Compatibility**: Follows established patterns for SDR applications
5. **Resource Management**: Proper thread lifecycle management and cleanup

### Queue-Based Data Flow

When thread mode is enabled, the data flow changes from direct DMA access to queue-based operation:

#### RX Data Flow (Thread Mode)
```
Hardware → RX Thread → RX Queue → readStream() → Application
```

- **RX Thread**: Continuously reads from DMA and queues data
- **RX Queue**: Thread-safe buffer between RX thread and application
- **readStream()**: Reads from queue instead of directly from DMA
- **Application**: Receives data through standard SoapySDR interface

#### TX Data Flow (Thread Mode)
```
Application → queueTXData() → TX Queue → TX Thread → Hardware
```

- **Application**: Queues data using `queueTXData()`
- **TX Queue**: Thread-safe buffer for pending transmissions
- **TX Thread**: Processes queued data with timestamp control
- **Hardware**: Receives data through DMA

#### Queue Management

```cpp
// RX Queue Control
size_t getRXQueueSize() const;           // Get current RX queue size
void setRXQueueMaxSize(size_t max_size); // Set RX queue maximum size

// TX Queue Control  
size_t getTXQueueSize() const;           // Get current TX queue size
bool queueTXData(data, timestamp);       // Queue data for TX

// Thread Status
bool isThreadModeEnabled() const;        // Check if threads are running
```

#### Automatic Mode Switching

The driver automatically switches between modes:
- **Thread Mode**: When threads are running, `readStream()` reads from RX queue
- **Direct Mode**: When threads are stopped, `readStream()` reads directly from DMA

This provides seamless compatibility with existing applications while enabling advanced thread-based operation.

## References

- [LimeSuiteNG TRXLooper](https://github.com/myriadrf/LimeSuiteNG/blob/develop/src/streaming/TRXLooper.cpp)
- [SoapySDR Documentation](https://github.com/pothosware/SoapySDR/wiki)
- [LiteX M2SDR Documentation](https://github.com/enjoy-digital/litex_m2sdr)
