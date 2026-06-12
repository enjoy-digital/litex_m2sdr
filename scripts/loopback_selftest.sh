#!/usr/bin/env bash
# M2SDR cable-loopback regression sweep.
#
# Builds scripts/loopback_selftest.c against the IN-REPO libm2sdr (never the
# installed copy) and runs it across the supported configuration matrix,
# verifying transmission correctness, DMA-header timestamps (including
# host-drop recovery) and stream restart alignment for each.
#
# Requires loopback cable(s) between the TX/RX port pair(s) given by --chan
# (default: port pair 2, i.e. TX2 -> RX2).
#
# Usage: scripts/loopback_selftest.sh [--chan 1|2|both] [--quick] [--config NAME]
#        extra engine flags can be passed after --

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBDIR="$REPO/litex_m2sdr/software/user/libm2sdr"
KERNDIR="$REPO/litex_m2sdr/software/kernel"
ENGINE=/tmp/loopback_selftest

CHAN=2
QUICK=0
ONLY=""
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --chan)   CHAN="$2"; shift 2 ;;
        --quick)  QUICK=1; shift ;;
        --config) ONLY="$2"; shift 2 ;;
        --)       shift; EXTRA=("$@"); break ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$LIBDIR/libm2sdr.a" ]; then
    echo "libm2sdr.a not built - build the user software first" >&2
    exit 1
fi

cc -O3 -Wall -Wextra -o "$ENGINE" "$REPO/scripts/loopback_selftest.c" \
   -I"$LIBDIR" -I"$KERNDIR" "$LIBDIR/libm2sdr.a" -lm -lpthread || exit 1

# name | engine args
# 2T2R @ 122.88 is RX-only: the AD9361 TX datapath scrambles in the
# 2R2T-oversampled mode (chip-internal limitation, see RESULTS_overclock.md).
CONFIGS=(
    "1t1r-30.72-sc16  | --layout 1t1r --rate 30720000  "
    "1t1r-61.44-sc16  | --layout 1t1r --rate 61440000  "
    "2t2r-61.44-sc16  | --layout 2t2r --rate 61440000  "
    "1t1r-122.88-sc16 | --layout 1t1r --rate 122880000 "
    "1t1r-122.88-sc8  | --layout 1t1r --rate 122880000 --sc8"
)
# KNOWN LIMITATION (run on demand with --config 2t2r-122.88-rx): 2T2R @
# 122.88 MSPS RX-to-host drops ~17% of samples regardless of format (sc16
# and sc8 both measure ~1.208x the nominal buffer period): the sys-domain
# RX datapath tops out at ~101.7M frames/s. The FPGA<->AD9361 interface is
# clean at this rate (PRBS/deskew verified); the ceiling is internal, and
# only the DMA-header timestamps make the loss visible. TX is additionally
# blocked chip-internally (see RESULTS_overclock.md), hence --no-tx.
EXTRA_CONFIGS=(
    "2t2r-122.88-rx   | --layout 2t2r --rate 122880000 --no-tx --sc8"
)
if [ "$QUICK" = 1 ]; then
    CONFIGS=(
        "1t1r-61.44-sc16  | --layout 1t1r --rate 61440000  "
        "1t1r-122.88-sc16 | --layout 1t1r --rate 122880000 "
    )
fi

declare -A RESULT
FAILED=0
for entry in "${CONFIGS[@]}" "${EXTRA_CONFIGS[@]}"; do
    name="${entry%%|*}"; name="${name// /}"
    args="${entry#*|}"
    if [ -n "$ONLY" ]; then
        [ "$name" != "$ONLY" ] && continue
    else
        # EXTRA_CONFIGS run only when explicitly selected.
        skip_extra=0
        for x in "${EXTRA_CONFIGS[@]}"; do
            xn="${x%%|*}"; xn="${xn// /}"
            [ "$name" = "$xn" ] && skip_extra=1
        done
        [ "$skip_extra" = 1 ] && continue
    fi
    echo
    echo "===== $name ====="
    # shellcheck disable=SC2086
    if "$ENGINE" $args --chan "$CHAN" "${EXTRA[@]+"${EXTRA[@]}"}"; then
        RESULT[$name]=PASS
    else
        RESULT[$name]=FAIL
        FAILED=1
    fi
done

echo
echo "===== summary ====="
for entry in "${CONFIGS[@]}" "${EXTRA_CONFIGS[@]}"; do
    name="${entry%%|*}"; name="${name// /}"
    [ -z "${RESULT[$name]:-}" ] && continue
    printf "%-20s %s\n" "$name" "${RESULT[$name]}"
done
exit $FAILED
