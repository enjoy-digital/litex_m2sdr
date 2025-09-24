#!/bin/bash

# Build script to test timestamp functionality compilation
# This script builds the SoapySDR driver with the new timestamp features

set -e

echo "Building LiteX M2SDR SoapySDR driver with timestamp functionality..."

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: Please run this script from the soapysdr directory"
    exit 1
fi

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build
echo "Building..."
make -j$(nproc)

echo "Build completed successfully!"
echo ""
echo "To test the timestamp functionality:"
echo "1. Install the driver: sudo make install"
echo "2. Run the test: python3 ../test_timestamp_tx.py"
echo ""
echo "To test the thread-based RX/TX loops:"
echo "3. Run the thread test: python3 ../test_thread_loops.py"
echo ""
echo "To test queue-based reading:"
echo "4. Run the queue reading test: python3 ../test_queue_reading.py"
echo ""
echo "Note: You may need to rebuild the gateware if you want to test the hardware timestamp features."
