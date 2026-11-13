# Building the gateware {#building-gateware}

Most users flash a release image and never build the gateware
([Flashing a release image](#gs-flash)). This chapter is for the others: it builds a bitstream
from the sources, loads or flashes it, opens the host bridges, produces the release archives and
shows where to add processing of your own in the FPGA.

## At a glance {#build-glance}

| Item | Value |
|---------------------|-------------------------------------------------------------------------------|
| Target script | `litex_m2sdr.py`, one command line per configuration |
| Toolchain | LiteX and its cores, Vivado for the XC7A200T |
| Build directory | `build/<build>/`, named after the options |
| Bitstreams | Single image, fallback image and operational image |
| Register exports | `scripts/csr.csv`, `build/<build>/csr.json`, C headers of the software |
| JTAG programmer | openFPGALoader, through an FTDI cable |
| Flash over PCIe | `litex_m2sdr/software/flash.py` |
| Release archives | `release.py`, then `scripts/github_release.py` |
| Extension point | `BaseSoC.add_rfic_datapath()` |

## Prerequisites {#build-prerequisites}

The build needs LiteX with its cores and Vivado. Both are installed as the
[LiteX installation guide](https://github.com/enjoy-digital/litex/wiki/Installation) describes;
the `standard` configuration of `litex_setup.py` brings LitePCIe, LiteEth, LiteSATA, LiteScope
and the other cores the script imports.

Loading and flashing over JTAG need openFPGALoader and an FTDI cable on the JTAG port of the
board or of the baseboard ([Hardware](#hardware), [Carriers and hosts](#carriers)). A White Rabbit
build also needs the `litex_wr_nic` package ([Time synchronisation](#timing)).

The options are listed by `./litex_m2sdr.py --help` and described in
[Build options](#build-options). The rules between options, such as which host links can be
enabled together, are in [Option rules](#soc-rules).

## The build flow {#build-flow}

The figure below shows what one run of the script produces and how the result reaches the board.
The script elaborates the SoC, writes the Verilog and the constraints, and runs Vivado when
`--build` is given. The register exports are written at every run, with or without Vivado.

![Build flow: the script drives Vivado and exports the register map; the bitstream reaches the board over JTAG or over PCIe.](build_flow.svg)

```sh
# PCIe, board in an M.2 slot: build, then load over JTAG.
./litex_m2sdr.py --variant=m2 --with-pcie --build --load
```

The script runs its actions in a fixed order, whatever the order of the options: build, export
of the software headers, reset, load, flash, flash of the two multiboot images, PCIe rescan.
Each action is optional, so `--load` alone loads the bitstream of an earlier build of the same
configuration.

The files of a build are the following. `<build>` is the build name, for example
`litex_m2sdr_m2_pcie_x1`; [Build name and timing](#soc-build) gives the naming rule.

| File | Content |
|------------------------------------------------------------------|----------------------------------|
| `build/<build>/gateware/<build>.v` | Generated Verilog of the SoC |
| `build/<build>/gateware/<build>.bit` | Bitstream for a JTAG load |
| `build/<build>/gateware/<build>.bin` | Single flash image |
| `<build>_fallback.bin` | Fallback image of the multiboot pair |
| `<build>_operational.bin` | Operational image of the multiboot pair |
| `<build>_timing.rpt` | Vivado timing report |
| `build/<build>/gateware/vivado.log` | Vivado log |
| `build/<build>/csr.json` | Register map, JSON |
| `scripts/csr.csv` | Register map, CSV, for the LiteX host tools |
| `test/analyzer.csv` | Signal list of the LiteScope probe, when a probe is built |

The two multiboot images and the timing report are in the `gateware` directory, next to the
bitstream. [Flash and multiboot](#soc-flash) explains the role of each image.

## Common builds {#build-common}

The commands below are the usual starting points. `--build` runs Vivado and `--load` loads the
result into the FPGA over JTAG; a loaded bitstream is lost at the next power cycle.

```sh
# PCIe, board in an M.2 slot.
./litex_m2sdr.py --variant=m2 --with-pcie --build --load
lspci

# PCIe, board on the baseboard.
./litex_m2sdr.py --variant=baseboard --with-pcie --build --load
lspci

# Ethernet 1000BASE-X on the first SFP cage of the baseboard.
./litex_m2sdr.py --variant=baseboard --with-eth --eth-sfp=0 --build --load
ping 192.168.1.50
```

`lspci` lists the board once the PCIe link is up, and `ping` checks the Ethernet build at its
default address. The table gives the options of the other features and the chapter that
describes each of them.

| Feature | Options | Chapter |
|--------------------|----------------------------------------------|----------------------------------|
| PCIe on 2 or 4 lanes | `--pcie-lanes=2`, `--pcie-lanes=4` | [Host links](#host-links) |
| Ethernet 2500BASE-X | `--with-eth` `--eth-phy=2500basex` | [Host links](#host-links) |
| Ethernet 5GBASE-R | `--with-eth` `--eth-phy=5000baser` | [Host links](#host-links) |
| VRT streaming | `--with-eth` `--with-eth-vrt` | [Host links](#host-links) |
| SATA storage | `--with-sata` | [Host links](#host-links) |
| Oversampling mode | `--with-rfic-oversampling` | [The RFIC interface](#rfic) |
| PCIe PTM | `--with-pcie` `--with-pcie-ptm` | [Time synchronisation](#timing) |
| Ethernet PTP | `--with-eth` `--with-eth-ptp` | [Time synchronisation](#timing) |
| PTP-referenced RFIC clock | `--with-eth-ptp-rfic-clock` | [Time synchronisation](#timing) |
| White Rabbit | `--with-white-rabbit` | [Time synchronisation](#timing) |
| PPS input on a test point | `--pps-input=tp1`, `--pps-input=tp2` | [Hardware](#hardware) |
| GPIO | `--with-gpio` | [The SoC](#soc) |
| LiteScope probe | One of the `--with-*-probe` options | [Probes](#soc-probes) |

The status of each feature is in the [status table](#intro-status). The oversampling mode and
White Rabbit are Experimental.

Three combinations are not rejected with a clear message.

- The baseboard has one PCIe lane. `--variant=baseboard` with `--pcie-lanes=2` or `4` stops on
  an assertion without a message.
- The oversampling mode is for PCIe alone. The script accepts it with Ethernet or SATA, but these
  paths are 64 bits wide and lose half of each word: do not combine them.
- Ethernet and White Rabbit would share one PLL channel and its reset. The script accepts the
  pair: do not enable them together.

### Elaborating without Vivado {#build-elaborate}

Without `--build`, the script stops before Vivado. It elaborates the whole SoC, checks the
options and writes the Verilog and the register exports. This is the quick way to check that a
combination of options is accepted and to look at its register map.

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-sata
```

The continuous integration uses the same mode to check the release configurations and the White
Rabbit variants, and the register appendix of this manual is generated this way
([Register reference](#registers)).

### System clock {#build-sysclk}

The system clock is 125 MHz by default, and 100 MHz when Ethernet is enabled, which gives the
Ethernet builds more timing margin. The 2500BASE-X build is the exception: its transceiver takes
the system clock as reference, so it stays at 125 MHz and refuses another value.

`--sys-clk-freq` overrides the default, and the build name then receives a `_sysclk_<Hz>`
suffix. SATA sets a lower bound: the script refuses a system clock under 75 MHz for the default
SATA generation 2, and under 150 MHz for generation 3. With the default clocks `--sata-gen=3`
is therefore always refused: add `--sys-clk-freq=150e6` or more.

### Timing {#build-timing}

A bitstream is usable only when Vivado meets timing, and the script does not check it. Read the
summary of the timing report before loading a new build; the release flow does it automatically
([Release archives](#build-release)).

```sh
grep -n -A 6 "Design Timing Summary" build/<build>/gateware/<build>_timing.rpt
```

The worst negative slack (WNS) and the worst hold slack (WHS) must be positive or zero. When a
failure may come from timing, rebuild at a lower system clock before changing the design, for
example `--sys-clk-freq=100e6` on a PCIe build. [Build name and timing](#soc-build) describes
the constraints and the Vivado directives the script applies.

## Loading and flashing {#build-load}

The script drives openFPGALoader for the JTAG operations. It looks for an FTDI cable in the
output of `lsusb` (FT4232, FT2232 or FT232) and stops with an error when it finds none. The JTAG
clock is 20 MHz.

| Option | Action | Persistent |
|--------------------------------------|-------------------------------------------|-------------------|
| `--load` | Load `<build>.bit` into the FPGA | No |
| `--flash` | Write `<build>.bin` at offset 0 of the flash | Yes |
| `--flash-multiboot` | Write and verify the fallback and operational images | Yes |
| `--reset` | Reset the FPGA through the programmer | |
| `--rescan` | Run `rescan.py` after the load or the flash | |

Use `--load` while a design is being debugged. Flash only a bitstream that has been validated,
or when the boot from flash is the thing under test.

`--flash-multiboot` is the persistent form to prefer. It writes the fallback image at offset 0
and the operational image at `0x00800000`, the layout the release images use
([Flash and multiboot](#soc-flash)).

### After a load: PCIe rescan {#build-rescan}

After a load or a reload from flash, the host must enumerate the board again. A reboot does it.
Without a reboot, `rescan.py` unloads the kernel driver, removes the board from the PCIe bus,
rescans the bus and loads the driver again.

```sh
cd litex_m2sdr/software
sudo ./rescan.py
```

The same sequence by hand is a remove followed by a rescan. Replace the address with the one
`lspci` printed before the load.

```sh
echo 1 | sudo tee /sys/bus/pci/devices/0000\:04\:00.0/remove
echo 1 | sudo tee /sys/bus/pci/rescan
```

### Flashing over PCIe {#build-flash-pcie}

A board that already runs a PCIe bitstream can be updated without a JTAG cable. `flash.py`
writes an image to the flash through the registers of the SoC, then asks the FPGA to reload its
configuration. The default offset is the one of the operational image, so the fallback image
stays intact.

```sh
cd litex_m2sdr/software
./flash.py ../../build/<build>/gateware/<build>_operational.bin
```

| Option | Default | Effect |
|---------------------------|----------------------|---------------------------------------------------|
| `bitstream` | required | Image to write, path relative to `litex_m2sdr/software`; not absolute |
| `-o`, `--offset` | `0x00800000` | Flash offset; another value asks for a second confirmation |
| `-c`, `--device_num` | `0` | Board number, when several boards are in the host |
| `-r`, `--rescan` | off | Run `rescan.py` after the reload |

The script asks for a confirmation, then calls the `flash-write` and `flash-reload` commands of
`m2sdr_util` ([Host software](#software)). Reboot or rescan afterwards. When the new build changes
the register map, rebuild the software against the new headers before using the board
([Generated software headers](#build-headers)).

`scripts/flash_gateware.sh` chains the same steps without a reboot for a developer bench: flash
write, reload, driver unload, PCIe remove and rescan, driver load. The PCIe address of the board
is written in the script, as `0000:05:00.0` in its `PCIDEV` variable: edit it for the host before
use.

> With a wrong address the script stops after the flash write and the reload, at the PCIe remove
> step. If another device sits at this address, the script removes that device from the bus.

Flashing a release image over JTAG, with its checks and its backups, is the job of
`scripts/flash_release.py` ([Flashing a release image](#gs-flash)).

## Generated software headers {#build-headers}

The kernel driver and the library are compiled against three headers that describe the SoC. Each
run of the script, with or without `--build`, writes them again from the elaborated SoC.

| File | Content |
|---------------------------------------------------------------------|-------------------------------|
| `litex_m2sdr/software/kernel/csr.h` | Addresses and fields of the registers |
| `litex_m2sdr/software/kernel/soc.h` | Constants of the SoC |
| `litex_m2sdr/software/kernel/mem.h` | Memory regions |

These files are tracked by git, so building another configuration leaves them modified. Never
edit them by hand, and before committing them, check which build produced them.

The software works with every bitstream whose registers are in its headers, which is why the
register map only grows ([Register map](#soc-csr)).

`--driver` goes further: it copies the generic LitePCIe driver and tools over the software of
the repository before writing the headers. It is a maintenance option, used to pull an update
of LitePCIe; the software of the repository has its own changes on top of the generic one.

## Host bridges {#build-bridges}

A host bridge gives the LiteX host tools access to the registers of a running board, for
register reads and writes and for LiteScope captures. `litex_server` opens the bridge and the
tools connect to it ([Debugging](#debugging)).

```sh
# JTAGBone, through the FTDI cable.
litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg

# PCIeBone, through BAR0 of the board (adapt the address).
sudo litex_server --pcie --pcie-bar=04:00.0

# Etherbone, on an Ethernet build.
litex_server --udp --udp-ip=192.168.1.50 --udp-port=1234
```

JTAGBone is part of every build unless `--without-jtagbone` is given. It works when no host
link does, which makes it the bridge of last resort. The OpenOCD configuration file follows the
FTDI chip of the cable: `openocd_xc7_ft2232.cfg` or `openocd_xc7_ft4232.cfg`.

The PCIe and Ethernet bridges exist only in the builds that have the host link.

## Release archives {#build-release}

A release is a set of dated archives, one per release configuration. Each archive holds the
bitstreams of the configuration, its register exports and a manifest. The figure below shows
the flow from the build to the board of a user.

![Release flow: release.py builds and checks each configuration, github_release.py publishes the archives, flash_release.py installs one on a board.](build_release.svg)

```sh
./release.py
```

`release.py` builds the five configurations of the table one after the other and writes
`build/<build>_<YYYY_MM_DD>.zip` for each, with the date of the day.

| Build name | Options after `./litex_m2sdr.py` |
|-------------------------------------|---------------------------------------------------------------|
| `litex_m2sdr_` `baseboard_eth` | `--variant=baseboard` `--with-eth` `--eth-sfp=0` |
| `litex_m2sdr_` `baseboard_eth_` `ptp_rfic_clock` | The line above, with `--with-eth-ptp` `--with-eth-ptp-rfic-clock` |
| `litex_m2sdr_` `baseboard_` `pcie_x1_eth` | `--variant=baseboard` `--with-pcie` `--pcie-lanes=1` `--with-eth` `--eth-sfp=0` |
| `litex_m2sdr_` `m2_pcie_x1` | `--variant=m2` `--with-pcie` `--pcie-lanes=1` |
| `litex_m2sdr_` `m2_pcie_x2` | `--variant=m2` `--with-pcie` `--pcie-lanes=2` |

The build names are split at an underscore to fit the column. The other configurations are
built from the sources: SATA, VRT, 2500BASE-X, 5GBASE-R, PCIe on four lanes, the oversampling
mode and White Rabbit.

### Timing gate {#build-release-timing}

After each build, the script parses the design timing summary of the Vivado report and refuses
to package a bitstream that fails. Setup and hold must be clean: slack positive or zero, no
failing endpoint.

Pulse width must be clean too, with one exception for the PCIe builds. The 7-Series PCIe core
can show one failing pulse-width endpoint on its generated clocks. The script accepts exactly
one such endpoint, with a total violation of at most 50 ps, and records it in the manifest.

### Archive content {#build-release-content}

| File | Content |
|--------------------------------------------------------------|--------------------------------------|
| `<build>.bit` | Bitstream for a JTAG load |
| `<build>.bin` | Single flash image |
| `<build>_fallback.bin` | Fallback image |
| `<build>_operational.bin` | Operational image |
| `csr.csv`, `csr.json` | Register map of the build |
| `release_manifest.json` | Description of the build |

The manifest records the project, the release date, the generation time, the build name, the
build command, the configuration, the git revision, whether the tree had uncommitted changes,
and the parsed timing summary. The archives and the bitstreams are never committed.

The script saves the three generated software headers before the first build and restores them
at the end, so a release run leaves the tree as it found it.

`./release.py --nobuild` runs the same flow without Vivado. It elaborates each configuration
and archives the files that exist, which checks the names and the layout.

### Publishing a release {#build-publish}

`scripts/github_release.py` publishes the archives of one date as a GitHub release. The tag is
the date itself, without prefix. The script needs the GitHub command line tool `gh`,
authenticated with the right to create releases.

```sh
# Print the checks and the commands without creating anything.
scripts/github_release.py --date <YYYY_MM_DD> --dry-run

# Tag, push the tag and create the release with its archives.
scripts/github_release.py --date <YYYY_MM_DD>
```

The script checks that the tracked files are clean and that the five archives of the date
exist. It then reads each manifest: all must carry this date, a clean tree and the same git
revision. It creates an annotated tag on that revision, pushes it and creates the release.

The release notes are the section of `CHANGELOG.md` whose title starts with the date in the
`YYYY-MM-DD` form. Without such a section, the notes are one line with the date.

| Option | Default | Effect |
|-------------------------------------------|------------------------------|---------------------------|
| `--date` | Today | Release date, `YYYY_MM_DD` |
| `--tag` | The date | Git tag to publish |
| `--title` | `LiteX M2SDR <tag>` | Title of the release |
| `--build-dir` | `build` | Directory of the archives |
| `--changelog` | `CHANGELOG.md` | File the notes are extracted from |
| `--notes-file` | none | Notes file, instead of the changelog |
| `--remote` | `origin` | Remote the tag is pushed to |
| `--tag-message` | The title | Message of the annotated tag |
| `--tag-revision` | `manifest` | Revision to tag; `manifest` is the one of the archives |
| `--push-branch` | off | Push the branch before the tag |
| `--reuse-tag` | off | Publish an existing local tag |
| `--upload-only` | off | Add the archives to an existing release |
| `--clobber` | off | With `--upload-only`, replace existing archives |
| `--draft` | off | Create the release as a draft |
| `--prerelease` | off | Mark the release as a prerelease |
| `--allow-dirty` | off | Accept modified tracked files |
| `--allow-manifest-mismatch` | off | Accept a tag revision that differs from the manifests |
| `--dry-run` | off | Print only |

### What the continuous integration builds {#build-ci}

No workflow runs Vivado. The `build` job of `.github/workflows/ci.yml` installs LiteX, runs
`./release.py --nobuild`, elaborates the White Rabbit variants and runs the test suite
([Tests](#debug-tests)). It uploads the archives of the elaboration as a workflow artifact.

The `software-build` job of the same file and `.github/workflows/build.yml` compile the kernel
driver, the utilities, the library and the SoapySDR driver.

## Extending the gateware {#build-extend}

The base design uses a part of the XC7A200T, and the rest is available for processing in the
FPGA: a channelizer, a correlator, a repeater from RX to TX, or any block that offloads the
host. A project adds such a block without modifying `litex_m2sdr.py`: it subclasses `BaseSoC`
and overrides one method.

### The hook {#build-extend-hook}

`BaseSoC` calls `add_rfic_datapath()` once, with the four streams that surround the RFIC. The
default implementation connects them straight through, so the stock design is unchanged.
[Streaming](#streaming) describes where the hook sits in the sample path.

| Argument | Direction | Stream |
|---------------------------|---------------------|----------------------------------------------------|
| `rfic_source` | From the RFIC | RX samples of the RFIC |
| `rx_sink` | To the host | RX stream, before the loopback and the RX header |
| `tx_source` | From the host | TX stream, after the TX header, the timed TX gate and the loopback |
| `rfic_sink` | To the RFIC | TX samples of the RFIC |

Three properties make the hook simple to use. The streams carry raw I/Q words and no header: the
RX timestamps are inserted after the hook, so processed samples are timestamped as usual. The
streams are in the system clock domain.

The digital loopback of the host stays on the host side of the hook, so the loopback tests keep
exercising the unprocessed path.

The streams are 64 bits wide, and 128 bits wide in a build with `--with-rfic-oversampling`.
[The RFIC interface](#rfic) gives the layout of the words.

### A subclass {#build-extend-example}

The example below inserts a block on the RX path and leaves TX untouched. The buffer stands for
the processing. The script is loaded by its path because the repository also holds a Python
package named `litex_m2sdr`, which hides the script from a plain `import`.

```python
import importlib.util

from litex.soc.interconnect import stream
from litex.soc.integration.builder import Builder
from litepcie.common import dma_layout

# Run from the repository root.
spec  = importlib.util.spec_from_file_location("litex_m2sdr_soc", "litex_m2sdr.py")
m2sdr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m2sdr)

class MySoC(m2sdr.BaseSoC):
    def add_rfic_datapath(self, rfic_sink, rfic_source, tx_source, rx_sink):
        self.dsp = stream.Buffer(dma_layout(64))
        self.comb += [
            tx_source.connect(rfic_sink),        # TX unchanged.
            rfic_source.connect(self.dsp.sink),  # RFIC RX samples to the processing.
            self.dsp.source.connect(rx_sink),    # Processing to the host RX stream.
        ]

soc     = MySoC(variant="m2", with_pcie=True, pcie_dmas=2)
builder = Builder(soc, output_dir="build/my_soc", csr_csv="scripts/csr.csv")
builder.build(run=True)
```

`BaseSoC` takes the options of the command line as keyword arguments, with the same names
without the leading dashes ([Build options](#build-options)). The base class keeps providing
the host links, the clocking, the clock generator, the board time and the RFIC interface.

Three points differ from the command line. `BaseSoC` uses a 125 MHz system clock unless
`sys_clk_freq` is given: pass `sys_clk_freq=int(100e6)` for an Ethernet build. `wr_sfp` has no
automatic value. `--wr-status` and `--wr-firmware-target` are handled by the script and are not
arguments.

### Topologies {#build-extend-topologies}

- Processing or lossless tap on RX: route `rfic_source` through the block, or tap it, and return
  the samples to `rx_sink`. The host keeps its RX stream.
- Injection on TX: connect the output of the block to `rfic_sink`, instead of `tx_source` or
  mixed with it.
- Repeater from RX to TX: feed `rfic_source` to the block and its output to `rfic_sink`;
  `rx_sink` can carry an observation copy to the host.
- Results on a side channel: build with more than one PCIe DMA channel and send the results on
  the extra channels.

For the side channel, `--pcie-dmas` (or the `pcie_dmas` argument) sets the number of DMA
channels. Channel 0 keeps the samples. The hook runs after the host links are created, so the
extra channels exist when it is called, as `self.pcie_dma1` and the next ones.

> The extra channels have a start synchroniser whose PPS input is not connected, and the kernel
> driver arms it on every channel. A design that uses an extra channel connects the PPS pulse to
> the synchroniser of this channel, as the SoC does for channel 0.

The kernel driver sets the buffer size and the interrupt rate per channel, so a low-rate side
channel can use small buffers ([Host software](#software)).

### LiteDSP {#build-extend-litedsp}

The generic signal processing blocks that fit this hook, such as oscillators, mixers, CIC and
FIR filters, are gathered in [LiteDSP](https://github.com/enjoy-digital/litedsp), a LiteX core
of its own. The hook is the place where a LiteDSP chain is connected to the sample streams of
the board.

### Adding registers {#build-extend-registers}

A new block usually comes with registers. The steps below keep the software and the gateware in
step.

1. Write the block in `litex_m2sdr/gateware/`, or in the project that subclasses `BaseSoC`.
2. Instantiate it from the SoC: a module with registers becomes a register bank that has the
   name of its attribute.
3. Give the bank a fixed location in `BaseSoC.csr_map` when software depends on it, and never
   reuse or move an existing location ([Register map](#soc-csr)).
4. Run the script so that `scripts/csr.csv` and the software headers match the SoC.
5. Add the host access in the library, in `m2sdr_util` or in a script when the register is meant
   to stay.

A gateware block that is itself a master of the main bus, as the sequencer of the clock
generator is, needs the full bus address of the bank it accesses.
`self.get_csr_address("<bank>")` returns it.

The address map of the register bus, `self.csr.address_map("<bank>", origin=True)`, returns an
offset inside the register space. It is not a bus address and must not be given to a master of
the main bus.

## In the code {#build-code}

| File | Role |
|---------------------------------------------------------|-------------------------------------------|
| `litex_m2sdr.py` | `BaseSoC`, `add_rfic_datapath()`, `main()`: options, build name, actions |
| `litex_m2sdr_platform.py` | Bitstream commands, multiboot images, programmer |
| `litex_m2sdr/software/__init__.py` | Export of the software headers, PCIe rescan helpers |
| `litex_m2sdr/software/flash.py` | Flash over PCIe |
| `litex_m2sdr/software/rescan.py` | PCIe rescan and driver reload |
| `release.py` | Release configurations, timing gate, archives |
| `scripts/github_release.py` | Publication of a release |
| `scripts/flash_gateware.sh` | Flash and rescan without reboot |
| `.github/workflows/ci.yml` | Elaboration, tests and software builds |

## Where to go next {#build-next}

[Build options](#build-options) lists every option. [The SoC](#soc) describes what the options
build, [Debugging](#debugging) what to do when the result does not work, and
[Register reference](#registers) the registers of each block.
