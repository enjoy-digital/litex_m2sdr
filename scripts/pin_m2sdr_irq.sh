#!/usr/bin/env bash
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Pin the LiteX-M2SDR PCIe MSI IRQ(s) to a CPU core, co-locating the DMA-completion
# interrupt with the (RT-pinned) radio loop. For low-latency timed TX (k2=1) the host
# must stay tightly ahead of the free-running DMA reader; keeping the completion IRQ on
# the SAME core as the loop is what makes that deterministic (banishing it to a far core
# makes drops WORSE). The affinity resets on every module reload (each insmod puts it
# back on a default core), so re-run this after each reload -- or wire it into your
# launch wrapper / a udev rule (ACTION=="add", KERNEL=="m2sdr*").
#
#   Usage: sudo scripts/pin_m2sdr_irq.sh [core]
#          core defaults to $DD_RADIO_CPU, else 2 (an isolcpus/nohz_full core here).
#
set -euo pipefail

core="${1:-${DD_RADIO_CPU:-2}}"

# Every /proc/interrupts line whose label mentions the m2sdr driver (usually one MSI).
irqs="$(awk -F: '/m2sdr/ { gsub(/[^0-9]/, "", $1); print $1 }' /proc/interrupts)"
if [ -z "$irqs" ]; then
    echo "pin_m2sdr_irq: no m2sdr IRQ in /proc/interrupts (is the module loaded?)" >&2
    exit 1
fi

rc=0
for irq in $irqs; do
    if echo "$core" > "/proc/irq/${irq}/smp_affinity_list" 2>/dev/null; then
        echo "pin_m2sdr_irq: IRQ ${irq} -> core $(cat /proc/irq/${irq}/smp_affinity_list)"
    else
        echo "pin_m2sdr_irq: FAILED to pin IRQ ${irq} to core ${core} (need root? managed IRQ?)" >&2
        rc=1
    fi
done
exit $rc
