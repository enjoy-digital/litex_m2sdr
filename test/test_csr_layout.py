#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""The CSR map must mean the same thing in every supported build configuration.

Host software is compiled against litex_m2sdr/software/kernel/csr.h once and then talks to
whichever image happens to be flashed. That only works if every configuration agrees on where
each CSR lives, so these tests elaborate the whole configuration matrix (no FPGA toolchain
needed) and fail on any disagreement, and on a tracked header that no longer matches.
"""

import filecmp
import importlib.util
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


gen_kernel_headers = _load("gen_kernel_headers", ROOT / "scripts" / "gen_kernel_headers.py")


@pytest.fixture(scope="module")
def elaborated():
    """Every supported configuration, elaborated once for the whole module."""
    soc_module = gen_kernel_headers.load_soc_module()
    with tempfile.TemporaryDirectory() as tmp:
        configs, skipped = gen_kernel_headers.elaborate_supported(soc_module, tmp)
        assert configs, "no build configuration could be elaborated"
        yield soc_module, configs, skipped, Path(tmp)


def test_every_csr_region_has_a_fixed_location(elaborated):
    # An auto-allocated region takes the lowest free location, which moves with the feature set.
    _, configs, _, _ = elaborated
    for name, (soc, _) in configs.items():
        soc.check_csr_map()


def test_no_csr_lands_in_the_catch_all_main_bank(elaborated):
    # CSRs declared directly on the SoC share LiteX's "main" bank, where unrelated registers of
    # different builds end up at the same address.
    _, configs, _, _ = elaborated
    for name, (_, csr_map) in configs.items():
        assert "main" not in csr_map["bases"], (
            f"{name} has a \"main\" CSR bank: move those CSRs into a module with its own "
            f"csr_map location.")


def test_configurations_agree_on_the_csr_map(elaborated):
    _, configs, _, _ = elaborated
    errors = gen_kernel_headers.check_agreement(configs)
    assert errors == [], "\n".join(errors)


def test_tracked_headers_are_up_to_date(elaborated):
    soc_module, configs, skipped, tmp = elaborated
    staging = tmp / "headers"
    union   = gen_kernel_headers.build_union(configs)
    gen_kernel_headers.write_headers(union, staging)
    if skipped:
        gen_kernel_headers.carry_over_missing_regions(staging / "csr.h",
            gen_kernel_headers.KERNEL_DIR / "csr.h", soc_module.BaseSoC.csr_map,
            union.csr_regions)

    stale = [h for h in gen_kernel_headers.HEADERS
             if not filecmp.cmp(staging / h, gen_kernel_headers.KERNEL_DIR / h, shallow=False)]
    assert stale == [], (
        f"{', '.join(stale)} no longer match the build configurations; "
        f"run ./scripts/gen_kernel_headers.py")


def test_released_configurations_are_covered():
    # A released image whose CSR map was never compared to the others is exactly the mismatch
    # this matrix exists to prevent.
    from litex_m2sdr.build_configs import SUPPORTED_CONFIGS, config_kwargs

    release = _load("release", ROOT / "release.py")
    covered = [config_kwargs(config) for config in SUPPORTED_CONFIGS.values()]
    for configuration in release.RELEASE_CONFIGURATIONS:
        wanted = {
            "variant"    : configuration["variant"],
            "with_pcie"  : configuration["with_pcie"],
            "with_eth"   : configuration["with_eth"],
        }
        if configuration["with_pcie"]:
            wanted["pcie_lanes"] = configuration["pcie_lanes"]
        if configuration["with_eth_ptp"]:
            wanted["with_eth_ptp"] = True
        if configuration["with_eth_ptp_rfic_clock"]:
            wanted["with_eth_ptp_rfic_clock"] = True
        assert any(all(kwargs.get(k, False) == v for k, v in wanted.items()) for kwargs in covered), (
            f"release configuration {configuration['build_name']} is not in "
            f"litex_m2sdr/build_configs.py, so its CSR map is never checked")


# The checks above only mean something if they fail when they should.

def _csr_map(bases=None, regs=None, constants=None, mems=None):
    return {
        "bases"     : bases     or {},
        "regs"      : regs      or {},
        "constants" : constants or {},
        "mems"      : mems      or {},
    }


def test_check_agreement_reports_a_moved_register():
    errors = gen_kernel_headers.check_agreement({
        "a": (None, _csr_map(bases={"rfic": 0x1000}, regs={"rfic_gain": (0x1000, 1, "rw")})),
        "b": (None, _csr_map(bases={"rfic": 0x1000}, regs={"rfic_gain": (0x1004, 1, "rw")})),
    })
    assert any("rfic_gain" in error for error in errors)


def test_check_agreement_reports_an_optional_csr_declared_mid_region():
    # "b" inserts a register before rfic_gain instead of appending after it.
    errors = gen_kernel_headers.check_agreement({
        "a": (None, _csr_map(bases={"rfic": 0x1000},
            regs={"rfic_gain": (0x1000, 1, "rw")})),
        "b": (None, _csr_map(bases={"rfic": 0x1000},
            regs={"rfic_deskew": (0x1000, 1, "rw"), "rfic_gain": (0x1004, 1, "rw")})),
    })
    assert any("not a prefix" in error for error in errors)


def test_check_csr_map_rejects_an_unpinned_region(elaborated):
    _, configs, _, _ = elaborated
    soc = next(iter(configs.values()))[0]
    soc.csr_regions["not_in_csr_map"] = soc.csr_regions[next(iter(soc.csr_regions))]
    try:
        with pytest.raises(ValueError, match="not_in_csr_map"):
            soc.check_csr_map()
    finally:
        del soc.csr_regions["not_in_csr_map"]
