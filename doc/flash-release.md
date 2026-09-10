# Flashing Release Images

Install `openFPGALoader` with its `spiOverJtag_xc7a200tsbg484.bit.gz` bridge and USB access rules, then power the board and connect its FTDI USB JTAG programmer. The release flasher requires Python 3 and Internet access; Vivado and a working PCIe connection are not required.

Flash the latest stable release, using the **M.2 PCIe x1** image by default:

```bash
./scripts/flash_release.py
```

The tool verifies the GitHub archive SHA-256 and release manifest, identifies the XC7A200T and its FPGA DNA, and asks before flashing. It backs up the two multiboot slots, writes fallback at `0x00000000` and operational at `0x00800000`, reads back both images, and checks FPGA startup and multiboot status before reporting `PASS`.

For a production batch, press Enter after connecting each board and enter `q` to finish:

```bash
./scripts/flash_release.py --batch

# Pin a release for repeatable production runs.
./scripts/flash_release.py --batch --release 2026_05_15

# Skip backups when programming blank production boards.
./scripts/flash_release.py --batch --release 2026_05_15 --no-backup
```

The selected release stays fixed throughout a batch, and a board that already passed in that batch is skipped when its FPGA DNA is seen again. Downloads, per-board `programmer.log` / `result.json` files, readback images, and backups are saved under `build/flash_release/` (override with `--output-dir`). The backup covers the first 16 MiB containing both multiboot slots. Keep the board records with the production batch; they include the release, source revision, FPGA DNA, image hashes and result. `PASS` covers flash contents and FPGA boot; use the board autotests for PCIe and RF qualification.

Other useful operations:

```bash
# Validate the latest release download without accessing the board.
./scripts/flash_release.py --dry-run

# Recheck a flashed board without writing flash (temporarily reconfigures the FPGA).
./scripts/flash_release.py --release 2026_05_15 --verify-only

# Select another release image.
./scripts/flash_release.py --image m2_pcie_x2

# Select a probe when several are connected.
openFPGALoader --scan-usb
./scripts/flash_release.py --busdev-num 001:016
```

If an operation fails, inspect that board's log, check power/JTAG connections and USB permissions, and retry with the same `--release`. Batch mode allows another attempt and returns a nonzero exit status if any attempt failed. A lower JTAG frequency, such as `--freq 10000000`, can help with unreliable cables. After an interrupted write, rerun the full flashing operation over JTAG to restore both slots. `--yes` skips the single-board confirmation; batch mode always waits for the operator to signal the next board.
