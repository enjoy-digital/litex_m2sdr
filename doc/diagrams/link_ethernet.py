#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Ethernet host link: PHY, UDP/IP core and the UDP ports of the gateware.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 400)
    sfp  = d.add(Box(20, 165, 100, 70, label="SFP\nmodule", fill=COLOR_HARDWARE))
    phy  = d.add(Box(160, 165, 120, 70, label="Ethernet\nPHY", fill=COLOR_ACCENT_LIGHT))
    core = d.add(Box(320, 40, 150, 320, label="LiteEthUDPIPCore\nUDP crossbar", fill=COLOR_ACCENT))
    eb   = d.add(Box(560,  40, 180, 60, label="Etherbone\nport 1234"))
    st   = d.add(Box(560, 125, 180, 60, label="Sample streamers\nport 2345"))
    ptp  = d.add(Box(560, 210, 180, 60, label="PTP\nports 319 and 320"))
    vrt  = d.add(Box(560, 295, 180, 60, label="VRT streamer\nport 4991"))
    d.add(Arrow((sfp, "r"), (phy, "l"), kind="data", heads="both"))
    d.add(Arrow((phy, "r"), (320, 200), kind="data", heads="both"))
    d.add(Arrow((470,  70), (eb,  "l"), kind="data", heads="both", label="registers"))
    d.add(Arrow((470, 155), (st,  "l"), kind="accent", heads="both", label="samples"))
    d.add(Arrow((470, 240), (ptp, "l"), kind="data", heads="both", label="time"))
    d.add(Arrow((vrt, "l"), (470, 325), kind="data", label="RX samples"))
    return d
