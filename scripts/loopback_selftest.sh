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
#
# The matrix is split by the gateware build that can run each config, because
# TX has no single bitstream that covers every rate:
#   - STANDARD build (default, non-oversampling): TX+RX for every rate whose
#     DATA_CLK stays <= 245.76MHz - i.e. all 1T1R rates and 2T2R <= 61.44. These
#     are CONFIGS below.
#   - OVERSAMPLING build (--with-rfic-oversampling, OSERDESE2 TX serializer):
#     the only build that transmits at 2T2R@122.88 (DATA_CLK 491.52MHz, 983Mbps
#     lanes via the OSERDESE2 serializer). Its TX MMCM is fixed for that DATA_CLK
#     and locks only over 184.62-492.31MHz, so this image covers DATA_CLK 245.76
#     too (2T2R@61.44) but nothing below - libm2sdr refuses those outright. These
#     are OVERSAMPLING_CONFIGS.
# The default run auto-selects the group for the LOADED image (the
# rfic_oversampling capability bit via m2sdr_util info); --config picks one
# config explicitly.
CONFIGS=(
    "1t1r-30.72-sc16  | --layout 1t1r --rate 30720000  "
    "1t1r-61.44-sc16  | --layout 1t1r --rate 61440000  "
    "2t2r-61.44-sc16  | --layout 2t2r --rate 61440000  "
    "1t1r-122.88-sc16 | --layout 1t1r --rate 122880000 "
    "1t1r-122.88-sc8  | --layout 1t1r --rate 122880000 --sc8"
)
# Oversampling+OSERDES build only (run on demand: --config 2t2r-122.88[-*]).
# 2t2r-122.88     : full TX+RX loopback through the OSERDESE2 TX serializer
#                   (983Mbps lanes; an ODDR TX eye is too poor at this rate).
# 2t2r-122.88-rx  : RX-only, to isolate the host-RX drop ceiling from TX. At
#                   2T2R@122.88 the RX-to-host path drops ~17% of samples (sc8
#                   and sc16 both measure ~1.208x the nominal buffer period):
#                   the sys-domain RX datapath tops out at ~101.7M frames/s. The
#                   FPGA<->AD9361 link is clean (PRBS/deskew verified); the
#                   ceiling is internal and only the DMA-header timestamps make
#                   the loss visible. This drop also perturbs the full-loopback
#                   sample-continuity check, so treat 2t2r-122.88 TX results via
#                   the tone/SNR metrics, not strict phase continuity.
OVERSAMPLING_CONFIGS=(
    "2t2r-122.88      | --layout 2t2r --rate 122880000 --sc8"
    "2t2r-122.88-rx   | --layout 2t2r --rate 122880000 --no-tx --sc8"
    # DATA_CLK 245.76MHz: the low end of the TX MMCM's lock range on this image, where the
    # OSERDESE2 lanes run 491.52Mbps and the RX IDELAYE2 insertion delay is compensated by the
    # in-spec delay midpoints rather than a per-lane deskew.
    "2t2r-61.44       | --layout 2t2r --rate 61440000"
)
# 1t1r-122.88-* from CONFIGS also runs on the oversampling image (DATA_CLK 245.76MHz as well;
# verified 25/25 on the bench). It stays in the standard group so --config picks one entry.
if [ "$QUICK" = 1 ]; then
    CONFIGS=(
        "1t1r-61.44-sc16  | --layout 1t1r --rate 61440000  "
        "1t1r-122.88-sc16 | --layout 1t1r --rate 122880000 "
    )
fi

# Default matrix = the group the LOADED image supports: the rfic_oversampling
# capability bit distinguishes the two builds (the oversampling image cannot
# transmit at the standard rates, and vice versa for 2T2R@122.88).
UTIL="$REPO/litex_m2sdr/software/user/m2sdr_util"
DEFAULT_CONFIGS=("${CONFIGS[@]}")
if [ -z "$ONLY" ] && [ -x "$UTIL" ] && "$UTIL" info 2>/dev/null | grep -q "Oversampling   : Yes"; then
    echo "Oversampling image detected -> running the 2T2R@122.88 config group."
    DEFAULT_CONFIGS=("${OVERSAMPLING_CONFIGS[@]}")
fi

declare -A RESULT
FAILED=0
for entry in "${CONFIGS[@]}" "${OVERSAMPLING_CONFIGS[@]}"; do
    name="${entry%%|*}"; name="${name// /}"
    args="${entry#*|}"
    if [ -n "$ONLY" ]; then
        [ "$name" != "$ONLY" ] && continue
    else
        # Run only the group the loaded image supports.
        in_default=0
        for x in "${DEFAULT_CONFIGS[@]}"; do
            xn="${x%%|*}"; xn="${xn// /}"
            [ "$name" = "$xn" ] && in_default=1
        done
        [ "$in_default" = 0 ] && continue
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
for entry in "${CONFIGS[@]}" "${OVERSAMPLING_CONFIGS[@]}"; do
    name="${entry%%|*}"; name="${name// /}"
    [ -z "${RESULT[$name]:-}" ] && continue
    printf "%-20s %s\n" "$name" "${RESULT[$name]}"
done
exit $FAILED
