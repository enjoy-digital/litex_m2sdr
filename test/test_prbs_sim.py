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
        self.drop_valid = Signal()
        self.inject_error = Signal()
        self.swap_channels = Signal()
        self.swap_iq = Signal()
        self.shift_right = Signal()
        self.slip_one = Signal()

        # # #

        prev_valid = Signal()
        prev_ia = Signal.like(self.source.ia)
        prev_qa = Signal.like(self.source.qa)
        prev_ib = Signal.like(self.source.ib)
        prev_qb = Signal.like(self.source.qb)

        src_valid = Signal()
        src_ia = Signal.like(self.source.ia)
        src_qa = Signal.like(self.source.qa)
        src_ib = Signal.like(self.source.ib)
        src_qb = Signal.like(self.source.qb)

        ch_ia = Signal.like(self.source.ia)
        ch_qa = Signal.like(self.source.qa)
        ch_ib = Signal.like(self.source.ib)
        ch_qb = Signal.like(self.source.qb)

        iq_ia = Signal.like(self.source.ia)
        iq_qa = Signal.like(self.source.qa)
        iq_ib = Signal.like(self.source.ib)
        iq_qb = Signal.like(self.source.qb)

        sh_ia = Signal.like(self.source.ia)
        sh_qa = Signal.like(self.source.qa)
        sh_ib = Signal.like(self.source.ib)
        sh_qb = Signal.like(self.source.qb)

        self.comb += self.sink.ready.eq(1)

        self.sync.rfic += [
            prev_valid.eq(self.sink.valid & self.sink.ready),
            prev_ia.eq(self.sink.ia),
            prev_qa.eq(self.sink.qa),
            prev_ib.eq(self.sink.ib),
            prev_qb.eq(self.sink.qb),
        ]

        self.comb += [
            src_valid.eq(Mux(self.slip_one, prev_valid, self.sink.valid & self.sink.ready)),
            src_ia.eq(Mux(self.slip_one, prev_ia, self.sink.ia)),
            src_qa.eq(Mux(self.slip_one, prev_qa, self.sink.qa)),
            src_ib.eq(Mux(self.slip_one, prev_ib, self.sink.ib)),
            src_qb.eq(Mux(self.slip_one, prev_qb, self.sink.qb)),
        ]

        self.comb += [
            ch_ia.eq(Mux(self.swap_channels, src_ib, src_ia)),
            ch_qa.eq(Mux(self.swap_channels, src_qb, src_qa)),
            ch_ib.eq(Mux(self.swap_channels, src_ia, src_ib)),
            ch_qb.eq(Mux(self.swap_channels, src_qa, src_qb)),
        ]

        self.comb += [
            iq_ia.eq(Mux(self.swap_iq, ch_qa, ch_ia)),
            iq_qa.eq(Mux(self.swap_iq, ch_ia, ch_qa)),
            iq_ib.eq(Mux(self.swap_iq, ch_qb, ch_ib)),
            iq_qb.eq(Mux(self.swap_iq, ch_ib, ch_qb)),
        ]

        self.comb += [
            sh_ia.eq(Mux(self.shift_right, Cat(C(0, 1), iq_ia[:-1]), iq_ia)),
            sh_qa.eq(Mux(self.shift_right, Cat(C(0, 1), iq_qa[:-1]), iq_qa)),
            sh_ib.eq(Mux(self.shift_right, Cat(C(0, 1), iq_ib[:-1]), iq_ib)),
            sh_qb.eq(Mux(self.shift_right, Cat(C(0, 1), iq_qb[:-1]), iq_qb)),
        ]

        self.sync.rfic += [
            If(self.drop_valid | ~self.loopback_enable,
                self.source.valid.eq(0)
            ).Else(
                self.source.valid.eq(src_valid),
                self.source.ia.eq(sh_ia ^ Replicate(self.inject_error, len(self.source.ia))),
                self.source.qa.eq(sh_qa),
                self.source.ib.eq(sh_ib),
                self.source.qb.eq(sh_qb),
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


def _configure_phy(dut, **kwargs):
    for name, value in kwargs.items():
        yield getattr(dut.phy, name).eq(int(bool(value)))


def test_prbs_fake_phy_matches_ad9361_12bit_layout():
    dut = PRBSSimHarness()

    assert len(dut.phy.sink.ia) == 12
    assert len(dut.phy.sink.qa) == 12
    assert len(dut.phy.sink.ib) == 12
    assert len(dut.phy.sink.qb) == 12
    assert len(dut.phy.source.ia) == 12
    assert len(dut.phy.source.qa) == 12
    assert len(dut.phy.source.ib) == 12
    assert len(dut.phy.source.qb) == 12


def test_prbs_fake_phy_loopback_reaches_sync():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=False,
            swap_channels=False,
            swap_iq=False,
            shift_right=False,
            slip_one=False,
        )
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
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=True,
            swap_channels=False,
            swap_iq=False,
            shift_right=False,
            slip_one=False,
        )
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
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=False,
            swap_channels=False,
            swap_iq=False,
            shift_right=False,
            slip_one=False,
        )
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


def test_prbs_fake_phy_one_sample_slip_reacquires_sync():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=False,
            swap_channels=False,
            swap_iq=False,
            shift_right=False,
            slip_one=True,
        )
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(1400)
        observed.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert observed["synced"] == 1
    assert observed["valid_count"] >= 1024


def test_prbs_fake_phy_channel_swap_is_invisible_to_checker():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=False,
            swap_channels=True,
            swap_iq=False,
            shift_right=False,
            slip_one=False,
        )
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(1400)
        observed.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert observed["synced"] == 1
    assert observed["valid_count"] >= 1024


def test_prbs_fake_phy_bit_shift_never_syncs():
    dut = PRBSSimHarness()
    observed = {}

    def stimulus():
        yield from _configure_phy(
            dut,
            loopback_enable=True,
            drop_valid=False,
            inject_error=False,
            swap_channels=False,
            swap_iq=False,
            shift_right=True,
            slip_one=False,
        )
        yield from _set_prbs_tx(dut, enable=True, rx_reset=False)
        yield from _wait_cycles(1400)
        observed.update((yield from _sample_prbs_status(dut)))

    run_simulation(dut, stimulus(), clocks={"sys": 10, "rfic": 10})

    assert observed["synced"] == 0
    assert observed["error"] == 1
    assert observed["valid_count"] > 0
    assert observed["error_count"] > 0
