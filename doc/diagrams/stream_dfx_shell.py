#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# DFX build: the static shell rfic_dfx, as LiteXHierarchyExplorer lists it, registers every stream
# at the partition boundary around the black box that each reconfigurable module fills.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 300)
    d.add(Container(90, 20, 580, 265, title="rfic_dfx", title_pos="br"))
    w, h = 80, 36
    tx_in  = d.add(Box(120,  40, w, h, label="tx_in"))
    rx_out = d.add(Box(120, 100, w, h, label="rx_out"))
    sb_in  = d.add(Box(120, 165, w, h, label="sb_in"))
    sb_out = d.add(Box(120, 225, w, h, label="sb_out"))
    rm     = d.add(Box(280, 40, 200, 221, label="m2sdr_dfx_datapath\nblack box", fill=COLOR_ACCENT))
    tx_out = d.add(Box(560,  40, w, h, label="tx_out"))
    rx_in  = d.add(Box(560, 100, w, h, label="rx_in"))
    d.add(Arrow((10, 58), (tx_in, "l"), kind="data", label="Host TX"))
    d.add(Arrow((rx_out, "l"), (10, 118), kind="data", label="Host RX"))
    d.add(Arrow((10, 183), (sb_in, "l"), kind="data", label="DMA1"))
    d.add(Arrow((sb_out, "l"), (10, 243), kind="data", label="DMA1"))
    d.add(Arrow((tx_in, "r"), (280, 58), kind="accent"))
    d.add(Arrow((280, 118), (rx_out, "r"), kind="accent"))
    d.add(Arrow((sb_in, "r"), (280, 183), kind="accent"))
    d.add(Arrow((280, 243), (sb_out, "r"), kind="accent"))
    d.add(Arrow((480, 58), (tx_out, "l"), kind="accent"))
    d.add(Arrow((rx_in, "l"), (480, 118), kind="accent"))
    d.add(Arrow((tx_out, "r"), (750, 58), kind="data", label="RFIC TX"))
    d.add(Arrow((750, 118), (rx_in, "r"), kind="data", label="RFIC RX"))
    return d
