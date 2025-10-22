#!/bin/bash
# Smart reinitialization script for the m2sdr driver
# Author: Ismail Essaidi
# Purpose: Automatically reload the m2sdr kernel module if m2sdr_util fails to init.

set -e  # Exit on errors
set -o pipefail

echo "──────────────────────────────────────────────"
echo " Checking m2sdr driver status..."
echo "──────────────────────────────────────────────"

INFO_OUTPUT=$(m2sdr_util info 2>&1 || true)

if echo "$INFO_OUTPUT" | grep -q "Could not init driver"; then
    echo "  Driver initialization failed: 'Could not init driver' detected."
    echo "→ Attempting to reload the m2sdr kernel module..."
    echo

    echo " Current m2sdr module status:"
    lsmod | grep m2 || echo "(not currently loaded)"
    echo

    echo "Removing m2sdr module..."
    if sudo rmmod m2sdr 2>/dev/null; then
        echo " Successfully removed m2sdr module."
    else
        echo "  Could not remove m2sdr (maybe not loaded?). Continuing..."
    fi

    echo
    echo "Verifying module removal..."
    if lsmod | grep -q m2; then
        echo "m2sdr still appears in lsmod! Something went wrong."
    else
        echo "m2sdr successfully removed (no match in lsmod)."
    fi

    echo
    echo "Re-inserting m2sdr module..."
    if sudo modprobe m2sdr; then
        echo "m2sdr reloaded successfully."
    else
        echo "Failed to modprobe m2sdr. Please check dmesg for details."
        exit 1
    fi

    echo
    echo "📋 Verifying that m2sdr is loaded:"
    lsmod | grep m2 || echo "m2sdr not found after modprobe."
    echo

    echo "Retesting driver initialization..."
    m2sdr_util info || echo " Still failing. Check kernel logs (dmesg)."

    echo
    echo "Done. m2sdr reload sequence complete."
    echo "──────────────────────────────────────────────"

else
    echo "Driver initialized correctly — no action needed."
    echo "──────────────────────────────────────────────"
fi
