#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Sample path: TX from the host links to the RFIC core, RX back, with the board time.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 265)
    y, h = 40, 120
    links = d.add(Box(15,  y, 100, h, label="PCIe DMA\nEthernet UDP\nSATA"))
    xbar  = d.add(Box(150, y, 90,  h, label="Crossbar"))
    head  = d.add(Box(275, y, 110, h, label="Header\nTX: timed gate\nRX: timestamp", size=13,
        fill=COLOR_ACCENT))
    loop  = d.add(Box(420, y, 95,  h, label="Loopback"))
    hook  = d.add(Box(550, y, 80,  h, label="User\nhook", fill=COLOR_ACCENT_LIGHT))
    rfic  = d.add(Box(665, y, 80,  h, label="RFIC\ncore"))
    time  = d.add(Box(275, 205, 110, 44, label="Board time"))
    chain = [links, xbar, head, loop, hook, rfic]
    for i, (a, b) in enumerate(zip(chain, chain[1:])):
        d.add(Arrow((a, "r", 0.25), (b, "l", 0.25), kind="accent", label="TX" if i == 0 else None,
            label_side="above"))
        d.add(Arrow((b, "l", 0.75), (a, "r", 0.75), kind="data", label="RX" if i == 0 else None,
            label_side="below"))
    d.add(Arrow((time, "t"), (head, "b"), kind="control", label="time (ns)", label_side="right"))
    return d
