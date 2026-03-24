# LiteX M2SDR Linux User-Space Utilities

> [!Note]
> This README describes the user-space tools provided by the **LiteX-M2SDR** project and shows examples of how to use them in typical TX/RX scenarios. The utilities rely on several libraries:

- **ad9361/**: Code adapted from the Analog Devices AD9361 driver, handling low-level RFIC configuration/registers.
- **liblitepcie/**: Part of [LitePCIe](https://github.com/enjoy-digital/litepcie), managing DMA operations and PCIe driver communication.
- **libm2sdr/**: Board-specific support for the M2SDR, integrating device configuration (SI5351, AD9361, etc.) and exposing higher-level APIs to these utilities.

The public `libm2sdr` C API is documented in `../../doc/libm2sdr/README.md` and is now the common device/streaming layer used by the user tools and the SoapySDR module.

---

## Overview of utilities

The user-space code is intentionally layered:

- `libm2sdr/` is the public device/RF/streaming API for host applications.
- `m2sdr_util`, `m2sdr_rf`, `m2sdr_play`, `m2sdr_record`, `m2sdr_check`, and `m2sdr_gpio` are thin application wrappers built on top of `libm2sdr` or the shared host-side support code.
- `liblitepcie/` and `libliteeth/` remain transport helpers used under `libm2sdr` and by a few lower-level tools.
- The SoapySDR module also uses `libm2sdr`, so feature additions in the library tend to propagate to both the CLI tools and Soapy path.

If you want to understand or extend the host stack, start with:

- `libm2sdr/m2sdr.h` for the public API surface.
- `libm2sdr/m2sdr_device.c` for open/close, capability, and register access.
- `libm2sdr/m2sdr_stream.c` for sync streaming and metadata/header handling.
- `libm2sdr/m2sdr_rf.c` for RF configuration helpers built around the AD9361 code.

For application development, prefer starting from `../../doc/libm2sdr/example_sync_rx.c`, `../../doc/libm2sdr/example_sync_tx.c`, or the higher-level wrappers below rather than adding new direct CSR access.

## CLI conventions

The user utilities now follow a mostly shared CLI vocabulary:

- `--device <dev-id>` selects an explicit device such as `pcie:/dev/m2sdr0` or `eth:192.168.1.50:1234`.
- `-c` / `--device-num` remains the short PCIe shorthand for `/dev/m2sdrN`.
- `--ip` and `--port` remain available on Etherbone-capable tools.
- Hyphenated long options are preferred: use `--sample-rate`, `--rx-freq`, `--tx-freq`, `--rx-gain`, `--tx-att`, `--refclk-freq`, etc.
- TX control uses positive attenuation values across the user tools and SoapySDR path: use `--tx-att` natively and `ATT` in SoapySDR.
- Streaming-oriented tools that support both SC16 and SC8 expose `--format sc16|sc8`. Older `--8bit` / `-8` forms are kept as compatibility aliases where applicable.
- Some tools still accept `-z` / `--zero-copy` for compatibility, but the current sync API hides transport-specific zero-copy behavior.

## Build dependencies

Core user tools build with standard C/C++ toolchains and the RF/audio dependencies already used by this project.

`m2sdr_scan` and `m2sdr_check` additionally require:
- `pkg-config`
- SDL2 development headers/libraries (`libsdl2-dev`)
- OpenGL development headers/libraries (`libgl1-mesa-dev`)
- a populated `software/user/cimgui/` directory containing the pinned `cimgui` and Dear ImGui sources

On Debian/Ubuntu:
~~~~
sudo apt install pkg-config libsdl2-dev libgl1-mesa-dev
~~~~

Populate the pinned `cimgui` checkout with:
~~~~
cd ../
./fetch_cimgui.py
~~~~

Or let the top-level software build do it for you:
~~~~
cd ../
./build.py --fetch-cimgui
~~~~

These dependencies are only needed for the optional GUI tools. When SDL2 is not available, or when `software/user/cimgui/` is missing, `m2sdr_scan` and `m2sdr_check` are skipped by the `Makefile`; the CLI tools, `libm2sdr`, and the SoapySDR module still build.

### m2sdr_util
General-purpose utility that provides board information, basic tests, and SPI flash operations.

**Usage**:
~~~~
m2sdr_util [options] cmd [args...]
~~~~

**Commands** include:
- **info** / **show-info**
  Get board information (FPGA version, gateware build, etc.).
- **reg-read** / **reg-write**
  Read/write FPGA registers.
- **dma-test**
  Test DMA transfers between host and FPGA.
- **scratch-test**
  Check scratch register for basic read/write.
- **clk-test**
  Measure on-board clock frequencies.
- **led-status**, **led-control**, **led-pulse**, **led-release**
  Inspect or override the user LED CSR block from the host side.
  `led-control` and `led-pulse` take raw bitmasks from `software/kernel/csr.h`.
- **vcxo-test**
  Test/characterize the VCXO output frequency.
- **si5351-init**, **si5351-dump**, **si5351-read**, **si5351-write**
  Initialize, dump, read, or write the SI5351 clock generator.
- **ad9361-dump**, **ad9361-read**, **ad9361-write**
  Dump AD9361 register space for debugging.
- **flash-write** / **flash-read**
  Write to/read from the on-board SPI Flash.
- **flash-reload**
  Reload the FPGA image from SPI Flash.

Example usage:
~~~~
./m2sdr_util info
./m2sdr_util dma-test
./m2sdr_util scratch-test
./m2sdr_util led-status
./m2sdr_util led-control 0x407
./m2sdr_util led-pulse 0x4
./m2sdr_util flash-read backup.bin 0x100000
~~~~

---

### m2sdr_rf
Tool to initialize or configure the AD936x-based RF front-end.

**Usage**:
~~~~
m2sdr_rf [options] cmd [args...]
~~~~

**Relevant options**:
- `--sync mode` (`internal` or `external`, default=`internal`)
- `--refclk-freq hz` AD9361 reference clock generated by SI5351 (typically `38400000` or `40000000`)
  With `--sync external`, the SI5351 input is an external `10MHz` CLKIN (UFL), but `--refclk-freq` remains the generated AD9361 RefClk.
- `--sample-rate sps` (default=`30720000`)
- `--bandwidth bw` (default=`56000000`)
- `--tx-freq freq` (default=`2400000000`)
- `--rx-freq freq` (default=`2400000000`)
- `--tx-att att` (default=`20` dB, preferred positive TX attenuation)
- `--rx-gain gain` (default=`0` dB)
- `--loopback enable` (enables internal loopback path)
- `--bist-tx-tone`, `--bist-rx-tone`, `--bist-prbs` (built-in self-tests)
- `--format sc16|sc8`

Example usage:
~~~~
./m2sdr_rf --sample-rate=30720000 \\
           --bandwidth=20000000 \\
           --tx-freq=2400000000 \\
           --rx-freq=2400000000 \\
           --tx-att=10 \\
           --rx-gain=10
~~~~

External 10MHz synchronization example:
~~~~
./m2sdr_rf --sync=external --refclk-freq=38400000
~~~~

---

### m2sdr_gen
Generates and streams a tone, white noise, PRBS, or basic OFDM signal directly to the FPGA’s TX path in real-time (DMA TX).
Also supports GPIO/PPS (pulse-per-second) toggling on a selected GPIO pin.

**Usage**:
~~~~
m2sdr_gen [options]
~~~~

**Relevant options**:
- `-c device_num` or `--device <dev-id>`
- `--sample-rate`
- `--signal`
- `--tone-freq`
- `--ofdm-fft-size`
- `--ofdm-cp-len`
- `--ofdm-carriers`
- `--ofdm-modulation`
- `--ofdm-seed`
- `--amplitude`
- `--zero-copy`
- `--pps-freq`
- `--gpio-pin`
- `--format sc16|sc8`
- `--enable-header`

Example usage without PPS:
~~~~
./m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1e6 --amplitude 0.5
./m2sdr_gen --sample-rate 30720000 --signal white --amplitude 0.5
./m2sdr_gen --sample-rate 30720000 --signal prbs --amplitude 0.5
./m2sdr_gen --sample-rate 30720000 --signal ofdm --ofdm-fft-size 1024 --ofdm-cp-len 128 --ofdm-carriers 800 --ofdm-modulation qpsk --amplitude 0.5
~~~~

OFDM mode currently generates repeated random OFDM symbols in real time with configurable FFT size, cyclic prefix length, occupied carrier count, modulation (`qpsk` or `16qam`), and PRBS seed. It is intended as a simple waveform generator rather than a complete framed modem/PHY.

Example usage with PPS on GPIO pin 0:
~~~~
./m2sdr_gen --device pcie:/dev/m2sdr0 --sample-rate 30720000 --signal tone --tone-freq 1e6 --amplitude 0.5 --pps-freq 1.0 --gpio-pin 0
~~~~

---

### m2sdr_play
Streams an I/Q samples file to the FPGA’s TX path (DMA TX). Supports piping from stdin with filename as `-`.
This is the simplest utility to read when you want a file-backed `libm2sdr` TX example.

**Usage**:
~~~~
m2sdr_play [options] filename [loops]
~~~~
- **filename**: Binary file of I/Q samples (or `-` for stdin).
- **loops**: Number of times to loop playback (default=1).

Common playback options:
- `--format sc16|sc8`
- `--timed-start`

SigMF input:
- `--capture-index`

`filename` can be a raw sample file, a `.sigmf-data` file, a `.sigmf-meta` file, or a SigMF basename when adjacent SigMF files exist.
For SigMF inputs, `core:dataset` is honored and resolved relative to the metadata file when needed.
M2SDR-specific SigMF datasets with `core:header_bytes=16` are also accepted and replayed with timestamps restored into TX metadata.

Example usage:
~~~~
./m2sdr_play --format sc16 tx_file.bin 10
./m2sdr_play capture.sigmf-meta 10
./m2sdr_play --capture-index 1 capture.sigmf-meta 1
./m2sdr_play --capture-index 2 framed_capture.sigmf-meta 0
~~~~

---

### m2sdr_record
Streams samples from the FPGA’s RX path back to a file on the host (DMA RX). Supports piping to stdout with filename as `-`.
This is the matching `libm2sdr` RX example, including optional timestamp/header handling.

**Usage**:
~~~~
m2sdr_record [options] filename size
~~~~
- **filename**: Destination file for captured I/Q samples (or `-` for stdout).
- **size**: Number of samples to capture.

Common capture options:
- `--format sc16|sc8`
- `--enable-header`
- `--strip-header`

SigMF output:
- `--sigmf`
- `--sample-rate`
- `--center-freq`
- `--nchannels`
- `--description`
- `--author`
- `--hw`

SigMF annotations:
- `--annotation-label`
- `--annotation-comment`
- `--annotation-start`
- `--annotation-count`
- `--annotation-freq-low`
- `--annotation-freq-high`
- `--annotation-add`
- `--annotate-ts-jumps`
- `--ts-jump-threshold-pct`

Example usage:
~~~~
./m2sdr_record --enable-header --strip-header rx_file.bin 2000000
./m2sdr_record --sigmf --sample-rate 30720000 --center-freq 2400000000 --enable-header --strip-header capture 2000000
./m2sdr_record --sigmf --sample-rate 30720000 --annotation-label burst --annotation-comment "loopback capture" capture 2000000
./m2sdr_record --sigmf --sample-rate 30720000 --annotation-label burst-a --annotation-start 0 --annotation-count 8192 --annotation-add --annotation-label burst-b --annotation-start 16384 --annotation-count 8192 capture 2000000
./m2sdr_record --sigmf --enable-header --strip-header --annotate-ts-jumps capture 2000000
~~~~

Current SigMF support is intentionally minimal: it writes a `.sigmf-data` + `.sigmf-meta` pair and expects stripped sample payloads when DMA headers are enabled.

---

### m2sdr_check
Native GUI utility to inspect recorded I/Q sample files.

**Usage**:
~~~~
m2sdr_check [options] filename
~~~~

`filename` can be a raw sample file, a `.sigmf-data` file, a `.sigmf-meta` file, or a SigMF basename when adjacent SigMF files exist.
For SigMF inputs, `core:dataset` is honored and resolved relative to the metadata file when needed.
M2SDR-specific SigMF datasets with `core:header_bytes=16` are also accepted for inspection.

Inspection options:
- `--nchannels`
- `--nbits`
- `--sample-rate`
- `--format sc16|sc8`
- `--frame-header`
- `--frame-size`
- `--max-samples`

SigMF input:
- `--capture-index`

The GUI provides:
- time-domain I/Q and magnitude plots
- constellation view
- FFT spectrum view
- I/Q histograms
- DC offset / RMS / clipping / timestamp summary
- SigMF capture layout and capture details panels
- SigMF capture-boundary markers in time view
- SigMF capture-center markers in spectrum view

Example usage:
~~~~
./m2sdr_check rx_file.bin --nchannels 2 --nbits 12 --sample-rate 30720000
./m2sdr_check rx_file.bin --nchannels 2 --nbits 12 --sample-rate 30720000 --frame-header --frame-size 245760
./m2sdr_check capture.sigmf-meta
./m2sdr_check --capture-index 1 capture.sigmf-meta
~~~~

---

### m2sdr_sigmf_info
Small text utility to inspect SigMF metadata without opening the GUI.

**Usage**:
~~~~
m2sdr_sigmf_info [--validate] [--strict] [--ci] <sigmf-meta|sigmf-data|basename>
~~~~

Example usage:
~~~~
./m2sdr_sigmf_info capture.sigmf-meta
./m2sdr_sigmf_info --validate capture.sigmf-meta
./m2sdr_sigmf_info --validate --strict --ci capture.sigmf-meta
./m2sdr_sigmf_info --validate --strict framed_capture.sigmf-meta
~~~~

---

### m2sdr_sata
Controls SATA streamers and crossbar routing to record/play I/Q directly to/from SSD, and supports replay through the TX/RX loopback.

**Usage**:
~~~~
m2sdr_sata [options] cmd [args...]
~~~~

**Commands** include:
- **status**
  Show crossbar + SATA/loopback/streamer status.
- **route `TXSRC RXDST [LOOPBACK]`**
  Set routing with optional loopback (0/1).
- **record `DST_SECTOR NSECTORS`**
  RX stream → SSD (SATA_RX_STREAMER).
- **play `SRC_SECTOR NSECTORS`**
  SSD → TX stream (SATA_TX_STREAMER).
- **replay `SRC_SECTOR NSECTORS DST`**
  SSD → TX → loopback → RX destination (`pcie|eth|sata`).
- **copy `SRC_SECTOR DST_SECTOR NSECTORS`**
  SSD → SSD using loopback.
- **header `TX|RX|BOTH ENABLE HEADER_ENABLE`**
  Raw header control (writes HEADER CSR enable bits).

Example usage:
~~~~
./m2sdr_sata status
./m2sdr_sata route pcie sata 0
./m2sdr_sata record 0x1000 8192
./m2sdr_sata play 0x1000 8192
./m2sdr_sata replay 0x1000 8192 pcie
./m2sdr_sata copy 0x2000 0x4000 4096
~~~~

---

### m2sdr_gpio
Provides GPIO control for the M2SDR board, including configuration, loopback mode, and CSR/DMA mode selection. This tool allows for setting output data, output enable bits, and reading back the 4-bit GPIO input.

**Usage**:
~~~~
m2sdr_gpio [options]
~~~~

**Options**:
- `--device <dev-id>` or `--device-num N`
- `--enable`
- `--loopback`
- `--source dma|csr`
- `--output-data`
- `--output-enable`

Example usage:
~~~~
./m2sdr_gpio --device-num 0 --enable --source csr --output-data 0xA --output-enable 0xA
~~~~

---

### m2sdr_fm_tx
FM modulates a WAV audio file or piped raw PCM into interleaved 16-bit I/Q samples for transmission.

**Usage**:
~~~~
m2sdr_fm_tx [options] input output
~~~~
- **input**: WAV file or `-` for stdin (use with ffmpeg for MP3/raw PCM).
- **output**: I/Q bin file or `-` for stdout.

**Options**:
- `-s samplerate` (default=1000000)
- `-d deviation` (default=75000)
- `-b bits` (default=12)
- `-e emphasis` (us/eu/none, default=eu)
- `-m mode` (mono/stereo, default=mono)
- `-i input-channels` (for stdin)
- `-f input-samplerate` (for stdin)

Example:
~~~~
ffmpeg -i music.mp3 -f s16le -ac 2 -ar 44100 - | ./m2sdr_fm_tx -s 1000000 -d 75000 -b 12 -e eu -m stereo -i 2 -f 44100 - - | ./m2sdr_play -
~~~~

---

### m2sdr_fm_rx
FM demodulates interleaved 16-bit I/Q samples into WAV audio or piped raw PCM.

**Usage**:
~~~~
m2sdr_fm_rx [options] input output
~~~~
- **input**: I/Q bin file or `-` for stdin.
- **output**: WAV file or `-` for stdout (pipe to ffplay/ffmpeg for playback).

**Options**:
- `-s samplerate` (default=1000000)
- `-d deviation` (default=75000)
- `-b bits` (default=12)
- `-e emphasis` (us/eu/none, default=eu)
- `-m mode` (mono/stereo, default=mono)

Example:
~~~~
./m2sdr_record - | ./m2sdr_fm_rx -s 1000000 -d 75000 -b 12 -e eu -m stereo - - | ffmpeg -f s16le -ac 2 -ar 44100 -i - -f alsa default
~~~~

---

### m2sdr_scan
Interactive wideband RF scanner with real-time spectrum + waterfall display (Dear ImGui UI).

**Usage**:
~~~~
m2sdr_scan [options]
~~~~

**Key options**:
- `--device <dev-id>` or `--device-num N`
- `--refclk-freq hz`
- `--start-freq hz`
- `--stop-freq hz`
- `--sample-rate hz`
- `--fft-len n`
- `--lines n`
- `--display N`
- `--rx-gain db`
- `--preset-load file`
- `--preset-save file`
- `--no-ui`
- `--export-csv file`
- `--export-png file`

Example:
~~~~
./m2sdr_scan --start-freq 88000000 --stop-freq 108000000 --sample-rate 15360000 --fft-len 16384
~~~~

Headless export example:
~~~~
./m2sdr_scan --preset-load fm_band.scan \\
             --no-ui \\
             --export-csv fm_band.csv \\
             --export-png fm_band.png
~~~~

Notes:
- Runtime controls (range, sample rate, FFT, overlap, gain, settle, palettes, peak tools) are available directly in the UI.
- `F8` moves the window to the next monitor and keeps maximized mode; `Shift+F8` expands it across all monitors.
- `F11` toggles a borderless fullscreen span across all monitors.
- `--preset-save` writes a simple text preset file containing the current scan settings.
- In headless/non-GUI build environments without SDL2, this binary is not built.
- A populated `software/user/cimgui/` checkout is also required for the build. Use `../fetch_cimgui.py` or `../build.py --fetch-cimgui`.

---

## Example End-to-End Workflow

Below is a quick guide to **generate** a tone, **initialize** the RF, **record** samples back, and **analyze** the captured data.

1. **Generate the tone**
   ~~~~
   ./m2sdr_gen --sample-rate 30720000 --signal tone --tone-freq 1000000 --amplitude 0.8
   ~~~~

2. **Initialize the RF**
   ~~~~
   ./m2sdr_rf --sample-rate=30720000 \\
              --bandwidth=40000000 \\
              --tx-freq=2400000000 \\
              --rx-freq=2400000000 \\
              --tx-att=10 \\
              --rx-gain=10 \\
              --loopback=1
   ~~~~
   *(Here `--loopback=1` enables internal loopback for testing.)*

3. **Record the RX**
   ~~~~
   ./m2sdr_record rx_file.bin 20000
   ~~~~
   *(Starts capturing 20K samples while `m2sdr_gen` is active.)*

4. **Analyze the captured data**
   ~~~~
   ./m2sdr_check rx_file.bin --nchannels 2 --nbits 12 --sample-rate 30720000
   ~~~~
   If loopback is active (or you have an over-the-air setup), you’ll see the received waveform in the time-domain, constellation, and spectrum views.

---

## Additional Notes

- **Zero-Copy DMA Mode**
  Some tools still accept a `-z` flag for CLI compatibility, but the `libm2sdr` sync API currently hides transport-specific zero-copy details. Treat it as a compatibility knob unless the utility documentation says otherwise.
- **Device Selection**
  If you have multiple M2SDRs, use `--device pcie:/dev/m2sdrN` or the shorter `--device-num N` form on PCIe tools.
- **API-first development**
  New host applications should generally use `libm2sdr` directly and only drop to raw CSR helpers for bring-up or diagnostics that are not covered by the public API yet.

Happy hacking and enjoy your LiteX M2SDR board! 🤗
