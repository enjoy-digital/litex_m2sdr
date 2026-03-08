#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import pytest

import litex_m2sdr.gateware.qpll as qpll_mod
from litex_m2sdr.gateware.qpll import SharedQPLL


class _DummyPlatform:
    def __init__(self):
        self.commands = []

    def add_platform_command(self, cmd):
        self.commands.append(cmd)


class _FakeChannel:
    pass


class _FakeQPLL:
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.channels = [_FakeChannel(), _FakeChannel()]


def test_shared_qpll_single_configuration_maps_named_channel(monkeypatch):
    """Verify a single selected function gets channel index 0 and emits the DRC command."""
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()

    dut = SharedQPLL(platform, with_pcie=True, with_eth=False, with_sata=False)

    assert dut.channel_map == {"pcie": 0}
    assert dut.get_channel("pcie") is dut.qpll.channels[0]
    assert platform.commands


def test_shared_qpll_dual_configuration_assigns_distinct_channels(monkeypatch):
    """Verify two enabled functions get distinct channel assignments."""
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()

    dut = SharedQPLL(platform, with_pcie=True, with_eth=True, eth_phy="2500basex")

    assert dut.channel_map == {"pcie": 0, "eth": 1}
    assert dut.get_channel("pcie") is dut.qpll.channels[0]
    assert dut.get_channel("eth") is dut.qpll.channels[1]


def test_shared_qpll_rejects_invalid_channel_name(monkeypatch):
    """Verify get_channel rejects unknown names with a clear error."""
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()
    dut = SharedQPLL(platform, with_eth=True)

    with pytest.raises(ValueError, match="Invalid QPLL name"):
        dut.get_channel("bogus")


def test_shared_qpll_rejects_unsupported_triple_use(monkeypatch):
    """Verify the constructor enforces the two-channel QPLL resource limit."""
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()

    with pytest.raises(AssertionError):
        SharedQPLL(platform, with_pcie=True, with_eth=True, with_sata=True)
