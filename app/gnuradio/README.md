[> Intro
--------

These directory provides a set of *GNU Radio*'s flowcharts for demonstrations and tests purpose
on how to use LiteX-M2SDR with GNURadio through SoapySDR plugin/driver.

[> FM Radio receiver
--------------------

`test_fm_rx.grc`: demonstrate FM demodulation at 8MS/s with a freq/waterfall/time sink (*Qt GUI Sink* 
block) and a audio sink (*Audio Sink* block). Output (audio) is configured at 48kHz.

Three sliders are present to dynamically changes parameters:
- `freq` to adapt the center frequency
- `gain` to adapt RFIC gain when signal is too small or to avoid antenna saturate
- `volume` to configure *Audio Sink* level

![test_fm_rx_fig](figs/test_fm_rx.png)

]> Test RX TX (external loopback)
---------------------------------

`test_rx.grc`: is a one channel loopback test. With only one flowchart it's possible to test/check
both TX and RX.
- TX is filled with a constant source so only the carrier frequency is emitted
- RX stream is connected to a *Qt GUI Sink* to have a direct feedback (time, frequency and waterfall)

Four sliders are present to adapt:
- TX center frequency (*freqTx*)
- TX gain/attenuation (*gainTx*)
- RX center frequency (*freqRx*)
- RX gain (*gainRx*)

![test_tx_rx_fig](figs/test_tx_rx.png)

]> Test RX
----------

`test_tx_rx.grc`: is a two channels receiver only (RX1 and RX2).
Each channel's stream are displayed and at the same time stored in a file (*/tmp/chanA.bin*,
*/tmp/chanB.bin*). File format are classic binary file in float interleave complex.

As for previous demonstrations two sliders are present: one for center frequency and another one
for gain

![test_rx_fig](figs/test_rx.png)

]> Test TX
----------

`test_tx.grc`: is a two channels emitter only (TX1 and TX2).
Stream for the source is splitted between a scope and the Soapy Sink block

By updating the flowchart user could change source type ('E' key to enable a block 'D' key to
disable to a block):
- *Constant Source* is to have a carrier frequency only
- *Signal Source* for a 1MHz complex sin
- *File  Source* to emit a custom signal stored to a file (float complex interleaved)

As for previous demonstrations, two sliders are present to configure frequency and gain/attenuation.

![test_tx_fig](figs/test_tx.png)
