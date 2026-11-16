#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Host links and SATA storage: transceiver lanes, link cores and the stream crossbar.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    d.add(Container(180, 20, 560, 290, title="FPGA", title_pos="br"))
    host = d.add(Box(20,  40, 120, 60, label="Host\nPCIe slot", fill=COLOR_HARDWARE))
    sfp  = d.add(Box(20, 130, 120, 60, label="SFP cage\nEthernet", fill=COLOR_HARDWARE))
    ssd  = d.add(Box(20, 220, 120, 60, label="SATA SSD", fill=COLOR_HARDWARE))
    pcie = d.add(Box(215,  40, 130, 60, label="LitePCIe\nMMAP, DMA", fill=COLOR_ACCENT))
    eth  = d.add(Box(215, 130, 130, 60, label="LiteEth\nUDP, Etherbone", fill=COLOR_ACCENT))
    sata = d.add(Box(215, 220, 130, 60, label="LiteSATA\nstreamers", fill=COLOR_ACCENT))
    xbar = d.add(Box(425,  40, 110, 240, label="Crossbar", fill=COLOR_ACCENT_LIGHT))
    rfic = d.add(Box(600, 130, 115, 60, label="TX and RX\nstreams"))
    d.add(Arrow((host, "r"), (pcie, "l"), kind="data", heads="both"))
    d.add(Arrow((sfp,  "r"), (eth,  "l"), kind="data", heads="both"))
    d.add(Arrow((ssd,  "r"), (sata, "l"), kind="data", heads="both"))
    d.add(Arrow((pcie, "r"), (425,  70), kind="data", heads="both", label="port 0"))
    d.add(Arrow((eth,  "r"), (425, 160), kind="data", heads="both", label="port 1"))
    d.add(Arrow((sata, "r"), (425, 250), kind="data", heads="both", label="port 2"))
    d.add(Arrow((xbar, "r"), (rfic, "l"), kind="accent", heads="both"))
    return d
