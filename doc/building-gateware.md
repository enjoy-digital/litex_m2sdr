# Building And Loading The Gateware

Most users can flash a prebuilt release image (see [Flashing Release Images](flash-release.md)).
This page covers building the FPGA gateware from source, loading/flashing it, and producing
release archives.

## Prerequisites

Install LiteX and Vivado following the
[LiteX installation guide](https://github.com/enjoy-digital/litex/wiki/Installation).

The full list of build options is available with `./litex_m2sdr.py --help`; the
[Capabilities table](../README.md#capabilities) maps features to their build flags.

## Common Builds

`--build` builds the bitstream, `--load` loads it into the FPGA over JTAG (volatile).

PCIe, board in an M.2 slot:

```bash
./litex_m2sdr.py --variant=m2 --with-pcie --build --load
lspci
```

PCIe, board on the LiteX Acorn Baseboard Mini:

```bash
./litex_m2sdr.py --variant=baseboard --with-pcie --build --load
lspci
```

Ethernet (1000BASE-X), board on the Acorn Baseboard Mini:

```bash
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build --load
ping 192.168.1.50
```

Ethernet (2.5GBASE-X) and other Ethernet options are described in [Ethernet](ethernet.md).
Feature-specific builds are documented on their own pages:

- SATA storage: [SATA Workflows](sata-workflows.md)
- Ethernet PTP / PTP-disciplined RFIC reference: [Ethernet PTP Bring-Up](ptp/README.md)
- PCIe PTM host time sync: [PCIe PTM](pcie-ptm.md)
- White Rabbit: [White Rabbit on M2SDR](white-rabbit.md)
- 122.88 MSPS 2T2R oversampling: [Wide Analog Bandwidth](wide-bandwidth.md)
- External PPS input selection: [Hardware Reference](hardware.md#external-pps-input)

## Host Bridges (JTAGBone / PCIeBone)

Start a LiteX server to access the SoC CSRs (MMAP peek & poke, LiteScope) over JTAG or PCIe:

```bash
litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg # JTAGBone
sudo litex_server --pcie --pcie-bar=04:00.0              # PCIeBone (adapt BAR)
```

See the [Debugging Guide](debugging-guide.md#host-access) for Etherbone and LiteScope usage.

## Flashing Over PCIe

Write a bitstream to the operational multiboot slot of the SPI flash over PCIe:

```bash
cd litex_m2sdr/software
./flash.py ../build/litex_m2sdr_platform/litex_m2sdr/gateware/litex_m2sdr_platform.bin
```

Then reboot, or remove the board and rescan the PCIe bus:

```bash
echo 1 | sudo tee /sys/bus/pci/devices/0000\:0X\:00.0/remove # Replace X with actual value
echo 1 | sudo tee /sys/bus/pci/rescan
```

`./flash.py --rescan` and `litex_m2sdr/software/rescan.py` automate the driver unload / remove /
rescan / reload sequence.

To flash over JTAG (no PCIe or Vivado needed), see [Flashing Release Images](flash-release.md).

## Release Artifacts

Date-named release archives are generated with:

```bash
./release.py
```

The release script checks the final Vivado timing report before creating each archive, so a
bitstream with setup/hold timing failures is not packaged. Release manifests include the parsed
timing summary; PCIe images may record the known Xilinx PCIe IP pulse-width warning when setup/hold
timing is otherwise clean. Ethernet-enabled builds default to a 100MHz system clock for timing
margin; PCIe-only M.2 builds keep the 125MHz system clock. The release matrix builds the core
PCIe/Ethernet images plus the validated Ethernet PTP RFIC-reference image:

| Archive prefix | Build command |
|----------------|---------------|
| `litex_m2sdr_baseboard_eth` | `./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build` |
| `litex_m2sdr_baseboard_eth_ptp_rfic_clock` | `./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --with-eth-ptp --with-eth-ptp-rfic-clock --build` |
| `litex_m2sdr_baseboard_pcie_x1_eth` | `./litex_m2sdr.py --variant=baseboard --with-pcie --pcie-lanes=1 --with-eth --eth-sfp=0 --build` |
| `litex_m2sdr_m2_pcie_x1` | `./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=1 --build` |
| `litex_m2sdr_m2_pcie_x2` | `./litex_m2sdr.py --variant=m2 --with-pcie --pcie-lanes=2 --build` |

Each `build/*_<YYYY_MM_DD>.zip` contains the `.bit`, `.bin`, multiboot fallback/operational images, CSR exports, and a JSON manifest. Generated archives and bitstreams are release artifacts and are not committed to git.

### Publishing A GitHub Release (maintainers)

GitHub release publication uses the same date string as the archive suffix, with no `v` prefix on
the tag. After generating the archives from the final release commit, validate the upload plan and
then publish it with:

```bash
scripts/github_release.py --date 2026_05_15 --dry-run
scripts/github_release.py --date 2026_05_15
```

The helper requires the GitHub CLI `gh` to be installed and authenticated with release write access. It checks for a clean tracked tree, verifies that the expected `build/*_2026_05_15.zip` files exist, reads each archive manifest, creates and pushes the annotated `2026_05_15` tag on the manifest git revision, extracts the matching `CHANGELOG.md` section as release notes, and uploads the archive set to the GitHub release.
