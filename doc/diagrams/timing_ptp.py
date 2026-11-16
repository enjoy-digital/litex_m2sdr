#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Ethernet PTP: from the LiteEth PTP time to the board time, the PPS and the 10 MHz clock.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    ptp  = d.add(Box(20,  60, 130, 60, label="LiteEthPTP\nPTP time", fill=COLOR_ACCENT_LIGHT))
    srv  = d.add(Box(210, 60, 150, 60, label="PTPTimeDiscipline\nservo", fill=COLOR_ACCENT))
    cdc  = d.add(Box(420, 60, 120, 60, label="Discipline\nCDC"))
    gen  = d.add(Box(600, 60, 140, 60, label="TimeGenerator\nboard time"))
    pps  = d.add(Box(600, 230, 140, 60, label="PPSGenerator\ninternal PPS"))
    c10  = d.add(Box(400, 230, 150, 60, label="PTPClock10\nDiscipline", fill=COLOR_ACCENT))
    dps  = d.add(Box(210, 230, 140, 60, label="MMCMPhase\nDiscipline", fill=COLOR_ACCENT))
    mmcm = d.add(Box(20, 230, 130, 60, label="clk10 MMCM\n10 MHz", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((ptp, "r"), (srv, "l"), kind="data"))
    d.add(Arrow((srv, "r"), (cdc, "l"), kind="accent"))
    d.add(Arrow((cdc, "r"), (gen, "l"), kind="accent"))
    d.add(Arrow((gen, "t"), (srv, "t"), kind="feedback", via=[(670, 30), (285, 30)], label="board time", label_pos=(478, 21)))
    d.add(Arrow((gen, "b"), (pps, "t"), kind="data"))
    d.add(Arrow((pps, "l"), (c10, "r"), kind="data"))
    d.add(Arrow((c10, "l"), (dps, "r"), kind="control", label="rate"))
    d.add(Arrow((dps, "l"), (mmcm, "r"), kind="control", label="steps"))
    d.add(Arrow((mmcm, "b"), (c10, "b"), kind="feedback", via=[(85, 312), (475, 312)], label="10 MHz marker", label_pos=(280, 303)))
    return d
