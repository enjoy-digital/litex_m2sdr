#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# SoC blocks: the sample path between the host links and the RFIC, and the control blocks.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    d.add(Container(125, 15, 510, 300, title="FPGA", title_pos="tl"))
    host  = d.add(Box(15, 70, 90, 60, label="Host", fill=COLOR_HARDWARE))
    links = d.add(Box(140, 70, 95, 60, label="Host\nlinks"))
    xbar  = d.add(Box(265, 70, 85, 60, label="Crossbar"))
    head  = d.add(Box(380, 70, 95, 60, label="Header,\nloopback", fill=COLOR_ACCENT_LIGHT))
    rfic  = d.add(Box(505, 70, 115, 60, label="RFIC core", fill=COLOR_ACCENT))
    chip  = d.add(Box(655, 70, 90, 60, label="AD9361", fill=COLOR_HARDWARE))
    brid  = d.add(Box(140, 190, 95, 60, label="Host\nbridges"))
    time  = d.add(Box(265, 190, 85, 60, label="Board\ntime", fill=COLOR_ACCENT_LIGHT))
    clk   = d.add(Box(380, 190, 95, 60, label="SI5351\ncontrol"))
    util  = d.add(Box(505, 190, 115, 60, label="Flash, ICAP,\nXADC, LED"))
    for a, b in [(host, links), (links, xbar), (xbar, head), (head, rfic), (rfic, chip)]:
        d.add(Arrow((a, "r"), (b, "l"), kind="data", heads="both"))
    d.add(Arrow((links, "b"), (brid, "t"), kind="control", heads="both", label="registers",
        label_side="right"))
    d.add(Arrow((host, "b"), (brid, "l"), via=[(host.cx, brid.cy)], kind="control", heads="both",
        label="JTAG", label_pos=(88, 170)))
    d.add(Arrow((time, "t", 0.8), (head, "b", 0.2), kind="control", label="timestamps",
        label_pos=(440, 150)))
    # CSR bus trunk under the control blocks.
    d.add(Bus([(brid.cx, 250), (brid.cx, 280), (util.cx, 280), (util.cx, 250)]))
    d.add(Bus([(time.cx, 250), (time.cx, 280)]))
    d.add(Bus([(clk.cx, 250), (clk.cx, 280)]))
    d.add(Text(375, 295, "CSR bus", size=FONT_SIZE_SM))
    return d
