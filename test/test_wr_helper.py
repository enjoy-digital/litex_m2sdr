#!/usr/bin/env python3

import pytest

from litex_m2sdr.wr_helper import (
    ERR_WR_PATCH_MISSING_CHECK,
    prepare_wr_environment,
    preflight_wr_cores,
    validate_wr_platform,
)

BASEBOARD_IO = [
    ("sfp", 0, object()),
    ("sfp", 1, object()),
    ("sata", 0, object()),
]


def test_validate_wr_platform_autoselect_sfp():
    result = validate_wr_platform(variant="baseboard", wr_sfp=None, baseboard_io=BASEBOARD_IO)
    assert result["wr_sfp"] == 0
    assert result["available_sfps"] == [0, 1]
    assert result["auto_selected"] is True


def test_validate_wr_platform_rejects_non_baseboard():
    with pytest.raises(ValueError, match="only supported with --variant=baseboard"):
        validate_wr_platform(variant="m2", wr_sfp=0, baseboard_io=BASEBOARD_IO)


def test_preflight_check_mode_detects_missing_patch(tmp_path):
    wr_subsystem_vhd = tmp_path / "wr-cores" / "modules" / "wrc_core" / "xwr_subsystem.vhd"
    wr_subsystem_vhd.parent.mkdir(parents=True)
    wr_subsystem_vhd.write_text('mux_class_i(1) => x"f0");\n')

    with pytest.raises(ValueError, match=ERR_WR_PATCH_MISSING_CHECK):
        preflight_wr_cores(str(tmp_path), wr_nic_dir=None, patch_mode="check")


def test_prepare_wr_environment_status_only_prints(capsys, tmp_path):
    wr_env = prepare_wr_environment(
        root_dir=str(tmp_path),
        variant="baseboard",
        baseboard_io=BASEBOARD_IO,
        with_white_rabbit=False,
        wr_sfp=None,
        wr_nic_dir=None,
        wr_firmware=None,
        wr_firmware_target="acorn",
        build=False,
        patch_mode="auto",
        status=True,
    )

    captured = capsys.readouterr()
    assert "White Rabbit status:" in captured.out
    assert wr_env["wr_sfp"] is None
