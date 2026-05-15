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


def _qpll_linerate(refclk_freq, settings, out_div):
    qpll_vco_freq = 2*refclk_freq*settings.fbdiv*settings.fbdiv_45/settings.refclk_div
    return qpll_vco_freq/out_div


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
    assert dut.qpll.kwargs["qpllsettings1"] == qpll_mod.QPLLSettings(
        refclksel  = 0b111,
        fbdiv      = 5,
        fbdiv_45   = 4,
        refclk_div = 1,
    )


def test_shared_qpll_eth_sata_configuration_assigns_distinct_channels(monkeypatch):
    """Verify Ethernet+SATA fits the two-channel QPLL resource budget."""
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()

    dut = SharedQPLL(platform, with_eth=True, with_sata=True)

    assert dut.channel_map == {"eth": 0, "sata": 1}
    assert dut.get_channel("eth") is dut.qpll.channels[0]
    assert dut.get_channel("sata") is dut.qpll.channels[1]
    assert dut.qpll.kwargs["qpllsettings1"] == qpll_mod.QPLLSettings(
        refclksel  = 0b111,
        fbdiv      = 5,
        fbdiv_45   = 4,
        refclk_div = 1,
    )


@pytest.mark.parametrize("eth_phy, refclk_freq, out_div, expected_linerate", [
    ("1000basex", 125e6,    4, 1.25e9),
    ("1000basex", 156.25e6, 4, 1.25e9),
    ("2500basex", 125e6,    2, 3.125e9),
    ("2500basex", 156.25e6, 2, 3.125e9),
])
def test_shared_qpll_eth_settings_generate_expected_linerate(monkeypatch, eth_phy, refclk_freq, out_div, expected_linerate):
    monkeypatch.setattr(qpll_mod, "QPLL", _FakeQPLL)
    platform = _DummyPlatform()

    dut = SharedQPLL(platform, with_eth=True, eth_phy=eth_phy, eth_refclk_freq=refclk_freq)
    settings = dut.qpll.kwargs["qpllsettings0"]

    assert _qpll_linerate(refclk_freq, settings, out_div) == pytest.approx(expected_linerate)


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
