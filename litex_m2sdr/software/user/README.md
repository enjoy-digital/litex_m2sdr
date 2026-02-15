# LiteX M2SDR Linux User-Space Utilities

> [!Note]
> This README describes the user-space tools provided by the **LiteX-M2SDR** project and shows examples of how to use them in typical TX/RX scenarios. The utilities rely on several libraries:

- **ad9361/**: Code adapted from the Analog Devices AD9361 driver, handling low-level RFIC configuration/registers.
- **liblitepcie/**: Part of [LitePCIe](https://github.com/enjoy-digital/litepcie), managing DMA operations and PCIe driver communication.
- **libm2sdr/**: Board-specific support for the M2SDR, integrating device configuration (SI5351, AD9361, etc.) and exposing higher-level APIs to these utilities.

---

## Overview of utilities

### m2sdr_util
General-purpose utility that provides board information, basic tests, and SPI flash operations.

**Usage**:
~~~~
m2sdr_util [options] cmd [args...]
~~~~

**Commands** include:
- **info**
  Get board information (FPGA version, gateware build, etc.).
- **dma_test**
  Test DMA transfers between host and FPGA.
- **scratch_test**
  Check scratch register for basic read/write.
- **clk_test**
  Measure on-board clock frequencies.
- **vcxo_test**
  Test/characterize the VCXO output frequency.
- **si5351_scan** / **si5351_init**
  Detect or initialize the SI5351 clock generator.
- **ad9361_dump**
  Dump AD9361 register space for debugging.
- **flash_write** / **flash_read**
  Write to/read from the on-board SPI Flash.
- **flash_reload**
  Reload the FPGA image from SPI Flash.

Example usage:
~~~~
./m2sdr_util info
./m2sdr_util dma_test
./m2sdr_util scratch_test
~~~~

---

### m2sdr_rf
Tool to initialize or configure the AD936x-based RF front-end.

**Usage**:
~~~~
m2sdr_rf [options] cmd [args...]
~~~~

**Relevant options**:
- `-samplerate sps` (default=30720000)
- `-bandwidth bw` (default=56000000)
- `-tx_freq freq` (default=2400000000)
- `-rx_freq freq` (default=2400000000)
- `-tx_gain gain` (default=-20 dB)
- `-rx_gain gain` (default=0 dB)
- `-loopback enable` (enables internal loopback path)
- `-bist_tx_tone`, `-bist_rx_tone`, `-bist_prbs` (built-in self-tests)

Example usage:
~~~~
./m2sdr_rf -samplerate=30720000 \\
           -bandwidth=20000000 \\
           -tx_freq=2400000000 \\
           -rx_freq=2400000000 \\
           -tx_gain=-10 \\
           -rx_gain=10
~~~~

---

### m2sdr_gen
Generates and streams a tone, white noise, or PRBS signal directly to the FPGA’s TX path in real-time (DMA TX).
Also supports GPIO/PPS (pulse-per-second) toggling on a selected GPIO pin.

**Usage**:
~~~~
m2sdr_gen [options]
~~~~

**Relevant options**:
- `-c device_num`
  Selects the device (default=0).
- `-s sample_rate`
  Set sample rate in Hz (default = 30720000).
- `-t signal_type`
  Set signal type: 'tone' (default), 'white', or 'prbs'.
- `-f frequency`
  Set tone frequency in Hz (default = 1000).
- `-a amplitude`
  Set amplitude (0.0 to 1.0, default = 1.0).
- `-z`
  Enable zero-copy DMA mode.
- `-p [pps_freq]`
  Enable PPS/toggle on GPIO. When specified, it toggles the configured GPIO at the given frequency (default = 1 Hz if no frequency is provided). The pulse is 20% high and 80% low.
- `-g gpio_pin`
  Select GPIO pin for PPS (range 0-3, default = 0).

Example usage without PPS:
~~~~
./m2sdr_gen -s 30720000 -t tone -f 1e6 -a 0.5
./m2sdr_gen -s 30720000 -t white -a 0.5
./m2sdr_gen -s 30720000 -t prbs -a 0.5
~~~~

Example usage with PPS on GPIO pin 0:
~~~~
./m2sdr_gen -c 0 -s 30720000 -t tone -f 1e6 -a 0.5 -p 1.0 -g 0
~~~~

---

### m2sdr_play
Streams an I/Q samples file to the FPGA’s TX path (DMA TX). Supports piping from stdin with filename as `-`.

**Usage**:
~~~~
m2sdr_play [options] filename [loops]
~~~~
- **filename**: Binary file of I/Q samples (or `-` for stdin).
- **loops**: Number of times to loop playback (default=1).

Example usage:
~~~~
./m2sdr_play tx_file.bin 10
~~~~

---

### m2sdr_record
Streams samples from the FPGA’s RX path back to a file on the host (DMA RX). Supports piping to stdout with filename as `-`.

**Usage**:
~~~~
m2sdr_record [options] filename size
~~~~
- **filename**: Destination file for captured I/Q samples (or `-` for stdout).
- **size**: Number of samples to capture.

Example usage:
~~~~
./m2sdr_record rx_file.bin 2000000
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
- **route `<txsrc> <rxdst> [loopback]`**
  Set routing with optional loopback (0/1).
- **record `<dst_sector> <nsectors>`**
  RX stream → SSD (SATA_RX_STREAMER).
- **play `<src_sector> <nsectors>`**
  SSD → TX stream (SATA_TX_STREAMER).
- **replay `<src_sector> <nsectors> <dst>`**
  SSD → TX → loopback → RX destination (`pcie|eth|sata`).
- **copy `<src_sector> <dst_sector> <nsectors>`**
  SSD → SSD using loopback.
- **header `<tx|rx|both> <enable> <header_enable>`**
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

### tone_gen.py
Python script that generates a pure-tone (sine wave) sample file.

**Key arguments**:
- `--frequency FREQUENCY` (default=1e6)
- `--amplitude AMPLITUDE` (0..1)
- `--samplerate SAMPLERATE` (default=30720000.0)
- `--nchannels`, `--nbits`
- `--nsamples`
*(for optional LiteX framing)*

Example usage:
~~~~
./tone_gen.py --frequency 1e6 \\
              --amplitude 0.8 \\
              --nchannels 2 \\
              --nbits 12 \\
              --nsamples 30720 \\
              tx_file.bin
~~~~

---

### tone_check.py
Python script that analyzes a file of I/Q samples. Can compute approximate amplitude and plot the time-domain waveform if requested.

**Key arguments**:
- `--plot`
- `--nchannels`, `--nbits`
- `--samplerate`

Example usage:
~~~~
./tone_check.py tx_file.bin \\
                --nchannels=2 \\
                --nbits=12 \\
                --samplerate=30720000 \\
                --plot
~~~~

---

### m2sdr_gpio
Provides GPIO control for the M2SDR board, including configuration, loopback mode, and CSR/DMA mode selection. This tool allows for setting output data, output enable bits, and reading back the 4-bit GPIO input.

**Usage**:
~~~~
m2sdr_gpio [options]
~~~~

**Options**:
- `-h`
  Display this help message.
- `-c device_num`
  Select the device (default = 0).
- `-g`
  Enable GPIO control.
- `-l`
  Enable GPIO loopback mode (requires `-g`).
- `-s`
  Use CSR mode (instead of the default DMA mode, requires `-g`).
- `-o output_data`
  Set GPIO output data (4-bit hexadecimal, e.g., `0xF`; requires `-s`).
- `-e output_enable`
  Set GPIO output enable (4-bit hexadecimal, e.g., `0xF`; requires `-s`).

Example usage:
~~~~
./m2sdr_gpio -c 0 -g -s -o 0xA -e 0xA
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

## Example End-to-End Workflow

Below is a quick guide to **generate** a tone, **initialize** the RF, **play** the samples, **record** them back, and **analyze** the captured data.

1. **Generate the tone**
   ~~~~
   ./tone_gen.py --frequency 1000000 \\
                 --amplitude 0.8 \\
                 --nchannels=2 \\
                 --nbits=12 \\
                 --nsamples=30720 \\
                 tx_file.bin
   ~~~~

2. **Initialize the RF**
   ~~~~
   ./m2sdr_rf -samplerate=30720000 \\
              -bandwidth=40000000 \\
              -tx_freq=2400000000 \\
              -rx_freq=2400000000 \\
              -tx_gain=-10 \\
              -rx_gain=10 \\
              -loopback=1
   ~~~~
   *(Here `-loopback=1` enables internal loopback for testing.)*

3. **Play the tone (in Terminal #1)**
   ~~~~
   ./m2sdr_gen tx_file.bin 1000
   ~~~~
   *(Will send the file 1000 times.)*

4. **Record the RX (in Terminal #2)**
   ~~~~
   ./m2sdr_record rx_file.bin 20000
   ~~~~
   *(Starts capturing 20K samples while TX is active.)*

5. **Analyze the captured data**
   ~~~~
   ./tone_check.py rx_file.bin \\
                   --nchannels=2 \\
                   --nbits=12 \\
                   --samplerate=30720000 \\
                   --plot
   ~~~~
   If loopback is active (or you have an over-the-air setup), you’ll see a sine wave in the time-domain plot/

---

## Additional Notes

- **Zero-Copy DMA Mode**
  All tools accept a `-z` option for lower CPU overhead (if your system supports it).
- **Device Selection**
  If you have multiple M2SDRs, use `-c device_num` to select which board (default=0).

Happy hacking and enjoy your LiteX M2SDR board! 🤗
