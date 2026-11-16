#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Host software layers: applications, SoapySDR driver, libm2sdr, transport helpers, kernel driver.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 500)
    d.add(Container(20, 105, 720, 290, title="Software of the project", title_pos="bl"))
    apps  = d.add(Box(45, 20, 670, 50, label="Applications", fill=COLOR_HARDWARE))
    soapy = d.add(Box(45, 125, 200, 50, label="SoapySDR driver"))
    utils = d.add(Box(280, 125, 200, 50, label="Utilities"))
    lib   = d.add(Box(45, 215, 670, 50, label="libm2sdr", fill=COLOR_ACCENT))
    lpcie = d.add(Box(45, 305, 200, 50, label="liblitepcie", fill=COLOR_ACCENT_LIGHT))
    leth  = d.add(Box(515, 305, 200, 50, label="libliteeth", fill=COLOR_ACCENT_LIGHT))
    kern  = d.add(Box(280, 305, 200, 50, label="Kernel driver", fill=COLOR_ACCENT_LIGHT))
    board = d.add(Box(280, 430, 435, 50, label="Board", fill=COLOR_HARDWARE))
    d.add(Arrow((soapy.cx, apps.y + apps.h), (soapy, "t"), kind="data", heads="both"))
    d.add(Arrow((soapy, "b"), (soapy.cx, lib.y), kind="data", heads="both"))
    d.add(Arrow((utils, "b"), (utils.cx, lib.y), kind="data", heads="both"))
    d.add(Arrow((leth.cx, apps.y + apps.h), (leth.cx, lib.y), kind="data", heads="both"))
    d.add(Arrow((lpcie.cx, lib.y + lib.h), (lpcie, "t"), kind="data", heads="both"))
    d.add(Arrow((leth.cx, lib.y + lib.h), (leth, "t"), kind="data", heads="both"))
    d.add(Arrow((lpcie, "r"), (kern, "l"), kind="data", heads="both"))
    d.add(Arrow((kern, "b"), (kern.cx, board.y), kind="accent", heads="both", label="PCIe: MMAP, DMA", label_pos=(kern.cx + 62, 417)))
    d.add(Arrow((leth, "b"), (leth.cx, board.y), kind="accent", heads="both", label="Ethernet: UDP", label_pos=(leth.cx + 52, 417)))
    return d
