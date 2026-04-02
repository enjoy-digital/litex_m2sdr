#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen.sim import run_simulation

from liteeth.core.ptp import PTP_MSG_ANNOUNCE, PTP_MSG_SYNC

from litex_m2sdr.gateware.ptp_identity import PTPIdentityTracker


def test_ptp_identity_tracker_prefers_master_matched_general_packets():
    dut = PTPIdentityTracker(with_csr=False)
    seen = {}

    master_general = 0x112233445566778899aa
    master_event = 0xaabbccddeeff00112233

    def gen():
        yield dut.master_ip.eq(0xc0a80164)
        yield dut.local_port_id.eq(0x0102030405060708090a)

        yield dut.general_valid.eq(1)
        yield dut.general_msg_type.eq(PTP_MSG_ANNOUNCE)
        yield dut.general_ip.eq(0xc0a80164)
        yield dut.general_source_port_id.eq(master_general)
        yield
        yield dut.general_valid.eq(0)
        yield

        yield dut.event_valid.eq(1)
        yield dut.event_msg_type.eq(PTP_MSG_SYNC)
        yield dut.event_ip.eq(0xc0a80165)
        yield dut.event_source_port_id.eq(master_event)
        yield
        yield dut.event_valid.eq(0)
        yield

        seen["master_port_id"] = (yield dut.master_port_id)
        seen["update_count"] = (yield dut.update_count)

    run_simulation(dut, gen())
    assert seen["master_port_id"] == master_general
    assert seen["update_count"] == 1

def test_ptp_identity_tracker_clear_resets_master_identity():
    dut = PTPIdentityTracker(with_csr=False)
    seen = {}

    def gen():
        yield dut.general_valid.eq(1)
        yield dut.general_msg_type.eq(PTP_MSG_ANNOUNCE)
        yield dut.general_source_port_id.eq(0x112233445566778899aa)
        yield
        yield dut.general_valid.eq(0)
        yield

        yield dut.clear.eq(1)
        yield
        yield dut.clear.eq(0)
        yield

        seen["master_port_id"] = (yield dut.master_port_id)
        seen["update_count"] = (yield dut.update_count)

    run_simulation(dut, gen())
    assert seen["master_port_id"] == 0
    assert seen["update_count"] == 0

