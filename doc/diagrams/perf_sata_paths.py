#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# SATA measurements: the RF path and the two host paths to the SSD.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 310)
    rf   = d.add(Box( 15,  40, 100, 60, label="RFIC", fill=COLOR_HARDWARE))
    host = d.add(Box( 15, 170, 100, 60, label="Host", fill=COLOR_HARDWARE))
    buf  = d.add(Box(165, 170, 100, 60, label="Staging\nbuffer"))
    s2s  = d.add(Box(315,  40, 140, 60, label="Stream2Sectors", fill=COLOR_ACCENT))
    dma  = d.add(Box(315, 170, 140, 60, label="Mem2Sector\nSector2Mem", fill=COLOR_ACCENT))
    sata = d.add(Box(510,  40, 100, 190, label="LiteSATA", fill=COLOR_ACCENT_LIGHT))
    ssd  = d.add(Box(660,  40,  85, 190, label="SATA\nSSD", fill=COLOR_HARDWARE))
    d.add(Arrow((rf, "r"), (s2s, "l"), kind="accent", label="RX stream"))
    d.add(Arrow((s2s, "r"), (510, 70), kind="accent"))
    d.add(Arrow((host, "r"), (buf, "l"), kind="data", heads="both", label="Etherbone", label_pos=(140, 158)))
    d.add(Arrow((buf, "r"), (dma, "l"), kind="data", heads="both"))
    d.add(Arrow((dma, "r"), (510, 200), kind="data", heads="both"))
    d.add(Arrow((sata, "r"), (ssd, "l"), kind="data", heads="both"))
    d.add(Arrow((host, "b"), (dma, "b"), kind="data", heads="both", via=[(65, 275), (385, 275)],
        label="PCIe DMA", label_pos=(225, 265)))
    return d
