import importlib.util
import json
import zipfile
from pathlib import Path
from types import SimpleNamespace

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "github_release.py"


def load_module():
    spec = importlib.util.spec_from_file_location("github_release", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_release_date_uses_archive_tag_format():
    github_release = load_module()

    assert github_release.release_date_to_iso("2026_05_15") == "2026-05-15"
    with pytest.raises(SystemExit):
        github_release.validate_release_date("2026-05-15")


def test_collect_release_assets_uses_expected_prefixes(tmp_path):
    github_release = load_module()

    assets = github_release.collect_release_assets(tmp_path, "2026_05_15")

    assert [asset.name for asset in assets] == [
        "litex_m2sdr_baseboard_eth_2026_05_15.zip",
        "litex_m2sdr_baseboard_eth_ptp_rfic_clock_2026_05_15.zip",
        "litex_m2sdr_baseboard_pcie_x1_eth_2026_05_15.zip",
        "litex_m2sdr_m2_pcie_x1_2026_05_15.zip",
        "litex_m2sdr_m2_pcie_x2_2026_05_15.zip",
    ]


def test_extract_changelog_notes_stops_at_next_release(tmp_path):
    github_release = load_module()
    changelog = tmp_path / "CHANGELOG.md"
    changelog.write_text(
        "\n".join([
            "# CHANGELOG",
            "",
            "[> 2026-05-15 First Date-Named Release",
            "--------------------------------------",
            "- First release note.",
            "",
            "[> 2026 Q2 to date (Apr - May)",
            "------------------------------",
            "- Development summary.",
        ]),
        encoding="utf-8",
    )

    notes = github_release.extract_changelog_notes(changelog, "2026_05_15")

    assert notes.startswith("# 2026-05-15 First Date-Named Release")
    assert "- First release note." in notes
    assert "Development summary" not in notes


def test_validate_asset_manifests_returns_shared_build_revision(tmp_path):
    github_release = load_module()
    asset = tmp_path / "litex_m2sdr_m2_pcie_x1_2026_05_15.zip"
    manifest = {
        "release_date": "2026_05_15",
        "git_revision": "0123456789abcdef",
        "git_dirty": False,
    }
    with zipfile.ZipFile(asset, "w") as archive:
        archive.writestr("release_manifest.json", json.dumps(manifest))

    revision = github_release.validate_asset_manifests([asset], "2026_05_15")

    assert revision == "0123456789abcdef"


def test_validate_asset_manifests_rejects_mismatched_dates(tmp_path):
    github_release = load_module()
    asset = tmp_path / "litex_m2sdr_m2_pcie_x1_2026_05_15.zip"
    manifest = {
        "release_date": "2026_05_14",
        "git_revision": "0123456789abcdef",
        "git_dirty": False,
    }
    with zipfile.ZipFile(asset, "w") as archive:
        archive.writestr("release_manifest.json", json.dumps(manifest))

    with pytest.raises(SystemExit):
        github_release.validate_asset_manifests([asset], "2026_05_15")


def test_build_release_commands_create_tag_push_and_release():
    github_release = load_module()
    args = SimpleNamespace(
        clobber=False,
        draft=False,
        prerelease=False,
        push_branch=False,
        remote="origin",
        reuse_tag=False,
        tag_message="LiteX M2SDR 2026_05_15",
        title="LiteX M2SDR 2026_05_15",
        upload_only=False,
    )

    commands = github_release.build_release_commands(
        args=args,
        tag="2026_05_15",
        tag_revision="0123456789abcdef",
        assets=[Path("build/litex_m2sdr_m2_pcie_x1_2026_05_15.zip")],
        notes="release notes",
        tag_exists=False,
    )

    assert commands[0] == [
        "git", "tag", "-a", "2026_05_15", "0123456789abcdef",
        "-m", "LiteX M2SDR 2026_05_15",
    ]
    assert commands[1] == ["git", "push", "origin", "2026_05_15"]
    assert commands[2][:7] == [
        "gh", "release", "create", "2026_05_15",
        "--verify-tag", "--title", "LiteX M2SDR 2026_05_15",
    ]
    assert commands[2][7:9] == ["--notes", "release notes"]
    assert commands[2][-1] == "build/litex_m2sdr_m2_pcie_x1_2026_05_15.zip"
