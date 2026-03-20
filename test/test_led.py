#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from litex.gen.sim import run_simulation

from litex_m2sdr.gateware.led import LedActivityBlink, LedDoublePulse, LedEventHold, StatusLed

# Status LED Tests -------------------------------------------------------------------------------


def test_led_double_pulse_repeats_expected_windows():
    """Check double-pulse pattern repeats the configured active windows."""
    dut = LedDoublePulse(period=16, pulse0=(0, 2), pulse1=(5, 7))
    active = []

    def gen():
        for _ in range(16):
            active.append((yield dut.active))
            yield dut.tick.eq(1)
            yield
            yield dut.tick.eq(0)
            yield

    run_simulation(dut, gen())

    assert active == [
        1, 1, 0, 0,
        0, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    ]


def test_led_event_hold_keeps_trigger_visible_until_timeout():
    """Check event hold converts a short trigger into a fixed-duration active window."""
    dut = LedEventHold(hold_ticks=3)
    active = []

    def gen():
        yield dut.trigger.eq(1)
        yield
        yield dut.trigger.eq(0)
        for _ in range(6):
            yield dut.tick.eq(1)
            yield
            active.append((yield dut.active))
            yield dut.tick.eq(0)
            yield

    run_simulation(dut, gen())

    assert active == [0, 1, 1, 1, 0, 0]


def test_led_activity_blink_flashes_but_does_not_stay_stuck_on():
    """Check sustained activity is converted to periodic flashes instead of a constant on state."""
    dut = LedActivityBlink(period_ticks=4, width_ticks=1)
    active = []

    def gen():
        yield dut.trigger.eq(1)
        for _ in range(10):
            yield dut.tick.eq(1)
            yield
            active.append((yield dut.active))
            yield dut.tick.eq(0)
            yield

    run_simulation(dut, gen())

    assert any(active)
    assert any(not v for v in active)
    assert sum(active) <= 3


def test_status_led_ready_mode_breathes_and_pps_overlays():
    """Check ready mode breathes and PPS overlay raises brightness above idle."""
    dut = StatusLed(sys_clk_freq=1_000)
    samples = {"idle": [], "pps": []}

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)

        for _ in range(320):
            samples["idle"].append((yield dut.level))
            yield

        yield dut.pps_pulse.eq(1)
        yield
        yield dut.pps_pulse.eq(0)

        for _ in range(40):
            samples["pps"].append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert max(samples["idle"]) > min(samples["idle"])
    assert max(samples["pps"]) >= 224


def test_status_led_pcie_discovery_uses_double_heartbeat():
    """Check PCIe-not-linked state produces two pulses followed by a dark gap."""
    dut = StatusLed(sys_clk_freq=1_000)
    steps = []

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)
        yield dut.pcie_present.eq(1)
        yield

        for cycle in range(500):
            if cycle % 10 == 0:
                steps.append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert all(level == 96 for level in steps[0:6])
    assert all(level == 96 for level in steps[12:18])
    assert all(level == 0 for level in steps[24:40])


def test_status_led_pcie_unsynced_keeps_transport_wait_pattern():
    """Check PCIe link-up without DMA sync still looks like transport-not-ready."""
    dut = StatusLed(sys_clk_freq=1_000)
    steps = []

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)
        yield dut.pcie_present.eq(1)
        yield dut.pcie_link_up.eq(1)
        for _ in range(4):
            yield

        for cycle in range(500):
            if cycle % 10 == 0:
                steps.append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert all(level == 96 for level in steps[0:6])
    assert all(level == 96 for level in steps[12:18])
    assert all(level == 0 for level in steps[24:40])


def test_status_led_eth_link_skips_transport_discovery_pattern():
    """Check an active Ethernet link suppresses the transport-discovery heartbeat."""
    dut = StatusLed(sys_clk_freq=1_000)
    samples = []

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)
        yield dut.eth_present.eq(1)
        yield dut.eth_link_up.eq(1)
        for _ in range(4):
            yield

        for _ in range(160):
            samples.append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert max(samples) < 96
    assert max(samples) > min(samples)


def test_status_led_activity_overlay_is_visible():
    """Check sustained TX activity creates repeated bright accents rather than a constant bright state."""
    dut = StatusLed(sys_clk_freq=1_000)
    samples = {"before": [], "after": []}

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)

        for _ in range(40):
            samples["before"].append((yield dut.level))
            yield

        yield dut.tx_activity.eq(1)
        for _ in range(160):
            samples["after"].append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert max(samples["after"]) >= 208
    assert max(samples["after"]) > max(samples["before"])
    assert min(samples["after"]) < max(samples["after"])


def test_status_led_eth_activity_contributes_to_visible_accents():
    """Check Ethernet traffic also contributes to the visible activity overlay."""
    dut = StatusLed(sys_clk_freq=1_000)
    samples = []

    def gen():
        yield dut.time_running.eq(1)
        yield dut.time_valid.eq(1)
        yield dut.eth_present.eq(1)
        yield dut.eth_link_up.eq(1)
        yield dut.eth_rx_activity.eq(1)
        for _ in range(4):
            yield

        for _ in range(160):
            samples.append((yield dut.level))
            yield

    run_simulation(dut, gen())

    assert max(samples) >= 208
    assert min(samples) < max(samples)
