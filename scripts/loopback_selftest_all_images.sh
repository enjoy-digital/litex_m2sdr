#!/usr/bin/env bash
# Full two-image loopback regression.
#
# The board needs two different gateware images to cover every TX mode (one
# bitstream cannot: the Oversampling image's TX serializer MMCM locks only at
# 491.52MHz DATA_CLK, the base image's ODDR TX eye is too poor at 983Mbps). So
# this driver flashes each image in turn, rebuilds the host software against
# THAT image's CSR map, and runs scripts/loopback_selftest.sh for the configs
# the image supports:
#
#   1. 122.88     (Oversampling + OSERDES): 2T2R@122.88 TX+RX (+ RX-only).
#   2. lower-rates (base):                  1T1R + 2T2R@<=61.44, sc8 + sc16.
#
# It reuses the proven no-reboot reflash (scripts/flash_gateware.sh) and the
# per-config test engine (scripts/loopback_selftest.sh) - it adds only the
# image sequencing, so there are no duplicate tests.
#
# Both images must be built first (build/<name>/). Requires the TX/RX loopback
# cable on the port pair given by --chan (default 2, i.e. TX2 -> RX2), and the
# same passwordless-sudo the reflash needs.
#
# Usage: scripts/loopback_selftest_all_images.sh [--chan 1|2|both] [--quick]
#                                                [--leave-on 122.88|lower-rates|none]
#   --leave-on : which image to reflash + rebuild software for at the end so the
#                board and the working tree are left consistent (default 122.88,
#                the branch's committed CSR map). 'none' leaves the board on the
#                last image tested (working tree left on that image's headers).

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNDIR="$REPO/litex_m2sdr/software/kernel"
USERDIR="$REPO/litex_m2sdr/software/user"

CHAN=2
QUICK=""
LEAVE_ON=122.88
while [ $# -gt 0 ]; do
    case "$1" in
        --chan)     CHAN="$2"; shift 2 ;;
        --quick)    QUICK="--quick"; shift ;;
        --leave-on) LEAVE_ON="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# label | build subdir under build/ | bin basename | selftest configs (space-sep; empty = default matrix)
IMAGES=(
    "122.88|litex_m2sdr_m2_pcie_x4_rfic_oversampling|litex_m2sdr_m2_pcie_x4_rfic_oversampling.bin|2t2r-122.88 2t2r-122.88-rx"
    "lower-rates|litex_m2sdr_m2_pcie_x4|litex_m2sdr_m2_pcie_x4.bin|"
)

img_field() { echo "$1" | cut -d'|' -f"$2"; }

# Switch the board + host software to one image: flash its bitstream, sync its
# CSR headers into the kernel dir, and rebuild the lib + tools against them.
#
# Headers come from build/<subdir>/kernel/ - a per-image snapshot of the
# self-contained kernel-flavor headers (csr.h without the <generated/soc.h>
# include that the modular build/<subdir>/software/include/generated/ ones carry;
# the user Makefile includes -I../kernel and expects the self-contained flavor).
# Populate it once per image with:  cp litex_m2sdr/software/kernel/{csr,soc,mem}.h
# build/<subdir>/kernel/   right after that image is built.
activate_image() {
    local subdir="$1" bin="$2"
    local hdr="$REPO/build/$subdir/kernel"
    local binpath="$REPO/build/$subdir/gateware/$bin"
    [ -f "$binpath" ] || { echo "MISSING bitstream: $binpath" >&2; return 1; }
    for h in csr soc mem; do
        [ -f "$hdr/$h.h" ] || { echo "MISSING header snapshot: $hdr/$h.h" >&2; return 1; }
    done
    echo ">>> flashing $binpath"
    "$REPO/scripts/flash_gateware.sh" "$binpath" 0x800000 || return 1
    echo ">>> syncing CSR map + rebuilding host software"
    cp "$hdr"/{csr,soc,mem}.h "$KERNDIR/"
    make -C "$USERDIR" libm2sdr/libm2sdr.a m2sdr_util m2sdr_rf >/dev/null || {
        echo "SW REBUILD FAILED for $subdir" >&2; return 1; }
    return 0
}

declare -A RESULT
overall=0
for entry in "${IMAGES[@]}"; do
    label="$(img_field "$entry" 1)"
    subdir="$(img_field "$entry" 2)"
    bin="$(img_field "$entry" 3)"
    configs="$(img_field "$entry" 4)"

    echo
    echo "############################################################"
    echo "# IMAGE: $label"
    echo "############################################################"
    if ! activate_image "$subdir" "$bin"; then
        RESULT[$label]="SETUP-FAIL"; overall=1; continue
    fi

    rc=0
    if [ -z "$configs" ]; then
        "$REPO/scripts/loopback_selftest.sh" --chan "$CHAN" $QUICK || rc=1
    else
        # explicit per-image configs (the on-demand oversampling ones)
        for cfg in $configs; do
            "$REPO/scripts/loopback_selftest.sh" --chan "$CHAN" --config "$cfg" || rc=1
        done
    fi
    RESULT[$label]=$([ $rc -eq 0 ] && echo PASS || echo FAIL)
    [ $rc -ne 0 ] && overall=1
done

# Leave the board + tree consistent on the requested image.
if [ "$LEAVE_ON" != none ]; then
    for entry in "${IMAGES[@]}"; do
        if [ "$(img_field "$entry" 1)" = "$LEAVE_ON" ]; then
            echo
            echo ">>> restoring board to '$LEAVE_ON'"
            activate_image "$(img_field "$entry" 2)" "$(img_field "$entry" 3)" || overall=1
        fi
    done
fi

echo
echo "===== all-images summary ====="
for entry in "${IMAGES[@]}"; do
    label="$(img_field "$entry" 1)"
    printf "%-14s %s\n" "$label" "${RESULT[$label]:-SKIP}"
done
exit $overall
