#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.ad9361.agc import AGCSaturationCount


def test_agc_saturation_counter():
    i = Signal(12)
    q = Signal(12)
    dut = AGCSaturationCount(ce=1, iqs=[i, q], inc=1)
    counts = []

    def gen():
        yield dut.control.fields.enable.eq(1)
        yield dut.control.fields.threshold.eq(8)

        # Below threshold.
        for _ in range(4):
            yield i.eq(1)
            yield q.eq(2)
            counts.append((yield dut.status.fields.count))
            yield

        # Above threshold on I.
        for _ in range(5):
            yield i.eq(9)
            yield q.eq(0)
            counts.append((yield dut.status.fields.count))
            yield

        # Clear.
        yield i.eq(0)
        yield q.eq(0)
        yield
        yield dut.control.fields.clear.eq(1)
        counts.append((yield dut.status.fields.count))
        yield
        yield dut.control.fields.clear.eq(0)
        yield
        counts.append((yield dut.status.fields.count))
        yield

    run_simulation(dut, gen())
    assert max(counts[:4]) == 0
    assert counts[8] >= 3
    assert counts[-1] == 0
