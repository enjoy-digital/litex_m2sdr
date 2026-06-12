#!/bin/bash
# No-reboot M2SDR gateware reflash (proven recipe, 2026-06-10):
# flash-write -> ICAP flash-reload (FPGA reconfigures, PCIe retrains by
# itself) -> driver unload -> PCIe remove/rescan -> driver reload.
#
# Usage: scripts/flash_gateware.sh <image.bin> [offset]
#   offset: 0x800000 (default) = x4/main image slot; 0x0 = fallback slot
#           (an x2 fallback image lives at 0x0 on this board).
#
# After this, REBUILD the user software if the CSR map changed
# (make -C litex_m2sdr/software/user) and relink any tools that statically
# link libm2sdr.a.
#
# If the device does not reappear: pulse Secondary Bus Reset on bridge
# 04:00.0 (setpci -s 04:00.0 BRIDGE_CONTROL bit 0x40) before rescan.
# If config space is dead (D3cold, "Unsupported device version 255"):
# only a full power cycle helps.
set -e
U="$(dirname "$0")/../litex_m2sdr/software/user"
BIN=${1:?usage: flash_gateware.sh image.bin [offset]}
OFF=${2:-0x800000}
PCIDEV=0000:05:00.0

echo YES | "$U/m2sdr_util" flash-write "$BIN" "$OFF"
"$U/m2sdr_util" flash-reload || true
sleep 4
sudo rmmod m2sdr 2>/dev/null || true
echo 1 | sudo tee /sys/bus/pci/devices/$PCIDEV/remove >/dev/null
sleep 1
echo 1 | sudo tee /sys/bus/pci/rescan >/dev/null
sleep 2
sudo modprobe m2sdr
sleep 1
lspci -d 10ee:
sudo lspci -s ${PCIDEV#0000:} -vv | grep LnkSta: || true
ls -l /dev/m2sdr0
