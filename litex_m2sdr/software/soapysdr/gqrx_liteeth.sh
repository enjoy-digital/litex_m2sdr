#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOFTWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${LITEX_M2SDR_SOAPY_BUILD:-/tmp/litex_m2sdr_soapy_liteeth}"
CONFIG_FILE="${LITEX_M2SDR_GQRX_CONFIG:-/tmp/gqrx_litex_m2sdr_liteeth.conf}"
ETH_IP="${LITEX_M2SDR_ETH_IP:-192.168.1.50}"
SAMPLE_RATE="${LITEX_M2SDR_SAMPLE_RATE:-10000000}"
BANDWIDTH="${LITEX_M2SDR_BANDWIDTH:-10000000}"
FREQUENCY="${LITEX_M2SDR_FREQUENCY:-2400000000}"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DUSE_LITEETH=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" -j"$(nproc)"

cat > "$CONFIG_FILE" <<EOF
[General]
configversion=3
crashed=false

[audio]
gain=-60
udp_host=localhost
udp_port=65535

[fft]
fft_size=32768
fft_window=0
pandapter_max_db=0
pandapter_min_db=-140
waterfall_max_db=0
waterfall_min_db=-140

[input]
bandwidth=$BANDWIDTH
device="soapy=0,driver=LiteXM2SDR,eth_ip=$ETH_IP,channels={0}"
frequency=$FREQUENCY
sample_rate=$SAMPLE_RATE

[receiver]
demod=AM
filter_high_cut=5000
filter_low_cut=-5000
offset=0

[remote_control]
allowed_hosts=127.0.0.1
enabled=true
port=7356
EOF

export SOAPY_SDR_ROOT="${SOAPY_SDR_ROOT:-/tmp/soapy_empty_root}"
mkdir -p "$SOAPY_SDR_ROOT"
export SOAPY_SDR_PLUGIN_PATH="$BUILD_DIR"
export LD_LIBRARY_PATH="$SOFTWARE_DIR/user/libm2sdr:${LD_LIBRARY_PATH:-}"

exec gqrx -c "$CONFIG_FILE"
