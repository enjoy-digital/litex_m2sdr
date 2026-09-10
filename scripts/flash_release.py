#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Flash published multiboot images over USB JTAG, including production batches."""

import re
import sys
import json
import time
import shlex
import shutil
import hashlib
import zipfile
import argparse
import tempfile
import subprocess
import urllib.request

from pathlib import Path
from datetime import datetime, timezone

# Constants ----------------------------------------------------------------------------------------

REPOSITORY  = "enjoy-digital/litex_m2sdr"
SLOT_SIZE   = 0x00800000
FPGA_IDCODE = 0x03636093
FPGA_PART   = "xc7a200tsbg484"

IMAGES = {
    "m2_pcie_x1" : {
        "variant"    : "m2",
        "with_pcie"  : True,
        "pcie_lanes" : 1,
        "with_eth"   : False,
    },
    "m2_pcie_x2" : {
        "variant"    : "m2",
        "with_pcie"  : True,
        "pcie_lanes" : 2,
        "with_eth"   : False,
    },
    "baseboard_eth" : {
        "variant"    : "baseboard",
        "with_pcie"  : False,
        "with_eth"   : True,
    },
    "baseboard_eth_ptp_rfic_clock" : {
        "variant"                : "baseboard",
        "with_pcie"              : False,
        "with_eth"               : True,
        "with_eth_ptp"           : True,
        "with_eth_ptp_rfic_clock" : True,
    },
    "baseboard_pcie_x1_eth" : {
        "variant"    : "baseboard",
        "with_pcie"  : True,
        "pcie_lanes" : 1,
        "with_eth"   : True,
    },
}

FTDI_CABLES = {
    "6011" : "ft4232",
    "6010" : "ft2232",
    "6014" : "digilent_hs2",
}

# Helpers ------------------------------------------------------------------------------------------

class FlashError(RuntimeError):
    pass


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# Release Download ---------------------------------------------------------------------------------

def request(url):
    headers = {
        "User-Agent"          : "litex-m2sdr-flash-release",
        "Accept"              : "application/vnd.github+json",
        "X-GitHub-Api-Version" : "2026-03-10",
    }
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30)


def fetch_release(tag):
    if tag != "latest" and not re.fullmatch(r"\d{4}_\d{2}_\d{2}", tag):
        raise FlashError("Use --release latest or a date tag such as 2026_05_15.")
    endpoint = "latest" if tag == "latest" else f"tags/{tag}"
    with request(f"https://api.github.com/repos/{REPOSITORY}/releases/{endpoint}") as response:
        release = json.load(response)
    resolved = release.get("tag_name", "")
    if not re.fullmatch(r"\d{4}_\d{2}_\d{2}", resolved):
        raise FlashError(f"Unsupported release tag: {resolved!r}")
    if tag != "latest" and resolved != tag:
        raise FlashError("GitHub returned a different release than requested.")
    if release.get("draft") or release.get("prerelease"):
        raise FlashError("Production flashing requires a published stable release.")
    return release


def download_asset(asset, directory):
    digest = asset.get("digest") or ""
    if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
        raise FlashError("The release asset has no GitHub SHA-256 digest; refusing to flash.")
    expected = digest.split(":", 1)[1].lower()
    size = asset.get("size", 0)
    if not isinstance(size, int) or not 0 < size <= 64 * 1024 * 1024:
        raise FlashError("Invalid release archive size.")
    path = directory / asset["name"]
    if path.exists() and path.stat().st_size == size and sha256(path) == expected:
        print(f"Using verified archive: {path}")
        return path
    url = asset["browser_download_url"]
    if not url.startswith(f"https://github.com/{REPOSITORY}/releases/download/"):
        raise FlashError("Unexpected release download URL.")
    print(f"Downloading {asset['name']}...")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=directory, delete=False) as output:
            temporary = Path(output.name)
            with request(url) as response:
                total = 0
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    total += len(chunk)
                    if total > size:
                        raise FlashError("Release download exceeds its advertised size.")
                    output.write(chunk)
        if temporary.stat().st_size != size or sha256(temporary) != expected:
            raise FlashError("Release archive size or SHA-256 mismatch.")
        temporary.replace(path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return path


# Release Images -----------------------------------------------------------------------------------

def unpack_images(archive_path, directory, tag, image):
    build = f"litex_m2sdr_{image}"
    names = ["release_manifest.json", f"{build}_fallback.bin", f"{build}_operational.bin"]
    with zipfile.ZipFile(archive_path) as archive:
        # Read only these exact members, never extract arbitrary archive paths.
        for name in names:
            if archive.namelist().count(name) != 1:
                raise FlashError(f"Archive must contain exactly one {name}.")
            limit = 64 * 1024 if name.endswith(".json") else SLOT_SIZE
            if not 0 < archive.getinfo(name).file_size <= limit:
                raise FlashError(f"Invalid size for {name}.")
        manifest = json.loads(archive.read(names[0]))
        expected = {
            "project"      : "litex_m2sdr",
            "release_date" : tag,
            "build_name"   : build,
        }
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise FlashError(f"Release manifest {key} does not match {value!r}.")
        if manifest.get("git_dirty") is not False or not re.fullmatch(
            r"[0-9a-fA-F]{40}", manifest.get("git_revision", "")
        ):
            raise FlashError("Release manifest must identify a clean source revision.")
        for key, value in IMAGES[image].items():
            if manifest.get("configuration", {}).get(key) != value:
                raise FlashError(f"Release configuration {key} does not match {value!r}.")
        contents = {name: archive.read(name) for name in names}
    # Both images have been checked before either is made available to the flasher.
    for name, data in contents.items():
        (directory / name).write_bytes(data)
    return manifest, [directory / name for name in names[1:]]


def prepare_release(args):
    release = fetch_release(args.release)
    tag     = release["tag_name"]
    name    = f"litex_m2sdr_{args.image}_{tag}.zip"
    assets  = [asset for asset in release.get("assets", []) if asset.get("name") == name]
    if len(assets) != 1:
        raise FlashError(f"Release {tag} does not contain exactly one {name}.")
    directory = args.output_dir / "releases" / tag / args.image
    directory.mkdir(parents=True, exist_ok=True)
    archive = download_asset(assets[0], directory)
    manifest, images = unpack_images(archive, directory, tag, args.image)
    write_json(directory / "github-release.json", release)
    return {
        "tag"            : tag,
        "image"          : args.image,
        "archive_sha256" : sha256(archive),
        "git_revision"   : manifest["git_revision"],
        "images"         : images,
    }


# JTAG Programmer ----------------------------------------------------------------------------------

def run_loader(command, log):
    rendered = "+ " + shlex.join(str(part) for part in command) + "\n"
    print(rendered, end="", flush=True)
    log.write(rendered)
    log.flush()
    output = []
    with subprocess.Popen(command,
        stdout  = subprocess.PIPE,
        stderr  = subprocess.STDOUT,
        text    = True,
        errors  = "replace",
        bufsize = 1,
    ) as process:
        for line in process.stdout:
            output.append(line)
            print(line, end="", flush=True)
            log.write(line)
            log.flush()
        returncode = process.wait()
    if returncode:
        raise FlashError(f"openFPGALoader failed (exit {returncode}); see programmer.log.")
    return "".join(output)


def select_probe(output, busdev=None, cable=None):
    probes = []
    for bus, device, pid in re.findall(
        r"^\s*(\d+)\s+(\d+)\s+0x0403:0x(6011|6010|6014)\b", output, re.MULTILINE
    ):
        address = f"{int(bus):03d}:{int(device):03d}"
        if busdev and address != busdev:
            continue
        if cable and FTDI_CABLES[pid] != cable:
            continue
        probes.append((address, FTDI_CABLES[pid]))
    if not probes:
        raise FlashError("No matching FTDI JTAG probe found. Check USB, permissions and probe selection.")
    if len(probes) != 1:
        raise FlashError("Multiple FTDI probes found; select one with --busdev-num BUS:DEVICE.")
    return probes[0]


# Board Identification / Boot Status ---------------------------------------------------------------

def identify_board(run):
    output = run("--detect")
    ids    = re.findall(r"\bidcode\s+0x([0-9a-fA-F]+)", output)
    if not ids:
        raise FlashError("No FPGA found on JTAG. Check board power and the JTAG connection.")
    if len(ids) != 1 or (int(ids[0], 16) & 0x0fffffff) != FPGA_IDCODE:
        raise FlashError("Expected one XC7A200T FPGA on the JTAG chain.")
    output = run("--read-dna")
    match  = re.search(r'"dna"\s*:\s*"0x([0-9a-fA-F]+)"', output)
    if not match or not 0 < int(match[1], 16) < (1 << 57) - 1:
        raise FlashError("Unable to read a valid FPGA DNA identifier.")
    return f"{int(match[1], 16):016x}"


def register_value(output):
    match = re.search(r"Register raw value:\s*0x([0-9a-fA-F]+)", output)
    if not match:
        raise FlashError("Cannot parse FPGA status; use an openFPGALoader version with --read-register.")
    return int(match[1], 16)


def check_boot(run):
    deadline = time.monotonic() + 5
    ready    = (1 << 14) | (1 << 4) # DONE and end of startup (UG470 STAT).
    errors   = (1 << 0) | (1 << 15) | (1 << 16) | (1 << 17)
    while True:
        status = register_value(run("--read-register", "STAT"))
        if status & errors:
            raise FlashError(f"FPGA configuration error: STAT=0x{status:08x}.")
        if (status & ready) == ready:
            break
        if time.monotonic() >= deadline:
            raise FlashError(f"FPGA did not finish booting: STAT=0x{status:08x}.")
        time.sleep(0.5)
    boot = register_value(run("--read-register", "BOOTSTS"))
    # Current entry must be valid, reached through IPROG and have no fallback/errors.
    # Check the older entry's errors too, when that entry is valid (UG470 BOOTSTS).
    if (boot & 0xff) != 0x05 or (boot & 0x100 and boot & 0xfa00):
        raise FlashError(f"Multiboot reported an error or fallback: BOOTSTS=0x{boot:08x}.")
    return {
        "STAT"    : f"0x{status:08x}",
        "BOOTSTS" : f"0x{boot:08x}",
    }


# Multiboot Flashing --------------------------------------------------------------------------------

def flash_and_verify(run, images, directory, report, backup=True, verify_only=False):
    bridge_active = False
    try:
        if backup and not verify_only:
            path = directory / "flash-before.bin"
            bridge_active = True
            run("--dump-flash", "--file-size", str(2 * SLOT_SIZE), str(path))
            bridge_active = False
            if path.stat().st_size != 2 * SLOT_SIZE:
                raise FlashError("Incomplete backup of the two multiboot slots.")
            report["backup_sha256"] = sha256(path)
        if not verify_only:
            for index, path in enumerate(images):
                bridge_active = True
                flags = ["--skip-load-bridge"] if index else []
                run(
                    "--write-flash", "--offset", hex(index * SLOT_SIZE),
                    "--verify", "--skip-reset", *flags, str(path),
                )
        # Compare actual flash bytes as well as using the programmer's verification.
        # This also catches loader versions that do not propagate a verify failure.
        report["slots"] = []
        for index, path in enumerate(images):
            readback = directory / f"{path.stem}-readback.bin"
            flags    = ["--skip-load-bridge"] if bridge_active else []
            if index == 0:
                flags.append("--skip-reset")
            bridge_active = True
            run(
                "--dump-flash", "--offset", hex(index * SLOT_SIZE),
                "--file-size", str(path.stat().st_size), *flags, str(readback),
            )
            bridge_active = index == 0
            expected = sha256(path)
            actual   = sha256(readback)
            if readback.stat().st_size != path.stat().st_size or actual != expected:
                raise FlashError(f"Flash readback mismatch at {hex(index * SLOT_SIZE)}.")
            report["slots"].append({
                "image"    : path.name,
                "offset"   : hex(index * SLOT_SIZE),
                "size"     : path.stat().st_size,
                "sha256"   : actual,
                "verified" : True,
            })
        report["boot"] = check_boot(run)
    finally:
        if bridge_active:
            # A failed/interrupted operation must not leave the SPI bridge running.
            try:
                run("--reset")
            except (FlashError, OSError) as error:
                print(f"Could not reset FPGA after failure: {error}", file=sys.stderr)


# Board Records ------------------------------------------------------------------------------------

def process_board(args, release, seen):
    runs = args.output_dir / "boards"
    runs.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = Path(tempfile.mkdtemp(prefix=timestamp + "-", dir=runs))
    report = {key: value for key, value in release.items() if key != "images"}
    report.update(
        started_utc = timestamp,
        result      = "failed",
        mode        = "verify" if args.verify_only else "flash",
    )
    try:
        with (directory / "programmer.log").open("w", encoding="utf-8") as log:
            probe_output = run_loader([args.loader, "--scan-usb"], log)
            busdev, cable = select_probe(probe_output, args.busdev_num, args.cable)
            command = [
                args.loader,
                "-c", cable,
                "--busdev-num", busdev,
                "--fpga-part", FPGA_PART,
                "--freq", str(args.freq),
            ]

            def run(*flags):
                return run_loader([*command, *flags], log)

            report.update(busdev=busdev, cable=cable)
            dna = identify_board(run)
            report["fpga_dna"] = dna
            print(f"Board FPGA DNA: {dna}")
            if dna in seen:
                report["result"] = "skipped"
                print("This board already passed in this batch. Connect the next board.")
                return "skipped"
            if not (args.yes or args.batch):
                action = "Read back and verify" if args.verify_only else "Flash both multiboot slots on"
                if input(f"{action} board {dna}? [y/N] ").strip().lower() not in ("y", "yes"):
                    report["result"] = "cancelled"
                    return "cancelled"
            flash_and_verify(run, release["images"], directory, report,
                backup      = not args.no_backup,
                verify_only = args.verify_only,
            )
            report["result"] = "passed"
            seen.add(dna)
            print(f"PASS: {dna} - {release['tag']} / {release['image']}")
            return "passed"
    except (FlashError, OSError, ValueError, EOFError, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
        raise
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(directory / "result.json", report)
        print(f"Board record: {directory}")


# Arguments ----------------------------------------------------------------------------------------

def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)

    # Release / Image.
    parser.add_argument("--release", default="latest", help="Release tag (default: latest stable).")
    parser.add_argument("--image",   default="m2_pcie_x1", choices=IMAGES,
        help="Release image (default: m2_pcie_x1).")

    # Flashing / Verification.
    parser.add_argument("--batch",       action="store_true", help="Prompt for successive boards with one release.")
    parser.add_argument("-y", "--yes",   action="store_true", help="Skip single-board confirmation.")
    parser.add_argument("--no-backup",   action="store_true", help="Skip the 16 MiB multiboot backup.")
    parser.add_argument("--verify-only", action="store_true", help="Read back flash and check boot; temporarily reconfigures the FPGA.")
    parser.add_argument("--dry-run",     action="store_true", help="Download and validate images without USB/JTAG access.")

    # JTAG Programmer.
    parser.add_argument("--cable",      default=None, choices=sorted(set(FTDI_CABLES.values())),
        help="FTDI cable type (default: auto-detect).")
    parser.add_argument("--busdev-num", default=None, help="USB probe address, e.g. 001:016.")
    parser.add_argument("--freq",       default=20000000, type=int, help="JTAG frequency in Hz (default: 20000000).")
    parser.add_argument("--loader",     default="openFPGALoader",   help="openFPGALoader executable.")

    # Release Cache / Board Records.
    parser.add_argument("--output-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "build" / "flash_release",
        help="Release cache, board logs and backups (default: build/flash_release).")
    return parser


# Main ---------------------------------------------------------------------------------------------

def main(argv=None):
    parser = argument_parser()
    args   = parser.parse_args(argv)
    if args.freq <= 0:
        parser.error("--freq must be positive")
    if args.busdev_num:
        if not re.fullmatch(r"\d{1,3}:\d{1,3}", args.busdev_num):
            parser.error("--busdev-num must use BUS:DEVICE, e.g. 001:016")
        args.busdev_num = ":".join(f"{int(value):03d}" for value in args.busdev_num.split(":"))
    if args.batch and not args.dry_run and not sys.stdin.isatty():
        parser.error("--batch needs an interactive terminal to signal each board change")
    args.output_dir = args.output_dir.resolve()
    try:
        if not args.dry_run and shutil.which(args.loader) is None:
            raise FlashError("openFPGALoader is required, including the XC7A200T SPI-over-JTAG bridge.")
        release = prepare_release(args)
        print(f"Release: {release['tag']} / {release['image']}")
        print(f"Archive SHA-256: {release['archive_sha256']}")
        for index, path in enumerate(release["images"]):
            print(f"  {path.name}: {path.stat().st_size} bytes at 0x{index * SLOT_SIZE:08x}")
        if args.dry_run:
            print("Dry run complete. No USB/JTAG access. Normal runs verify both slots and FPGA boot.")
            return 0
        seen     = set()
        failures = 0
        while True:
            if args.batch:
                answer = input("Connect the next board, then press Enter (q to finish): ").strip().lower()
                if answer == "q":
                    break
                if answer:
                    continue
            try:
                result = process_board(args, release, seen)
                if result == "cancelled":
                    break
            except (FlashError, OSError, ValueError) as error:
                failures += 1
                print(f"FAIL: {error}", file=sys.stderr)
            if not args.batch:
                break
        print(f"Completed: {len(seen)} board(s) passed, {failures} failed attempt(s).")
        return 1 if failures else 0
    except (FlashError, OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except (EOFError, KeyboardInterrupt):
        print("\nStopped. See the per-board records for completed or interrupted operations.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
