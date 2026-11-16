#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Oversampling PHY: OSERDESE2 serializers on TX, per-lane IDELAYE2 deskew on RX.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 300)
    chip = d.add(Box(640, 30, 105, 246, label="AD9361", fill=COLOR_HARDWARE))
    fifo = d.add(Box(120, 30, 110, 56, label="Group\nFIFO", fill=COLOR_ACCENT_LIGHT))
    ser  = d.add(Box(370, 30, 120, 56, label="OSERDESE2 x7\n8:1 DDR", fill=COLOR_ACCENT))
    mmcm = d.add(Box(370, 125, 120, 44, label="MMCM"))
    dly  = d.add(Box(370, 220, 120, 56, label="IDELAYE2 x6\nper lane", fill=COLOR_ACCENT))
    cap  = d.add(Box(120, 220, 130, 56, label="IDDR,\nslot decode"))
    cnt  = d.add(Box(120, 130, 130, 44, label="Lane error\ncounters", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((10, 58), (fifo, "l"), kind="data", label="TX groups", label_side="above"))
    d.add(Arrow((fifo, "r"), (ser, "l"), kind="data", label="122.88 MHz", label_side="above"))
    d.add(Arrow((ser, "r"), (chip, "l", (58 - 30)/246), kind="data", label="983 Mbps per pair",
        label_side="above"))
    d.add(Arrow((chip, "l", (147 - 30)/246), (mmcm, "r"), kind="control", label="DATA_CLK",
        label_side="above"))
    d.add(Arrow((mmcm, "t"), (ser, "b"), kind="control", label="CLK, CLKDIV", label_side="right"))
    d.add(Arrow((chip, "l", (248 - 30)/246), (dly, "r"), kind="data", label="RX pairs",
        label_side="above"))
    d.add(Arrow((dly, "l"), (cap, "r"), kind="data"))
    d.add(Arrow((cap, "l"), (10, 248), kind="data", label="RX groups", label_side="above"))
    d.add(Arrow((cap, "t"), (cnt, "b"), kind="control"))
    return d
