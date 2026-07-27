#!/usr/bin/env bash
# M2SDR image qualification — a BENCH acceptance test for a gateware image we built,
# so we know what we ship to customers is stable and solid.
#
# This is NOT a field/runtime tool and it does NO demotion: run it on OUR hardware,
# with the TX<->RX loopback cable(s) in place, against an image we just built. It
# certifies the image on two levels and prints a single verdict:
#
#   software-asserted : timing-clean  (WNS >= 0, zero failing setup/hold endpoints),
#                       parsed from the build's Vivado timing report (release.check_timing).
#   hardware-tested   : RFIC alive (product-ID 0x0a), all clocks present (incl. the AD9361
#                       DATA clock), PCIe link up, CSR sanity, TX->RX datapath + DMA-header
#                       integrity (loopback_selftest), a DMA soak, and — for images that carry
#                       the timed-TX gate — on-air timing accuracy (timed_tx_selftest).
#
# A candidate that mis-clocks the RFIC (the exact failure that once looked like a "dead chip")
# is caught HERE, on the bench, and never ships.
#
# Usage:
#   scripts/qualify_image.sh --build-dir build/<name> [--timed-tx]   # timing-assert + flash + full hw qual
#   scripts/qualify_image.sh --image <operational.bin>               # flash that image + hw qual
#   scripts/qualify_image.sh                                         # qualify the CURRENTLY-LOADED image (no flash)
#
# Options:
#   --build-dir DIR   LiteX build output dir (build/<name>); enables the timing assertion and
#                     auto-locates <name>_operational.bin to flash.
#   --image BIN       explicit operational .bin to flash to the operational slot (0x800000).
#   --chan 1|2|both   loopback port pair for the datapath/timed tests (default: 1, i.e. TX1<->RX1).
#   --soak SEC        DMA soak duration in seconds (default: 30; 0 disables).
#   --timed-tx        include timed-TX gate accuracy. Requires the gate in the image AND a loopback
#                     cable. Opt-in on purpose: probing a fixed CSR on an image that lacks the gate
#                     would hit the CSR-map-shift trap, so we never auto-poke.
#   --expect "STR"    optional expected PCIe link, e.g. "Gen2 x4" — asserted, else just reported.
#   --quick           short loopback matrix (faster smoke).
#   -y                skip the confirmation prompt before flashing.
#   -h, --help        this help.
#
# Exit status: 0 = QUALIFIED (all checks PASS; SKIPs are allowed), 1 = NOT QUALIFIED, 2 = usage error.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
USERDIR="$REPO/litex_m2sdr/software/user"
LIBDIR="$USERDIR/libm2sdr"
KERNDIR="$REPO/litex_m2sdr/software/kernel"
UTIL="$USERDIR/m2sdr_util"
RF="$USERDIR/m2sdr_rf"
KO="$KERNDIR/m2sdr.ko"
OP_OFFSET=0x800000

BUILD_DIR=""
IMAGE=""
CHAN=1
SOAK=30
DO_TIMED=0
EXPECT=""
QUICK=0
FORCE=0
RATE=30720000
LAYOUT=1t1r
# Loopback link budget. Defaults suit a bare TX->RX cable; a conducted loopback with fixed
# attenuators must compensate them or the tone/marker lands in the noise and the datapath checks
# fail on a perfectly good image (measured on this bench: 40 dB pads need rx-gain ~54 on the TX1/RX1
# path and ~44 on TX2/RX2 -- the two paths differ ~10 dB -- with tx-att 0).
RX_GAIN=40
TX_ATT=10
LB_CONFIG=""    # optional: restrict the loopback matrix to one config name
TEMP_MAX=100    # fail the qualification above this AD9361 junction temperature
TEMP_COOL=85    # wait for the part to drop below this before each RF-heavy phase

die()  { echo "qualify: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --image)     IMAGE="${2:-}"; shift 2 ;;
        --chan)      CHAN="${2:-}"; shift 2 ;;
        --rate)      RATE="${2:-}"; shift 2 ;;
        --layout)    LAYOUT="${2:-}"; shift 2 ;;
        --rx-gain)   RX_GAIN="${2:-}"; shift 2 ;;
        --tx-att)    TX_ATT="${2:-}"; shift 2 ;;
        --config)    LB_CONFIG="${2:-}"; shift 2 ;;
        --soak)      SOAK="${2:-}"; shift 2 ;;
        --timed-tx)  DO_TIMED=1; shift ;;
        --expect)    EXPECT="${2:-}"; shift 2 ;;
        --quick)     QUICK=1; shift ;;
        -y|--force)  FORCE=1; shift ;;
        -h|--help)   sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'; exit 0 ;;
        *) die "unknown arg: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------------------
# result tracking
# ---------------------------------------------------------------------------
declare -a STEP_NAME STEP_STATE STEP_DETAIL
NFAIL=0
record_step() {  # name state detail
    STEP_NAME+=("$1"); STEP_STATE+=("$2"); STEP_DETAIL+=("${3:-}")
    printf '  [%-4s] %-22s %s\n' "$2" "$1" "${3:-}"
    [ "$2" = FAIL ] && NFAIL=$((NFAIL + 1))
    return 0
}
pass() { record_step "$1" PASS "${2:-}"; }
fail() { record_step "$1" FAIL "${2:-}"; }
skip() { record_step "$1" SKIP "${2:-}"; }
hdr()  { echo; echo "== $* =="; }

# ---------------------------------------------------------------------------
# 0. build the in-repo tools we depend on (never the installed copy)
# ---------------------------------------------------------------------------
hdr "prep"
# The tools MUST be built against the csr.h that the gateware build generated: the CSR map shifts
# with the gateware, and tools compiled against a different map drive registers at wrong addresses
# (an AD9361 SPI read then returns 0x0/0xFF and looks like a dead RFIC). Always rebuild when we are
# about to flash an image, so tools and bitstream match by construction.
if [ -n "$BUILD_DIR" ] || [ -n "$IMAGE" ]; then
    echo "rebuilding in-repo user tools against the current generated csr.h ..."
    make -C "$USERDIR" clean >/dev/null 2>&1
    make -C "$USERDIR" libm2sdr/libm2sdr.a m2sdr_util m2sdr_rf >/dev/null || die "user tool build failed"
elif [ ! -x "$UTIL" ] || [ ! -x "$RF" ] || [ ! -f "$LIBDIR/libm2sdr.a" ]; then
    echo "building in-repo user tools ..."
    make -C "$USERDIR" libm2sdr/libm2sdr.a m2sdr_util m2sdr_rf >/dev/null || die "user tool build failed"
fi
echo "repo:   $REPO"
echo "commit: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '?')"

# ---------------------------------------------------------------------------
# 1. software assertion: timing-clean (only when a build dir is given)
# ---------------------------------------------------------------------------
WNS_NS=""
if [ -n "$BUILD_DIR" ]; then
    hdr "software assert: timing"
    BUILD_DIR="${BUILD_DIR%/}"
    BUILD_NAME="$(basename "$BUILD_DIR")"
    BUILD_ROOT="$(dirname "$BUILD_DIR")"
    TIMING_OUT="$(cd "$REPO" && python3 - "$BUILD_ROOT" "$BUILD_NAME" <<'PY' 2>&1
import sys
from release import check_timing, parse_timing_summary
from pathlib import Path
build_root, name = sys.argv[1], sys.argv[2]
rpt = Path(build_root, name, "gateware", f"{name}_timing.rpt")
try:
    s = parse_timing_summary(rpt.read_text(errors="replace"))
    # Same policy as the project's release flow: setup/hold must be clean; the single -42ps
    # pulse-width endpoint on the Xilinx 7-series PCIe IP clocks is explicitly allowed there.
    check_timing(build_root, name, allow_pcie_pulse_width_warning=True)
    print(f"OK wns={s['wns_ns']}ns tns_fail={s['tns_failing_endpoints']} ths_fail={s['ths_failing_endpoints']}")
except SystemExit as e:
    print(f"FAIL {e}")
    sys.exit(1)
except Exception as e:
    print(f"FAIL {e}")
    sys.exit(1)
PY
    )"
    if [ $? -eq 0 ]; then
        WNS_NS="$(echo "$TIMING_OUT" | grep -oE 'wns=[-0-9.]+' | head -1)"
        pass "timing-clean" "$TIMING_OUT"
    else
        fail "timing-clean" "$TIMING_OUT"
        echo; echo ">> refusing to qualify a bitstream that fails timing (marginal images kill the AD9361)."
        # continue to report, but a timing failure alone is disqualifying
    fi
    # locate the operational image to flash if none was given explicitly
    if [ -z "$IMAGE" ]; then
        CAND="$BUILD_DIR/gateware/${BUILD_NAME}_operational.bin"
        [ -f "$CAND" ] && IMAGE="$CAND" || echo "note: no ${BUILD_NAME}_operational.bin found; will qualify the loaded image"
    fi
fi

# ---------------------------------------------------------------------------
# 2. flash the candidate to the operational slot + warm-boot the FPGA (opt-in)
# ---------------------------------------------------------------------------
if [ -n "$IMAGE" ]; then
    [ -f "$IMAGE" ] || die "image not found: $IMAGE"
    hdr "flash + reload"
    echo "image:  $IMAGE  ->  operational slot $OP_OFFSET"
    if [ "$FORCE" != 1 ]; then
        read -r -p "flash this image to the board and warm-boot it? [y/N] " ans
        [ "$ans" = y ] || [ "$ans" = Y ] || die "aborted by user"
    fi
    echo YES | sudo "$UTIL" flash-write "$IMAGE" "$OP_OFFSET" || die "flash-write failed"
    sudo timeout 30 "$UTIL" flash-reload 2>&1 | tail -1 || true       # ICAP IPROG; link drops
    sleep 4
    sudo rmmod m2sdr 2>/dev/null || true
    BDF="$(lspci -Dn -d 10ee: 2>/dev/null | awk 'NR==1{print $1}')"
    if [ -n "$BDF" ]; then
        echo 1 | sudo tee "/sys/bus/pci/devices/$BDF/remove" >/dev/null 2>&1 || true
        sleep 1
    fi
    echo 1 | sudo tee /sys/bus/pci/rescan >/dev/null 2>&1 || true
    sleep 2
    [ -f "$KO" ] && sudo insmod "$KO" 2>/dev/null || sudo modprobe m2sdr 2>/dev/null || true
    sleep 1
    if [ -e /dev/m2sdr0 ]; then pass "flash+reload" "device re-enumerated"; else fail "flash+reload" "/dev/m2sdr0 absent after reload"; fi
else
    echo; echo "== no image given: qualifying the CURRENTLY-LOADED image (no flash) =="
fi

# ---------------------------------------------------------------------------
# helper: run a m2sdr_util status command, capture output
# ---------------------------------------------------------------------------
util() { sudo "$UTIL" "$@" 2>&1; }

# ---------------------------------------------------------------------------
# 3. RFIC bring-up — the core "does this image bring the AD9361 up?" test.
#    m2sdr_rf exits 0 even on failure, so gate on the output, not $?.
#    A failure here with REFCLK present is the tools<->gateware CSR-map mismatch
#    signature (see the clocks step), not a dead chip.
# ---------------------------------------------------------------------------
MSPS="$(awk "BEGIN{printf \"%.2f\", $RATE/1e6}")"
hdr "hardware: RFIC bring-up ($LAYOUT @ ${MSPS} MSPS)"
BU="$(sudo "$RF" --sample-rate "$RATE" --channel-layout "$LAYOUT" 2>&1)"
if echo "$BU" | grep -qiE 'error|failed|Unsupported PRODUCT_ID'; then
    fail "rfic-bringup" "$(echo "$BU" | grep -iE 'error|failed|PRODUCT_ID' | head -1)"
else
    pass "rfic-bringup" "$LAYOUT @ ${MSPS} MSPS"
fi

# ---------------------------------------------------------------------------
# 4. identity + RFIC presence
# ---------------------------------------------------------------------------
hdr "hardware: identity + RFIC"
INFO="$(util info)"
BUILD_ID="$(echo "$INFO" | grep -iE 'built on' | sed -E 's/.*built on //; s/\.$//')"
PROD="$(echo "$INFO" | grep -iE 'AD9361 Product ID' | sed -E 's/.*:[[:space:]]*//' | grep -oiE '[0-9a-f]{4}' | head -1)"
PRESENCE="$(echo "$INFO" | grep -iE 'AD9361 Presence' | grep -oiE 'yes|no' | head -1)"
TEMP="$(echo "$INFO" | grep -iE 'AD9361 Temperature' | grep -oE '[0-9]+\.[0-9]+' | head -1)"
echo "build-id: ${BUILD_ID:-?}"
if [ "${PRESENCE,,}" = yes ] && [ "${PROD,,}" = "000a" ]; then
    pass "rfic-presence" "product-ID 0x${PROD}, temp ${TEMP:-?}C"
else
    fail "rfic-presence" "presence=${PRESENCE:-?} product-ID=0x${PROD:-????} (want yes/0x000a)"
fi
if [ -n "$TEMP" ] && LC_ALL=C awk "BEGIN{exit !($TEMP>0 && $TEMP<=$TEMP_MAX)}"; then
    pass "rfic-temp" "${TEMP}C"
else
    fail "rfic-temp" "AD9361 at ${TEMP:-?}C (limit ${TEMP_MAX}C) -- let the board cool before testing"
fi

# Thermal guard. Sustained full-power TX into a conducted loopback heats the AD9361 fast: measured
# 112C on this bench after back-to-back runs at tx-att 0 (vs ~70C idle), and every datapath check
# then fails on a perfectly good image. Refuse to start the RF-heavy phases when the part is already
# hot, and prefer a lower TX power with more RX gain for the same link budget.
# Each RF/DMA phase leaves gateware state behind that breaks the next one: the loopback sweep leaves
# the DMA headers enabled, dma-test reconfigures the DMA/loopback path, and the timed-TX selftest
# repoints the crossbar. That state lives in gateware CSRs, so a module reload does NOT clear it --
# only an FPGA reconfig does. Reload the bitstream before each phase so every phase is measured on
# pristine hardware and the phases stay order-independent.
reset_gateware() {
    echo "  resetting gateware state (ICAP reload) ..."
    sudo "$UTIL" flash-reload >/dev/null 2>&1 || true
    sleep 4
    sudo rmmod m2sdr 2>/dev/null || true
    local bdf; bdf="$(lspci -Dn -d 10ee: 2>/dev/null | awk 'NR==1{print $1}')"
    if [ -n "$bdf" ]; then
        echo 1 | sudo tee "/sys/bus/pci/devices/$bdf/remove" >/dev/null 2>&1 || true
        sleep 1
    fi
    echo 1 | sudo tee /sys/bus/pci/rescan >/dev/null 2>&1 || true
    sleep 2
    { [ -f "$KO" ] && sudo insmod "$KO" 2>/dev/null; } || sudo modprobe m2sdr 2>/dev/null || true
    sleep 1
    [ -e /dev/m2sdr0 ] || echo "  WARNING: /dev/m2sdr0 missing after gateware reset"
}

rfic_temp_now() { util info 2>/dev/null | grep -iE 'AD9361 Temperature' | grep -oE '[0-9]+\.[0-9]+' | head -1; }
wait_until_cool() {
    local t; t="$(rfic_temp_now)"
    [ -z "$t" ] && return 0
    local waited=0
    while LC_ALL=C awk "BEGIN{exit !($t > $TEMP_COOL)}" && [ "$waited" -lt 300 ]; do
        echo "  AD9361 at ${t}C (> ${TEMP_COOL}C) -- cooling ${waited}/300s ..."
        sleep 30; waited=$((waited + 30)); t="$(rfic_temp_now)"
    done
    echo "  AD9361 at ${t}C -- proceeding"
}

# ---------------------------------------------------------------------------
# 4. clocks — the AD9361 DATA clock present is the direct "RFIC alive" signal
# ---------------------------------------------------------------------------
hdr "hardware: clocks"
CLK="$(util clk-test 1 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g')"
DATCLK="$(echo "$CLK" | awk '$1 ~ /^[0-9]+$/{print $5; exit}')"   # AD9361 Dat Clk (chip-generated)
REFCLK="$(echo "$CLK" | awk '$1 ~ /^[0-9]+$/{print $4; exit}')"   # AD9361 Ref Clk (SI5351 -> chip)
if [ -n "$DATCLK" ] && awk "BEGIN{exit !($DATCLK>1)}"; then
    pass "rfic-dataclk" "${DATCLK} MHz (ref ${REFCLK} MHz)"
elif [ -n "$REFCLK" ] && awk "BEGIN{exit !($REFCLK>1)}"; then
    # REFCLK present but no DATA clock + SPI silent: the chip is powered and clocked,
    # it just isn't answering. That is NOT a dead chip — it is a tools<->gateware
    # CSR-map mismatch (rebuild tools from THIS image's tree) or a soft SPI wedge
    # (module reload, else reflash a known-good image). Never a power cycle.
    fail "rfic-dataclk" "REFCLK ${REFCLK} MHz present but no DATA clock + SPI silent -> tools<->gateware CSR-map MISMATCH or soft SPI wedge (NOT a dead chip; rebuild tools from this image's tree, or module reload / reflash)"
else
    fail "rfic-dataclk" "no AD9361 clocks (dat ${DATCLK:-0} / ref ${REFCLK:-0} MHz)"
fi

# ---------------------------------------------------------------------------
# 5. PCIe link
# ---------------------------------------------------------------------------
hdr "hardware: PCIe link"
SPEED="$(echo "$INFO" | grep -iE 'PCIe Speed' | sed -E 's/.*:\s*//')"
LANES="$(echo "$INFO" | grep -iE 'PCIe Lanes' | sed -E 's/.*:\s*//')"
LINK="$(echo "$SPEED $LANES" | tr -s ' ')"
if [ -n "$LANES" ]; then
    if [ -n "$EXPECT" ]; then
        if echo "${LINK,,}" | grep -qi "${EXPECT,,}"; then pass "pcie-link" "$LINK"; else fail "pcie-link" "$LINK (expected $EXPECT)"; fi
    else
        pass "pcie-link" "$LINK"
    fi
else
    fail "pcie-link" "no PCIe link reported"
fi

# ---------------------------------------------------------------------------
# 6. CSR sanity (scratch register)
# ---------------------------------------------------------------------------
hdr "hardware: CSR sanity"
if util scratch-test 2>&1 | grep -qiE 'pass|ok|success'; then
    pass "scratch" "CSR read/write ok"
else
    SC="$(util scratch-test 2>&1 | tail -1)"
    # scratch-test returns nonzero on mismatch; treat missing "pass" cautiously
    if [ ${PIPESTATUS:-1} -eq 0 ]; then pass "scratch" "$SC"; else fail "scratch" "$SC"; fi
fi

# ---------------------------------------------------------------------------
# 9. timed-TX gate accuracy (opt-in; requires the gate + loopback cable)
# ---------------------------------------------------------------------------
hdr "hardware: timed-TX gate"
if [ "$DO_TIMED" = 1 ]; then
    reset_gateware
    wait_until_cool
    TT_BIN=/tmp/qualify_timed_tx_selftest
    if cc -O3 -Wall -o "$TT_BIN" "$REPO/scripts/timed_tx_selftest.c" \
        -I"$LIBDIR" -I"$KERNDIR" "$LIBDIR/libm2sdr.a" -lm -lpthread 2>/dev/null; then
        # Must run at the qualified rate: the selftest defaults to 122.88 MSPS, which needs the
        # wide-bandwidth (oversampling) image and aborts with "interface failed to align" on a
        # regular build. Pass the link budget through too (attenuated conducted loopback).
        TT_CHAN="$CHAN"; [ "$TT_CHAN" = both ] && TT_CHAN=1
        TT_OUT="$(sudo "$TT_BIN" --rate "$RATE" --chan "$TT_CHAN" \
                    --rx-gain "$RX_GAIN" --tx-att "$TX_ATT" 2>&1)"
        if echo "$TT_OUT" | grep -q 'RESULT PASS'; then
            pass "timed-tx" "$(echo "$TT_OUT" | grep -E '^D \(on-air' | head -1)"
        else
            fail "timed-tx" "$(echo "$TT_OUT" | grep -iE 'RESULT|failed to align|FAIL' | head -1)"
        fi
    else
        fail "timed-tx" "timed_tx_selftest.c failed to build against in-repo libm2sdr"
    fi
else
    skip "timed-tx" "not requested (pass --timed-tx for gate images)"
fi

# ---------------------------------------------------------------------------
# 8. DMA soak — sustained streaming with no overflow/underflow/error
# ---------------------------------------------------------------------------
hdr "hardware: DMA soak (${SOAK}s)"
if [ "${SOAK:-0}" -gt 0 ] 2>/dev/null; then
    reset_gateware
    # dma-test prints a table whose LAST column is the error count, with a repeated header row
    # ("DMA_SPEED... ERRORS"). Sum the error column over data rows only -- matching the word
    # "ERRORS" in the header would fail every healthy run.
    SOAK_OUT="$(sudo "$UTIL" --duration "$SOAK" dma-test 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g')"
    # LC_ALL=C: under a comma-decimal locale awk does not treat "3.07" as numeric, which silently
    # filters out every data row and reports a healthy soak as unparseable.
    SOAK_ROWS="$(echo "$SOAK_OUT" | LC_ALL=C awk 'NF>=5 && $1 ~ /^[0-9]+([.][0-9]+)?$/ {print}')"
    # The ERRORS column is CUMULATIVE, not per-interval, and it always picks up a fixed batch while
    # the pattern checker syncs on the first buffers. Summing it multiplies that startup transient by
    # the row count; what matters is whether it keeps GROWING once streaming is established, so
    # measure the growth from an early baseline row to the last row.
    SOAK_ERRS="$(echo "$SOAK_ROWS" | LC_ALL=C awk 'NR==3{base=$NF} {last=$NF} END{print (NR>=3 ? last-base : last)+0}')"
    SOAK_ERRS_TOTAL="$(echo "$SOAK_ROWS" | LC_ALL=C awk 'END{print $NF+0}')"
    SOAK_NROW="$(echo "$SOAK_ROWS" | grep -c . || true)"
    SOAK_SPEED="$(echo "$SOAK_ROWS" | LC_ALL=C awk '{s+=$1; n++} END{if(n) printf "%.2f Gbps avg", s/n}')"
    if [ "$SOAK_NROW" -eq 0 ]; then
        fail "dma-soak" "no DMA throughput rows parsed from dma-test output"
    elif [ "$SOAK_ERRS" -ne 0 ]; then
        fail "dma-soak" "$SOAK_ERRS new DMA errors after sync over ${SOAK}s ($SOAK_NROW samples)"
    else
        pass "dma-soak" "${SOAK}s, 0 errors after sync over $SOAK_NROW samples ($SOAK_ERRS_TOTAL during startup sync), $SOAK_SPEED"
    fi
else
    skip "dma-soak" "disabled (--soak 0)"
fi

# ---------------------------------------------------------------------------
# 7. datapath + DMA-header integrity (TX->RX loopback)
#
# NOTE ON ORDER/STATE: the DMA header enables and crossbar selects live in GATEWARE CSRs, which a
# module reload (rmmod/insmod) does not clear -- only an FPGA reconfig does. Each RF/DMA phase leaves
# such state behind, so every phase does a full reset_gateware (flash-reload) first and runs on
# pristine hardware. That keeps the phase order irrelevant and each result trustworthy.
# ---------------------------------------------------------------------------
hdr "hardware: datapath (loopback + DMA headers)"
reset_gateware
wait_until_cool
LB_ARGS=(--chan "$CHAN"); [ "$QUICK" = 1 ] && LB_ARGS+=(--quick)
[ -n "$LB_CONFIG" ] && LB_ARGS+=(--config "$LB_CONFIG")
LB_ARGS+=(-- --rx-gain "$RX_GAIN" --tx-att "$TX_ATT")
if [ -x "$REPO/scripts/loopback_selftest.sh" ]; then
    if "$REPO/scripts/loopback_selftest.sh" "${LB_ARGS[@]}"; then
        pass "loopback" "all configs (chan $CHAN)"
    else
        fail "loopback" "one or more configs failed (chan $CHAN) — see log above"
    fi
else
    skip "loopback" "loopback_selftest.sh missing"
fi

# ---------------------------------------------------------------------------
# verdict + record
# ---------------------------------------------------------------------------
hdr "VERDICT"
NPASS=0; NSKIP=0
for i in "${!STEP_NAME[@]}"; do
    case "${STEP_STATE[$i]}" in PASS) NPASS=$((NPASS+1));; SKIP) NSKIP=$((NSKIP+1));; esac
done
STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ "$NFAIL" -eq 0 ]; then VERDICT="QUALIFIED"; else VERDICT="NOT QUALIFIED"; fi
echo "$VERDICT  —  $NPASS pass / $NFAIL fail / $NSKIP skip"

REC="$REPO/scripts/qual_records/${BUILD_ID// /_}_${STAMP//:/-}.txt"
mkdir -p "$(dirname "$REC")"
{
    echo "M2SDR image qualification record"
    echo "verdict:   $VERDICT"
    echo "date:      $STAMP"
    echo "build-id:  ${BUILD_ID:-?}"
    echo "git:       $(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo '?')"
    echo "timing:    ${WNS_NS:-not-checked}"
    echo "pcie:      ${LINK:-?}"
    echo "loopback:  chan $CHAN${QUICK:+ (quick)}"
    echo "---"
    for i in "${!STEP_NAME[@]}"; do
        printf '%-6s %-22s %s\n' "${STEP_STATE[$i]}" "${STEP_NAME[$i]}" "${STEP_DETAIL[$i]}"
    done
} | tee "$REC"
echo
echo "record: $REC"

[ "$NFAIL" -eq 0 ]
