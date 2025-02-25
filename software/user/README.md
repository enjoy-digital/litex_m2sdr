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
```
m2sdr_util [options] cmd [args...]
```

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
```
m2sdr_rf [options] cmd [args...]
```

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
./m2sdr_rf -samplerate=30720000 \
           -bandwidth=20000000 \
           -tx_freq=2400000000 \
           -rx_freq=2400000000 \
           -tx_gain=-10 \
           -rx_gain=10
~~~~

---

### m2sdr_tone
Generates and streams a pure-tone (sine wave) directly to the FPGA’s TX path in real-time (DMA TX).

**Usage**:
```
m2sdr_tone [options]
```

**Relevant options**:

    -s sample_rate (default=30720000)
    -f frequency (default=1000)
    -a amplitude (0.0 to 1.0, default=1.0)

Example usage:
~~~~
./m2sdr_tone -s 30720000 -f 1e6 -a 0.5
~~~~

---

### m2sdr_play
Streams an I/Q samples file to the FPGA’s TX path (DMA TX).

**Usage**:
```
m2sdr_play [options] filename [loops]
```
- **filename**: Binary file of I/Q samples.
- **loops**: Number of times to loop playback (default=1).

Example usage:
~~~~
./m2sdr_play tx_file.bin 10
~~~~

---

### m2sdr_record
Streams samples from the FPGA’s RX path back to a file on the host (DMA RX).

**Usage**:
```
m2sdr_record [options] filename size
```
- **filename**: Destination file for captured I/Q samples.
- **size**: Number of samples to capture.

Example usage:
~~~~
./m2sdr_record rx_file.bin 2000000
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
./tone_gen.py --frequency 1e6 \
              --amplitude 0.8 \
              --nchannels 2 \
              --nbits 12 \
              --nsamples 30720 \
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
./tone_check.py tx_file.bin \
                --nchannels=2 \
                --nbits=12 \
                --samplerate=30720000 \
                --plot
~~~~

---

## Example End-to-End Workflow

Below is a quick guide to **generate** a tone, **initialize** the RF, **play** the samples, **record** them back, and **analyze** the captured data.

1. **Generate the tone**
   ~~~~
   ./tone_gen.py --frequency 1000000 \
                 --amplitude 0.8 \
                 --nchannels=2 \
                 --nbits=12 \
                 --nsamples=30720 \
                 tx_file.bin
   ~~~~

2. **Initialize the RF**
   ~~~~
   ./m2sdr_rf -samplerate=30720000 \
              -bandwidth=40000000 \
              -tx_freq=2400000000 \
              -rx_freq=2400000000 \
              -tx_gain=-10 \
              -rx_gain=10 \
              -loopback=1
   ~~~~
   *(Here `-loopback=1` enables internal loopback for testing.)*

3. **Play the tone (in Terminal #1)**
   ~~~~
   ./m2sdr_play tx_file.bin 1000
   ~~~~
   *(Will send the file 1000 times.)*

4. **Record the RX (in Terminal #2)**
   ~~~~
   ./m2sdr_record rx_file.bin 20000
   ~~~~
   *(Starts capturing 20K samples while TX is active.)*

5. **Analyze the captured data**
   ~~~~
   ./tone_check.py rx_file.bin \
                   --nchannels=2 \
                   --nbits=12 \
                   --samplerate=30720000 \
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
