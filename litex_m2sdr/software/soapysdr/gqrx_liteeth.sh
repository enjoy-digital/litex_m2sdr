#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOFTWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${LITEX_M2SDR_SOAPY_BUILD:-/tmp/litex_m2sdr_soapy_runtime}"
CONFIG_FILE="${LITEX_M2SDR_GQRX_CONFIG:-/tmp/gqrx_litex_m2sdr_liteeth.conf}"
ETH_IP="${LITEX_M2SDR_ETH_IP:-192.168.1.50}"
ETH_PORT="${LITEX_M2SDR_ETH_PORT:-1234}"
SAMPLE_RATE="${LITEX_M2SDR_SAMPLE_RATE:-2000000}"
BANDWIDTH="${LITEX_M2SDR_BANDWIDTH:-1500000}"
FREQUENCY="${LITEX_M2SDR_FREQUENCY:-100000000}"
DEMOD="${LITEX_M2SDR_GQRX_DEMOD:-WFM (mono)}"
FILTER_HIGH="${LITEX_M2SDR_GQRX_FILTER_HIGH:-80000}"
FILTER_LOW="${LITEX_M2SDR_GQRX_FILTER_LOW:--80000}"
OFFSET="${LITEX_M2SDR_GQRX_OFFSET:-0}"
UDP_BUF_COUNT="${LITEX_M2SDR_UDP_BUF_COUNT:-128}"
UDP_RCVBUF="${LITEX_M2SDR_UDP_RCVBUF:-16777216}"
SOAPY_INDEX="${LITEX_M2SDR_GQRX_SOAPY_INDEX:-}"

if [[ "$ETH_IP" == eth:* ]]; then
    DEVICE_ID="$ETH_IP"
elif [[ "$ETH_IP" == *:* ]]; then
    DEVICE_ID="eth:$ETH_IP"
else
    DEVICE_ID="eth:$ETH_IP:$ETH_PORT"
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" -j"$(nproc)"

export SOAPY_SDR_ROOT="${SOAPY_SDR_ROOT:-/tmp/soapy_empty_root}"
mkdir -p "$SOAPY_SDR_ROOT"
export SOAPY_SDR_PLUGIN_PATH="$BUILD_DIR"
export LD_LIBRARY_PATH="$SOFTWARE_DIR/user/libm2sdr:${LD_LIBRARY_PATH:-}"

if [[ -z "$SOAPY_INDEX" ]]; then
    FIND_OUTPUT="$(SoapySDRUtil --find="driver=LiteXM2SDR" 2>&1 || true)"
    SOAPY_INDEX="$(printf '%s\n' "$FIND_OUTPUT" | awk -v dev_id="$DEVICE_ID" '
        /^Found device [0-9]+/ {
            idx = $3
            sub(/:$/, "", idx)
        }
        /^[[:space:]]*dev_id[[:space:]]*=/ && $3 == dev_id {
            print idx
            exit
        }
    ')"
    if [[ -z "$SOAPY_INDEX" ]]; then
        printf 'Could not find %s in Soapy discovery output:\n%s\n' "$DEVICE_ID" "$FIND_OUTPUT" >&2
        exit 1
    fi
fi

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
device="soapy=$SOAPY_INDEX,driver=LiteXM2SDR,dev_id=$DEVICE_ID,channels={0},udp_buf_count=$UDP_BUF_COUNT,udp_rcvbuf=$UDP_RCVBUF"
frequency=$FREQUENCY
sample_rate=$SAMPLE_RATE

[receiver]
demod=$DEMOD
filter_high_cut=$FILTER_HIGH
filter_low_cut=$FILTER_LOW
offset=$OFFSET

[remote_control]
allowed_hosts=127.0.0.1
enabled=true
port=7356
EOF

exec gqrx -c "$CONFIG_FILE"
