# LiteX M2SDR GNU Radio Flowcharts

This directory provides a set of GNU Radio flowcharts for demonstration and testing purposes, showing how to use the **LiteX-M2SDR** board with the SoapySDR plugin/driver. These examples leverage the flexibility of the board and its software stack to quickly set up RX, TX, loopback, and FM receiver applications.

---

## Intro

The provided flowcharts illustrate different usage scenarios:
- **Test RX**: A two-channel receiver that displays incoming IQ data and stores it to files.
- **Test TX**: A two-channel transmitter that offers multiple source options.
- **Test RX TX (External Loopback)**: A single-channel loopback test to verify both TX and RX paths.
- **FM Radio Receiver**: An FM demodulation example featuring a waterfall display and audio output.

---

## Test RX

**Flowchart:** `test_tx_rx.grc`

This flowchart implements a two-channel receiver (RX1 and RX2). Each channel's stream is simultaneously displayed and saved to a file (e.g., */tmp/chanA.bin* and */tmp/chanB.bin*). The files are standard binary files with interleaved float complex samples.

Two sliders in the flowchart allow dynamic adjustment of:
- **Center Frequency**
- **Gain**

![test_rx_fig](https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/e8178f7f-de92-4d28-b9ed-230f485925bd)

---

## Test TX

**Flowchart:** `test_tx.grc`

This flowchart is a two-channel transmitter (TX1 and TX2). The source stream is split between a scope and a Soapy Sink block. Users can switch the source type dynamically by enabling or disabling blocks using keyboard shortcuts (for example, 'E' to enable, 'D' to disable):
- **Constant Source**: Emits a steady carrier frequency.
- **Signal Source**: Emits a 1 MHz complex sine wave.
- **File Source**: Plays a custom signal stored in a file (float complex interleaved).

Two sliders let you adjust:
- **Frequency**
- **Gain/Attenuation**

![test_tx_fig](https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/ff7b4a2f-f0db-4c11-b3db-b0c5a6e4bef1)

---

## Test RX TX (External Loopback)

**Flowchart:** `test_rx.grc`

This flowchart provides a loopback test using a single channel. It verifies both transmission and reception:
- **TX Path**: Driven by a constant source (emitting only the carrier).
- **RX Path**: Connected to a Qt GUI Sink, offering time, frequency, and waterfall displays.

Four sliders allow real-time adjustments of:
- **TX Center Frequency** (`freqTx`)
- **TX Gain/Attenuation** (`gainTx`)
- **RX Center Frequency** (`freqRx`)
- **RX Gain** (`gainRx`)

![test_tx_rx_fig](https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/942339b8-3d0b-4aa6-aade-e607bab4035e)

---

## FM Radio Receiver

**Flowchart:** `test_fm_rx.grc`

This flowchart demonstrates FM demodulation at 8 MS/s. It uses a Qt GUI Sink to display frequency, waterfall, and time domains, alongside an Audio Sink configured at 48 kHz for sound output.

Three sliders are provided for on-the-fly adjustment of:
- **Center Frequency** (`freq`)
- **RFIC Gain** (`gain`) – Adjust to optimize signal level or avoid saturation.
- **Audio Volume** (`volume`)

![test_fm_rx_fig](https://github.com/enjoy-digital/litex_m2sdr/assets/1450143/ce074aad-ec68-4110-90c3-633a7b48bc50)

---

Happy experimenting with your **LiteX-M2SDR** board and GNU Radio!
