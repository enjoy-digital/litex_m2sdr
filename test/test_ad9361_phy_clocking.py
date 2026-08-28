#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""The TX serializer MMCM decides which sample rates the oversampling image can transmit at.

Its multiply/divide are fixed at build time, so the input frequencies it can lock at follow from
them and the VCO range -- and outside that range ~locked holds every OSERDESE2 in reset, i.e. the
board transmits zeros while the bring-up still reports success. libm2sdr refuses those rates
(m2sdr_image_rate_error() in libm2sdr/m2sdr_rf.c) using exactly the numbers asserted here, so this
test is what stops the two from drifting apart.
"""

from migen import Record

from litex_m2sdr.gateware.ad9361.phy import AD9361PHY

# Mirrors M2SDR_OSERDES_MMCM_* in litex_m2sdr/software/user/libm2sdr/m2sdr_rf.c.
DRIVER_MMCM_MULT     = 13 / 4
DRIVER_MMCM_DIV      = 1
DRIVER_VCO_RANGE_HZ  = (600e6, 1600e6)
# DATA_CLK bounds the driver enforces: VCO_MIN * DEN / NUM and VCO_MAX * DEN / NUM, rounded.
DRIVER_DATA_CLK_LOCK_RANGE_HZ = (184615385, 492307692)


def _rfic_pads():
    return Record([
        ("rx_clk_p",   1), ("rx_clk_n",   1),
        ("rx_frame_p", 1), ("rx_frame_n", 1),
        ("rx_data_p",  6), ("rx_data_n",  6),
        ("tx_clk_p",   1), ("tx_clk_n",   1),
        ("tx_frame_p", 1), ("tx_frame_n", 1),
        ("tx_data_p",  6), ("tx_data_n",  6),
    ])


def _oserdes_mmcm_config():
    phy = AD9361PHY(_rfic_pads(), with_loopback=False, with_rx_idelay=True, with_tx_oserdes=True)
    return phy.tx_oserdes_mmcm, phy.tx_oserdes_mmcm.compute_config()


def test_oserdes_mmcm_synthesizes_the_serializer_clock_pair():
    # CLK is the DDR bit clock and CLKDIV the 8:1 parallel-load clock, so CLK/CLKDIV must be 4.
    _, config = _oserdes_mmcm_config()
    assert config["clkout0_freq"] == 491.52e6
    assert config["clkout1_freq"] == 122.88e6
    assert config["clkout0_freq"] / config["clkout1_freq"] == 4


def test_oserdes_mmcm_ratio_matches_the_driver_constants():
    # The ratio is what makes the lock range a property of DATA_CLK alone: every output scales with
    # the input, so the serializer keeps its 8:1 ratio at any frequency the MMCM locks at.
    mmcm, config = _oserdes_mmcm_config()
    assert config["clkfbout_mult"] == DRIVER_MMCM_MULT, (
        f"TX serializer MMCM multiply changed to {config['clkfbout_mult']}; update "
        f"M2SDR_OSERDES_MMCM_MULT_NUM/DEN in libm2sdr/m2sdr_rf.c")
    assert config["divclk_divide"] == DRIVER_MMCM_DIV
    assert mmcm.vco_freq_range == DRIVER_VCO_RANGE_HZ, (
        f"MMCM VCO range changed to {mmcm.vco_freq_range}; update "
        f"M2SDR_OSERDES_MMCM_VCO_MIN_HZ/MAX_HZ in libm2sdr/m2sdr_rf.c")


def test_oserdes_mmcm_lock_range_covers_the_supported_data_clocks():
    # DATA_CLK is 2x the sample rate per channel: 245.76 MHz at 2T2R@61.44 and 1T1R@122.88,
    # 491.52 MHz at 2T2R@122.88. 122.88 MHz (2T2R@30.72) is below the VCO minimum.
    mmcm, config = _oserdes_mmcm_config()
    vco_min, vco_max = mmcm.vco_freq_range
    lock_min = vco_min * config["divclk_divide"] / config["clkfbout_mult"]
    lock_max = vco_max * config["divclk_divide"] / config["clkfbout_mult"]

    assert (round(lock_min), round(lock_max)) == DRIVER_DATA_CLK_LOCK_RANGE_HZ
    assert lock_min <= 245.76e6 <= lock_max
    assert lock_min <= 491.52e6 <= lock_max
    assert 122.88e6 < lock_min
