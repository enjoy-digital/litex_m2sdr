#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# SATA storage: LiteSATA core, crossbar ports, DMA engines and stream recorder/player.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 420)
    ssd  = d.add(Box(20, 175, 90, 70, label="SATA\nSSD", fill=COLOR_HARDWARE))
    core = d.add(Box(150, 175, 120, 70, label="LiteSATA\nPHY and core", fill=COLOR_ACCENT_LIGHT))
    xbar = d.add(Box(310, 40, 110, 340, label="LiteSATA\ncrossbar", fill=COLOR_ACCENT_LIGHT))
    idn  = d.add(Box(490,  40, 130, 55, label="Identify"))
    s2m  = d.add(Box(490, 115, 130, 55, label="Sector2Mem", fill=COLOR_ACCENT))
    m2s  = d.add(Box(490, 190, 130, 55, label="Mem2Sector", fill=COLOR_ACCENT))
    rec  = d.add(Box(490, 265, 130, 55, label="Stream2Sectors", fill=COLOR_ACCENT))
    ply  = d.add(Box(490, 340, 130, 55, label="Sectors2Stream", fill=COLOR_ACCENT))
    buf  = d.add(Box(660, 115, 85, 130, label="Memory"))
    d.add(Arrow((ssd, "r"), (core, "l"), kind="data", heads="both"))
    d.add(Arrow((core, "r"), (310, 210), kind="data", heads="both"))
    d.add(Arrow((420,  67.5), (idn, "l"), kind="control", heads="both"))
    d.add(Arrow((420, 142.5), (s2m, "l"), kind="data"))
    d.add(Arrow((m2s, "l"), (420, 217.5), kind="data"))
    d.add(Arrow((rec, "l"), (420, 292.5), kind="data"))
    d.add(Arrow((420, 367.5), (ply, "l"), kind="data"))
    d.add(Arrow((s2m, "r"), (660, 142.5), kind="data"))
    d.add(Arrow((660, 217.5), (m2s, "r"), kind="data"))
    d.add(Arrow((745, 292.5), (rec, "r"), kind="accent", label="RX stream"))
    d.add(Arrow((ply, "r"), (745, 367.5), kind="accent", label="TX stream"))
    return d
