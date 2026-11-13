#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Board block diagram: M.2 connector, FPGA, RFIC, clock generator, oscillator, flash, RF connectors.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 350)
    d.add(Container(130, 20, 610, 310, title="LiteX-M2SDR", title_pos="br"))
    host  = d.add(Box( 20, 110,  80,  80, label="Carrier", fill=COLOR_HARDWARE))
    m2    = d.add(Box(150, 110, 100,  80, label="M.2 Key M\nconnector"))
    flash = d.add(Box(310,  40, 130,  40, label="SPI flash"))
    fpga  = d.add(Box(310, 110, 130,  80, label="FPGA\nXC7A200T", fill=COLOR_ACCENT))
    xo    = d.add(Box(310, 230, 130,  40, label="100 MHz XO"))
    rfic  = d.add(Box(500, 110, 110,  80, label="RFIC\nAD9361", fill=COLOR_ACCENT_LIGHT))
    si    = d.add(Box(500, 230, 110,  40, label="SI5351"))
    rf    = d.add(Box(660, 110,  60,  80, label="RF\nports", fill=COLOR_HARDWARE))
    d.add(Arrow((host, "r"), (m2, "l"), kind="accent", heads="both"))
    d.add(Arrow((m2, "r"), (fpga, "l"), kind="accent", heads="both", label="4 lanes"))
    d.add(Arrow((flash, "b"), (fpga, "t"), kind="data", heads="both"))
    d.add(Arrow((350, 230), (350, 190), kind="control"))
    d.add(Arrow((fpga, "r"), (rfic, "l"), kind="data", heads="both", label="LVDS"))
    d.add(Arrow((si, "t"), (rfic, "b"), kind="control", label="reference", label_side="right"))
    d.add(Arrow((522, 230), (415, 190), via=[(522, 210), (415, 210)], kind="control", label="clocks",
        label_pos=(468, 224)))
    d.add(Arrow((rfic, "r"), (rf, "l"), kind="data", heads="both"))
    return d
