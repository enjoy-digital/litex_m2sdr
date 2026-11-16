#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Shared QPLL: two PLLs of the GTP quad for three possible links.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    pcie = d.add(Box(20,  40, 150, 60, label="PCIe\nrefclk_pcie"))
    eth  = d.add(Box(20, 135, 150, 60, label="Ethernet, WR\nrefclk_eth or sys"))
    sata = d.add(Box(20, 230, 150, 60, label="SATA\nrefclk_sata"))
    d.add(Container(230, 20, 300, 290, title="SharedQPLL", title_pos="br"))
    sel  = d.add(Box(255,  95, 100, 140, label="Selection"))
    pll0 = d.add(Box(405,  60, 100, 60, label="PLL 0", fill=COLOR_ACCENT))
    pll1 = d.add(Box(405, 175, 100, 60, label="PLL 1", fill=COLOR_ACCENT))
    gt0  = d.add(Box(590,  60, 150, 60, label="Transceivers\nof the first link", fill=COLOR_HARDWARE))
    gt1  = d.add(Box(590, 175, 150, 60, label="Transceiver\nof the second link", fill=COLOR_HARDWARE))
    d.add(Arrow((pcie, "r"), (sel, "l", 0.2), kind="control"))
    d.add(Arrow((eth,  "r"), (sel, "l", 0.5), kind="control"))
    d.add(Arrow((sata, "r"), (sel, "l", 0.8), kind="control"))
    d.add(Arrow((sel, "r", 0.25), (pll0, "l"), kind="control"))
    d.add(Arrow((sel, "r", 0.75), (pll1, "l"), kind="control"))
    d.add(Arrow((pll0, "r"), (gt0, "l"), kind="accent", label="channel 0", label_pos=(562, 78)))
    d.add(Arrow((pll1, "r"), (gt1, "l"), kind="accent", label="channel 1", label_pos=(562, 193)))
    return d
