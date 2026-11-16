#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# LitePCIe in a nutshell: a memory-mapped path to the registers and DMA channels for the streams.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 240)
    d.add(Container(10, 10, 200, 220, title="Host", title_pos="bl"))
    d.add(Container(290, 10, 460, 220, title="FPGA", title_pos="br"))
    app = d.add(Box(30, 30, 160, 40, label="Application"))
    drv = d.add(Box(30, 110, 160, 40, label="Kernel driver", fill=COLOR_ACCENT_LIGHT))
    phy = d.add(Box(310, 30, 90, 150, label="PCIe PHY", fill=COLOR_HARDWARE))
    d.add(Arrow((app, "b"), (drv, "t"), kind="data", heads="both"))
    d.add(Arrow((drv, "r"), (310, 130), kind="accent", heads="both", label="PCIe"))
    rows = [
        ("MMAP",       "Registers", "both", "control"),
        ("DMA writer", "RX stream", "start", "data"),
        ("DMA reader", "TX stream", "end",  "data"),
    ]
    for i, (core, peer, heads, kind) in enumerate(rows):
        y = 30 + 55*i
        c = d.add(Box(440, y, 110, 40, label=core, fill=COLOR_ACCENT))
        p = d.add(Box(610, y, 110, 40, label=peer))
        d.add(Arrow((400, y + 20), (c, "l"), kind=kind, heads=heads))
        d.add(Arrow((c, "r"), (p, "l"), kind=kind, heads=heads))
    return d
