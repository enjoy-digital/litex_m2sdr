#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# The same board over its links: one SDR datapath, one LiteX core per link.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 300)
    d.add(Container(10, 10, 415, 280, title="LiteX-M2SDR", title_pos="tl"))
    d.add(Container(135, 22, 278, 256, title="FPGA", title_pos="bl", title_size=15, fill="#f6f6f6"))
    rfic = d.add(Box(25, 115, 90, 50, label="AD9361", fill=COLOR_HARDWARE))
    path = d.add(Box(150, 115, 105, 50, label="SDR\ndatapath", fill=COLOR_ACCENT))
    d.add(Arrow((rfic, "r"), (path, "l"), kind="data", heads="both"))
    d.add(Bus([(255, 140), (270, 140)], kind="data"))
    d.add(Bus([(270, 55), (270, 235)], kind="data"))
    links = [
        ("LitePCIe",  "PCIe",     "Host, M.2 slot"),
        ("LiteEth",   "Ethernet", "Host, network"),
        ("USB3-PIPE", "USB3",     "Host, USB 3 port"),
        ("LiteSATA",  "SATA",     "SSD"),
    ]
    for i, (core, link, peer) in enumerate(links):
        y = 35 + 60*i
        c = d.add(Box(290, y, 110, 40, label=core, fill=COLOR_ACCENT_LIGHT))
        p = d.add(Box(590, y, 155, 40, label=peer, fill=COLOR_HARDWARE))
        d.add(Arrow((270, y + 20), (c, "l"), kind="data", heads="both"))
        d.add(Arrow((c, "r"), (p, "l"), kind="accent", heads="both", label=link))
    return d
