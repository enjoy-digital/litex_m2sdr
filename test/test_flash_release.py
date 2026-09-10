#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import io
import json
import hashlib
import zipfile
import importlib.util

from pathlib import Path

import pytest

# Helpers ------------------------------------------------------------------------------------------
@pytest.fixture
def flasher(monkeypatch):
    path   = Path(__file__).resolve().parents[1] / "scripts" / "flash_release.py"
    spec   = importlib.util.spec_from_file_location("flash_release", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.setattr(module.time, "sleep", lambda seconds: None)
    return module


def make_archive(tmp_path, manifest_changes=None, config_changes=None, missing=None, extra=None):
    build = "litex_m2sdr_m2_pcie_x1"
    manifest = {
        "project"       : "litex_m2sdr",
        "release_date"  : "2026_05_15",
        "build_name"    : build,
        "git_revision"  : "a" * 40,
        "git_dirty"     : False,
        "configuration" : {
            "variant"    : "m2",
            "with_pcie"  : True,
            "pcie_lanes" : 1,
            "with_eth"   : False,
        },
    }
    manifest.update(manifest_changes or {})
    manifest["configuration"].update(config_changes or {})
    path = tmp_path / f"{build}_2026_05_15.zip"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("release_manifest.json", json.dumps(manifest))
        for slot in ("fallback", "operational"):
            if slot != missing:
                archive.writestr(f"{build}_{slot}.bin", (slot + " image").encode())
        if extra:
            archive.writestr(*extra)
    return path


# Release Downloads / Validation -------------------------------------------------------------------

@pytest.mark.parametrize("tag,endpoint", [("latest", "latest"), ("2026_05_15", "tags/2026_05_15")])
def test_resolves_latest_and_pinned_release(flasher, monkeypatch, tag, endpoint):
    requests = []

    def request(url):
        requests.append(url)
        return io.BytesIO(json.dumps({"tag_name": "2026_05_15"}).encode())

    monkeypatch.setattr(flasher, "request", request)
    assert flasher.fetch_release(tag)["tag_name"] == "2026_05_15"
    assert requests == [f"https://api.github.com/repos/enjoy-digital/litex_m2sdr/releases/{endpoint}"]


def test_download_rechecks_cached_bytes_and_repairs_corruption(flasher, tmp_path, monkeypatch):
    contents = b"release archive bytes"
    asset = {
        "name": "release.zip", "size": len(contents),
        "digest": "sha256:" + hashlib.sha256(contents).hexdigest(),
        "browser_download_url": "https://github.com/enjoy-digital/litex_m2sdr/releases/download/2026_05_15/release.zip",
    }
    requests = []

    def request(url):
        requests.append(url)
        return io.BytesIO(contents)

    monkeypatch.setattr(flasher, "request", request)
    path = flasher.download_asset(asset, tmp_path)
    assert path.read_bytes() == contents
    flasher.download_asset(asset, tmp_path)
    assert len(requests) == 1
    path.write_bytes(b"X" * len(contents))
    flasher.download_asset(asset, tmp_path)
    assert len(requests) == 2
    assert path.read_bytes() == contents


@pytest.mark.parametrize("digest", [None, "sha256:" + "0" * 64])
def test_download_rejects_missing_or_wrong_digest(flasher, tmp_path, monkeypatch, digest):
    asset = {
        "name": "release.zip", "size": 4, "digest": digest,
        "browser_download_url": "https://github.com/enjoy-digital/litex_m2sdr/releases/download/2026_05_15/release.zip",
    }
    monkeypatch.setattr(flasher, "request", lambda url: io.BytesIO(b"data"))
    with pytest.raises(flasher.FlashError, match="SHA-256"):
        flasher.download_asset(asset, tmp_path)
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize("changes", [
    {"manifest_changes": {"release_date": "2026_05_14"}},
    {"manifest_changes": {"git_dirty": True}},
    {"manifest_changes": {"build_name": "litex_m2sdr_m2_pcie_x2"}},
    {"config_changes": {"pcie_lanes": 2}},
    {"config_changes": {"variant": "baseboard"}},
    {"missing": "operational"},
])
def test_rejects_wrong_or_incomplete_release_before_extracting(flasher, tmp_path, changes):
    path = make_archive(tmp_path, **changes)
    output = tmp_path / "output"
    output.mkdir()
    with pytest.raises(flasher.FlashError):
        flasher.unpack_images(path, output, "2026_05_15", "m2_pcie_x1")
    assert not list(output.iterdir())


def test_extracts_only_expected_members(flasher, tmp_path):
    path = make_archive(tmp_path, extra=("../outside.bin", b"untrusted"))
    output = tmp_path / "output"
    output.mkdir()
    manifest, images = flasher.unpack_images(path, output, "2026_05_15", "m2_pcie_x1")
    assert manifest["configuration"]["pcie_lanes"] == 1
    assert [path.read_bytes() for path in images] == [b"fallback image", b"operational image"]
    assert not (tmp_path / "outside.bin").exists()


def test_rejects_image_larger_than_multiboot_slot(flasher, tmp_path):
    path = make_archive(tmp_path, extra=("irrelevant", b"data"))
    flasher.SLOT_SIZE = 4
    with pytest.raises(flasher.FlashError, match="Invalid size"):
        flasher.unpack_images(path, tmp_path, "2026_05_15", "m2_pcie_x1")


# Probe Selection ----------------------------------------------------------------------------------

def test_probe_selection_requires_unambiguous_target(flasher):
    output = """Bus device vid:pid       probe_type manufacturer serial product
001 016    0x0403:0x6011 ft4232     FTDI         none   Quad RS232-HS
002 003    0x0403:0x6010 ft2232     FTDI         none   Dual RS232-HS
"""
    with pytest.raises(flasher.FlashError, match="Multiple"):
        flasher.select_probe(output)
    assert flasher.select_probe(output, "001:016") == ("001:016", "ft4232")
    assert flasher.select_probe(output, cable="ft2232") == ("002:003", "ft2232")
    with pytest.raises(flasher.FlashError, match="No matching"):
        flasher.select_probe(output, "001:017")


# Flash Model --------------------------------------------------------------------------------------

@pytest.mark.parametrize("detected", ["", "idcode 0x12345678", "idcode 0x3636093\nidcode 0x3636093"])
def test_rejects_missing_wrong_or_multiple_fpgas_before_dna_read(flasher, detected):
    commands = []

    def run(*flags):
        commands.append(flags)
        return detected

    with pytest.raises(flasher.FlashError):
        flasher.identify_board(run)
    assert commands == [("--detect",)]


class FakeBoard:
    """A flash memory model independent of the flasher's expected image hashes."""

    def __init__(self, flasher, images):
        self.flasher           = flasher
        self.images            = images
        self.memory            = bytearray(b"\xff") * 0x1000000
        self.commands          = []
        self.dna               = "00381c891c104854"
        self.boot              = 0x5
        self.silent_corruption = False
        self.fail_write        = False
        self.fail_backup       = False
        self.short_backup      = False

    def __call__(self, *flags):
        self.commands.append(flags)
        if "--scan-usb" in flags:
            return "001 016    0x0403:0x6011 ft4232 FTDI none Quad RS232-HS\n"
        if "--detect" in flags:
            return "index 0:\n\tidcode 0x3636093\n"
        if "--read-dna" in flags:
            return json.dumps({"dna": "0x" + self.dna})
        if "--read-register" in flags:
            value = 0x501079fc if flags[-1] == "STAT" else self.boot
            return f"Register raw value: 0x{value:x}\n"
        if "--write-flash" in flags:
            if self.fail_write:
                raise self.flasher.FlashError("injected write failure")
            offset = int(flags[flags.index("--offset") + 1], 0)
            data = Path(flags[-1]).read_bytes()
            self.memory[offset:offset + len(data)] = data
            if self.silent_corruption:
                self.memory[offset] ^= 0xff
        if "--dump-flash" in flags:
            size = int(flags[flags.index("--file-size") + 1])
            offset = int(flags[flags.index("--offset") + 1], 0) if "--offset" in flags else 0
            if size == 0x1000000:
                if self.fail_backup:
                    raise self.flasher.FlashError("injected backup failure")
                if self.short_backup:
                    size -= 1
            Path(flags[-1]).write_bytes(self.memory[offset:offset + size])
        return "Done\n"


@pytest.fixture
def board(flasher, tmp_path):
    images = [tmp_path / "fallback.bin", tmp_path / "operational.bin"]
    images[0].write_bytes(b"fallback contents" * 64)
    images[1].write_bytes(b"operational contents" * 64)
    return FakeBoard(flasher, images)


# Programming / Readback / Boot ---------------------------------------------------------------------

def test_programs_both_slots_preserves_other_bytes_and_verifies_boot(flasher, board, tmp_path):
    before = bytes(board.memory)
    report = {}
    flasher.flash_and_verify(board, board.images, tmp_path, report)
    assert (tmp_path / "flash-before.bin").read_bytes() == before
    for offset, image in zip((0, 0x800000), board.images):
        data = image.read_bytes()
        assert board.memory[offset:offset + len(data)] == data
        assert board.memory[offset + len(data):offset + 0x800000] == before[offset + len(data):offset + 0x800000]
    assert report["boot"] == {"STAT": "0x501079fc", "BOOTSTS": "0x00000005"}
    assert len(report["slots"]) == 2
    assert all(slot["verified"] for slot in report["slots"])
    writes = [command for command in board.commands if "--write-flash" in command]
    assert all("--verify" in command and "--skip-reset" in command for command in writes)
    assert "--skip-load-bridge" not in writes[0]
    assert "--skip-load-bridge" in writes[1]


@pytest.mark.parametrize("failure", ["fail_backup", "short_backup", "fail_write", "silent_corruption"])
def test_failures_never_reach_boot_pass_and_reset_bridge(flasher, board, tmp_path, failure):
    setattr(board, failure, True)
    report = {}
    with pytest.raises(flasher.FlashError):
        flasher.flash_and_verify(board, board.images, tmp_path, report)
    assert "boot" not in report
    if failure in ("fail_backup", "short_backup"):
        assert not any("--write-flash" in command for command in board.commands)
    else:
        assert board.commands[-1] == ("--reset",)


def test_verify_only_never_writes_flash(flasher, board, tmp_path):
    for offset, image in zip((0, 0x800000), board.images):
        data = image.read_bytes()
        board.memory[offset:offset + len(data)] = data
    before = bytes(board.memory)
    report = {}
    flasher.flash_and_verify(board, board.images, tmp_path, report, verify_only=True)
    assert board.memory == before
    assert not any("--write-flash" in command for command in board.commands)
    assert report["boot"]["BOOTSTS"] == "0x00000005"


@pytest.mark.parametrize("status,boot", [
    (0x501079fd, 0x5),  # CRC error.
    (0x501079fc, 0x7),  # Fallback, even though DONE is high.
    (0x501079fc, 0x1),  # No multiboot jump.
    (0x501079fc, 0x2105),  # Older valid entry contains a CRC error.
])
def test_rejects_configuration_and_multiboot_errors(flasher, status, boot):
    def run(*flags):
        return f"Register raw value: 0x{status if flags[-1] == 'STAT' else boot:x}"

    with pytest.raises(flasher.FlashError):
        flasher.check_boot(run)


def test_boot_waits_before_accessing_configuration_registers(flasher, monkeypatch):
    elapsed     = 0
    interrupted = False

    def sleep(seconds):
        nonlocal elapsed
        elapsed += seconds

    def run(*flags):
        nonlocal interrupted
        # Model CFG_IN taking over SPI configuration when accessed during boot.
        if elapsed < 2:
            interrupted = True
        if flags[-1] == "STAT":
            status = 0x5000190c if interrupted else 0x501079fc
            return f"Register raw value: 0x{status:x}"
        return "Register raw value: 0x5"

    monkeypatch.setattr(flasher.time, "sleep", sleep)
    monkeypatch.setattr(flasher.time, "monotonic", lambda: elapsed)
    assert flasher.check_boot(run) == {"STAT": "0x501079fc", "BOOTSTS": "0x00000005"}
    assert not interrupted


def test_boot_timeout_is_reported(flasher, monkeypatch):
    ticks = iter([0, 6])
    monkeypatch.setattr(flasher.time, "monotonic", lambda: next(ticks))
    with pytest.raises(flasher.FlashError, match="did not finish booting"):
        flasher.check_boot(lambda *flags: "Register raw value: 0x0")


# Board Records / Batch Mode -----------------------------------------------------------------------

def test_records_board_result_and_skips_duplicate_dna(flasher, board, tmp_path, monkeypatch):
    monkeypatch.setattr(flasher, "run_loader", lambda command, log: board(*command))
    args = flasher.argument_parser().parse_args(["--yes", "--no-backup", "--output-dir", str(tmp_path)])
    release = {"tag": "2026_05_15", "image": "m2_pcie_x1", "images": board.images}
    seen = set()
    assert flasher.process_board(args, release, seen) == "passed"
    write_count = sum("--write-flash" in command for command in board.commands)
    assert flasher.process_board(args, release, seen) == "skipped"
    assert sum("--write-flash" in command for command in board.commands) == write_count
    results = [json.loads(path.read_text()) for path in (tmp_path / "boards").glob("*/result.json")]
    assert sorted(result["result"] for result in results) == ["passed", "skipped"]
    assert all(result["fpga_dna"] == board.dna for result in results)


def test_records_failed_board(flasher, board, tmp_path, monkeypatch):
    board.silent_corruption = True
    monkeypatch.setattr(flasher, "run_loader", lambda command, log: board(*command))
    args = flasher.argument_parser().parse_args(["--yes", "--no-backup", "--output-dir", str(tmp_path)])
    release = {"tag": "2026_05_15", "image": "m2_pcie_x1", "images": board.images}
    seen = set()
    with pytest.raises(flasher.FlashError, match="readback mismatch"):
        flasher.process_board(args, release, seen)
    result = json.loads(next((tmp_path / "boards").glob("*/result.json")).read_text())
    assert result["result"] == "failed"
    assert result["fpga_dna"] == board.dna
    assert not seen


def test_dry_run_does_not_access_programmer(flasher, board, monkeypatch):
    release = {"tag": "2026_05_15", "image": "m2_pcie_x1", "archive_sha256": "a" * 64, "images": board.images}
    monkeypatch.setattr(flasher, "prepare_release", lambda args: release)

    def unexpected(*args):
        pytest.fail("Dry run accessed programmer")

    monkeypatch.setattr(flasher, "run_loader", unexpected)
    assert flasher.main(["--dry-run", "--loader", "/missing/programmer"]) == 0


def test_batch_pins_release_continues_after_failure_and_returns_failure(flasher, board, monkeypatch):
    release = {"tag": "2026_05_15", "image": "m2_pcie_x1", "archive_sha256": "a" * 64, "images": board.images}
    downloads = []
    boards = []

    def prepare(args):
        downloads.append(args.release)
        return release

    def process(args, selected, seen):
        boards.append(selected)
        if len(boards) == 1:
            raise flasher.FlashError("first board failed")
        seen.add(board.dna)
        return "passed"

    answers = iter(["", "", "q"])
    monkeypatch.setattr(flasher.sys.stdin, "isatty", lambda: True)
    monkeypatch.setattr(flasher.shutil, "which", lambda name: name)
    monkeypatch.setattr("builtins.input", lambda prompt: next(answers))
    monkeypatch.setattr(flasher, "prepare_release", prepare)
    monkeypatch.setattr(flasher, "process_board", process)
    assert flasher.main(["--batch"]) == 1
    assert downloads == ["latest"]
    assert boards == [release, release]
