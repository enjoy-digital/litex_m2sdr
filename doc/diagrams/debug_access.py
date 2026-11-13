#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Debug access: host tools, the library or litex_server, and the three host bridges of the SoC.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 360)
    d.add(Container(395, 15, 350, 330, title="FPGA", title_pos="br"))
    d.add(Container(408, 28, 144, 300, title="Host bridges", title_pos="bl", title_size=FONT_SIZE))
    util = d.add(Box( 15,  40, 140, 50, label="m2sdr_util"))
    tool = d.add(Box( 15, 130, 140, 190, label="litex_cli\nlitescope_cli\nscripts"))
    lib  = d.add(Box(205,  40, 115, 50, label="libm2sdr"))
    srv  = d.add(Box(205, 130, 115, 190, label="litex_server", fill=COLOR_ACCENT))
    pcie = d.add(Box(420,  40, 120, 50, label="PCIe MMAP", fill=COLOR_ACCENT_LIGHT))
    eb   = d.add(Box(420, 125, 120, 50, label="Etherbone", fill=COLOR_ACCENT_LIGHT))
    jtag = d.add(Box(420, 210, 120, 50, label="JTAGBone", fill=COLOR_ACCENT_LIGHT))
    regs = d.add(Box(600,  40, 120, 220, label="Registers", fill=COLOR_ACCENT))
    d.add(Arrow((util, "r"), (lib, "l"), kind="control"))
    d.add(Arrow((155, 225), (205, 225), kind="control"))
    d.add(Arrow((320, 55), (420, 55), kind="control"))
    d.add(Arrow((320, 75), (420, 150), kind="control", via=[(365, 75), (365, 150)]))
    d.add(Arrow((320, 285), (408, 285), kind="control"))
    d.add(Arrow((pcie, "r"), (600, 65), kind="control", heads="both"))
    d.add(Arrow((eb, "r"), (600, 150), kind="control", heads="both"))
    d.add(Arrow((jtag, "r"), (600, 235), kind="control", heads="both"))
    return d
