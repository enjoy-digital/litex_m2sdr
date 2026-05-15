#!/usr/bin/env python3

import argparse
import datetime
import json
import re
import shlex
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


BUILD_NAMES = [
    "litex_m2sdr_baseboard_eth",
    "litex_m2sdr_baseboard_eth_ptp_rfic_clock",
    "litex_m2sdr_baseboard_pcie_x1_eth",
    "litex_m2sdr_m2_pcie_x1",
    "litex_m2sdr_m2_pcie_x2",
]

DATE_RE = re.compile(r"^\d{4}_\d{2}_\d{2}$")


def default_release_date():
    return datetime.datetime.now().strftime("%Y_%m_%d")


def validate_release_date(release_date):
    if DATE_RE.fullmatch(release_date):
        return release_date
    raise SystemExit(
        "Release date must use YYYY_MM_DD format, for example 2026_05_15."
    )


def release_date_to_iso(release_date):
    validate_release_date(release_date)
    return release_date.replace("_", "-")


def command_text(command):
    return shlex.join(str(part) for part in command)


def run(command, dry_run=False):
    print("+ " + command_text(command))
    if not dry_run:
        subprocess.run(command, check=True)


def git_output(command):
    return subprocess.check_output(command, text=True).strip()


def ensure_clean_tracked_tree(allow_dirty=False):
    if allow_dirty:
        return
    status = git_output(["git", "status", "--short", "--untracked-files=no"])
    if status:
        raise SystemExit(
            "Tracked files are dirty; commit or restore them before publishing:\n"
            f"{status}\n"
            "Use --allow-dirty only for an intentional emergency release."
        )


def local_tag_exists(tag):
    command = ["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"]
    return subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0


def collect_release_assets(build_dir, release_date):
    validate_release_date(release_date)
    return [
        build_dir / f"{build_name}_{release_date}.zip"
        for build_name in BUILD_NAMES
    ]


def validate_assets(assets):
    missing = [asset for asset in assets if not asset.exists()]
    if missing:
        missing_list = "\n".join(f"  - {asset}" for asset in missing)
        raise SystemExit(f"Missing release assets:\n{missing_list}")


def extract_changelog_notes(changelog, release_date):
    iso_date = release_date_to_iso(release_date)
    if not changelog.exists():
        return f"LiteX M2SDR release {iso_date}"

    lines = changelog.read_text(encoding="utf-8").splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.startswith(f"[> {iso_date}"):
            start = index
            break

    if start is None:
        return f"LiteX M2SDR release {iso_date}"

    end = len(lines)
    for index in range(start + 1, len(lines)):
        if lines[index].startswith("[> "):
            end = index
            break

    title = lines[start].replace("[> ", "", 1).strip()
    body_start = start + 1
    if body_start < end and set(lines[body_start]) <= {"-"}:
        body_start += 1
    body = lines[body_start:end]

    notes = [f"# {title}", *body]
    return "\n".join(notes).strip()


def read_asset_manifest(asset):
    try:
        with zipfile.ZipFile(asset) as archive:
            with archive.open("release_manifest.json") as manifest:
                return json.loads(manifest.read().decode("utf-8"))
    except KeyError:
        raise SystemExit(f"{asset} does not contain release_manifest.json")
    except zipfile.BadZipFile:
        raise SystemExit(f"{asset} is not a valid zip archive")


def validate_asset_manifests(assets, release_date):
    revisions = set()
    dirty_assets = []
    for asset in assets:
        manifest = read_asset_manifest(asset)
        if manifest.get("release_date") != release_date:
            raise SystemExit(
                f"{asset} was built for {manifest.get('release_date')!r}, "
                f"not {release_date!r}."
            )
        if manifest.get("git_dirty"):
            dirty_assets.append(asset)
        revision = manifest.get("git_revision")
        if not revision:
            raise SystemExit(f"{asset} manifest has no git_revision field.")
        revisions.add(revision)

    if dirty_assets:
        dirty_list = "\n".join(f"  - {asset}" for asset in dirty_assets)
        raise SystemExit(f"Release assets were built from a dirty tree:\n{dirty_list}")
    if len(revisions) != 1:
        revision_list = "\n".join(f"  - {revision}" for revision in sorted(revisions))
        raise SystemExit(f"Release assets do not share one git revision:\n{revision_list}")
    return revisions.pop()


def git_revision(ref):
    return git_output(["git", "rev-parse", "--verify", ref])


def resolve_tag_revision(tag_revision, manifest_revision, allow_manifest_mismatch=False):
    if tag_revision == "manifest":
        return manifest_revision

    resolved = git_revision(tag_revision)
    if resolved != manifest_revision and not allow_manifest_mismatch:
        raise SystemExit(
            f"Requested tag revision {resolved} does not match release manifests "
            f"({manifest_revision}). Rebuild the archives or use "
            "--allow-manifest-mismatch intentionally."
        )
    return resolved


def build_release_commands(args, tag, tag_revision, assets, notes, tag_exists=None):
    if tag_exists is None:
        tag_exists = local_tag_exists(tag)

    if args.upload_only:
        command = ["gh", "release", "upload", tag]
        command.extend(str(asset) for asset in assets)
        if args.clobber:
            command.append("--clobber")
        return [command]

    if tag_exists:
        if not args.reuse_tag:
            raise SystemExit(
                f"Local tag {tag!r} already exists; use --reuse-tag to publish it."
            )
        commands = []
    else:
        commands = [["git", "tag", "-a", tag, tag_revision, "-m", args.tag_message]]

    if args.push_branch:
        commands.append(["git", "push", args.remote, "HEAD"])
    commands.append(["git", "push", args.remote, tag])

    command = [
        "gh", "release", "create", tag,
        "--verify-tag",
        "--title", args.title,
        "--notes", notes,
    ]
    if args.draft:
        command.append("--draft")
    if args.prerelease:
        command.append("--prerelease")
    command.extend(str(asset) for asset in assets)
    commands.append(command)
    return commands


def main():
    parser = argparse.ArgumentParser(
        description="Tag and publish a LiteX M2SDR date-named GitHub release."
    )
    parser.add_argument(
        "--date",
        default=default_release_date(),
        help="Release date in YYYY_MM_DD format. Defaults to today.",
    )
    parser.add_argument(
        "--tag",
        help="Git tag to publish. Defaults to the release date without a prefix.",
    )
    parser.add_argument(
        "--title",
        help="GitHub release title. Defaults to 'LiteX M2SDR <tag>'.",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="Directory containing the release zip archives.",
    )
    parser.add_argument(
        "--changelog",
        type=Path,
        default=Path("CHANGELOG.md"),
        help="Changelog file used to extract release notes.",
    )
    parser.add_argument(
        "--notes-file",
        type=Path,
        help="Release notes file. Overrides changelog extraction.",
    )
    parser.add_argument(
        "--remote",
        default="origin",
        help="Git remote used when pushing the release tag.",
    )
    parser.add_argument(
        "--tag-message",
        help="Annotated tag message. Defaults to the release title.",
    )
    parser.add_argument(
        "--tag-revision",
        default="manifest",
        help=(
            "Revision to tag. Defaults to 'manifest', which uses the git "
            "revision recorded in the release manifests."
        ),
    )
    parser.add_argument(
        "--push-branch",
        action="store_true",
        help="Push the current branch HEAD before pushing the release tag.",
    )
    parser.add_argument(
        "--reuse-tag",
        action="store_true",
        help="Publish an existing local tag instead of creating a new one.",
    )
    parser.add_argument(
        "--upload-only",
        action="store_true",
        help="Upload assets to an existing GitHub release without touching tags.",
    )
    parser.add_argument(
        "--clobber",
        action="store_true",
        help="Replace existing assets when used with --upload-only.",
    )
    parser.add_argument(
        "--draft",
        action="store_true",
        help="Create the GitHub release as a draft.",
    )
    parser.add_argument(
        "--prerelease",
        action="store_true",
        help="Mark the GitHub release as a prerelease.",
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow publishing with modified tracked files.",
    )
    parser.add_argument(
        "--allow-manifest-mismatch",
        action="store_true",
        help="Allow the tag revision to differ from the archive manifests.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the checks and commands without creating or pushing anything.",
    )
    args = parser.parse_args()

    release_date = validate_release_date(args.date)
    tag = args.tag or release_date
    title = args.title or f"LiteX M2SDR {tag}"
    args.title = title
    args.tag_message = args.tag_message or title

    ensure_clean_tracked_tree(args.allow_dirty)

    assets = collect_release_assets(args.build_dir, release_date)
    validate_assets(assets)
    manifest_revision = validate_asset_manifests(assets, release_date)
    tag_revision = resolve_tag_revision(
        args.tag_revision,
        manifest_revision,
        args.allow_manifest_mismatch,
    )

    if not args.dry_run and shutil.which("gh") is None:
        raise SystemExit(
            "GitHub CLI 'gh' was not found. Install it and authenticate with "
            "'gh auth login' before publishing."
        )

    if args.notes_file:
        notes = args.notes_file.read_text(encoding="utf-8").strip()
    else:
        notes = extract_changelog_notes(args.changelog, release_date)

    print(f"Release date : {release_date}")
    print(f"Release tag  : {tag}")
    print(f"Release title: {title}")
    print(f"Tag revision : {tag_revision}")
    print(f"Build revision: {manifest_revision}")
    print("Assets:")
    for asset in assets:
        print(f"  - {asset}")

    commands = build_release_commands(args, tag, tag_revision, assets, notes)

    if args.dry_run:
        print("\nRelease notes:")
        print(notes)
        print("\nDry-run commands:")

    for command in commands:
        run(command, dry_run=args.dry_run)

    return 0


if __name__ == "__main__":
    sys.exit(main())
