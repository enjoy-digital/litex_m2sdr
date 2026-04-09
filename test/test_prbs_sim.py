#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.sim import run_simulation

from litex.gen import LiteXModule
from litex.soc.interconnect import stream

from litex_m2sdr.gateware.ad9361.core import AD9361RFIC
from litex_m2sdr.gateware.ad9361.phy import phy_layout


class FakePHY(LiteXModule):
    def __init__(self):
        self.sink = stream.Endpoint(phy_layout())
        self.source = stream.Endpoint(phy_layout())
        self.loopback_enable = Signal(reset=1)
        self.inject_error = Signal()
        self.drop_valid = Signal()

        self.comb += self.sink.ready.eq(1)
        self.sync.rfic += [
            If(self.drop_valid | ~self.loopback_enable,
                self.source.valid.eq(0)
            ).Else(
                self.source.valid.eq(self.sink.valid & self.sink.ready),
                self.source.ia.eq(self.sink.ia ^ Replicate(self.inject_error, len(self.source.ia))),
                self.source.qa.eq(self.sink.qa),
                self.source.ib.eq(self.sink.ib),
                self.source.qb.eq(self.sink.qb),
            )
        ]


class PRBSSimHarness(LiteXModule):
    def __init__(self):
        self.cd_sys = ClockDomain("sys")
        self.cd_rfic = ClockDomain("rfic")
        self.phy = FakePHY()
        self.submodules.phy = self.phy
        AD9361RFIC.add_prbs(self)


def _set_prbs_tx(dut, enable=False, rx_reset=False):
    value = (1 if enable else 0) | ((1 if rx_reset else 0) << 1)
    yield from dut.prbs_tx.write(value)


def _sample_prbs_status(dut):
    return {
        "synced": (yield dut.prbs_rx.fields.synced),
        "error": (yield dut.prbs_rx.fields.error),
        "valid_count": (yield dut.prbs_rx.fields.valid_count),
        "error_count": (yield dut.prbs_rx.fields.error_count),
    }


def _wait_cycles(count):
    for _ in range(count):
        yield


def test_prbs_fake_phy_loopback_reaches_sync():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield dut.phy.loopback_enable.eq(1)
        yield dut.phy.inject_error.eq(0)
        yield dut.phy.drop_valid.eq(0)
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(1400)
        observed.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert observed["synced"] == 1
    assert observed["error"] == 0
    assert observed["valid_count"] >= 1024
    assert observed["error_count"] == 0


def test_prbs_fake_phy_error_injection_sets_sticky_error():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield dut.phy.loopback_enable.eq(1)
        yield dut.phy.inject_error.eq(1)
        yield dut.phy.drop_valid.eq(0)
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(128)
        observed.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert observed["synced"] == 0
    assert observed["error"] == 1
    assert observed["valid_count"] > 0
    assert observed["error_count"] > 0


def test_prbs_fake_phy_rx_reset_clears_observation_only():
    dut = PRBSSimHarness()
    before_reset = {}
    after_reset = {}
    after_resettle = {}

    def stimulus():
        yield dut.phy.loopback_enable.eq(1)
        yield dut.phy.inject_error.eq(0)
        yield dut.phy.drop_valid.eq(0)
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(1400)
        before_reset.update((yield from _sample_prbs_status(dut)))

        yield from _set_prbs_tx(dut, enable=True, rx_reset=True)
        yield
        after_reset.update((yield from _sample_prbs_status(dut)))

        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(64)
        after_resettle.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert before_reset["synced"] == 1
    assert before_reset["error"] == 0
    assert before_reset["error_count"] == 0
    assert after_reset["error"] == 0
    assert after_reset["valid_count"] == 0
    assert after_reset["error_count"] == 0
    assert after_resettle["synced"] == 1
    assert after_resettle["error"] == 0
    assert after_resettle["error_count"] == 0
