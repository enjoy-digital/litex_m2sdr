[> Intro
--------

These set of flowchart are demonstrations and tests to use LiteX-M2SDR with GNURadio through
SoapySDR plugin/driver.

[> FM Radio receiver
--------------------

`test_fm_rx.grc`: demonstrate FM demodulation at 8MS/s with a freq/waterfall/time sink (*Qt GUI Sink* 
block) and a audio sink (*Audio Sink* block)

![test_fm_rx_fig](figs/test_fm_rx.png)

]> Test RX TX (external loopback)
---------------------------------

`test_rx.grc`: is a loopback test. With only one flowchart it's possible to test/check both TX and RX.

 to display both channels samples received and store them into a file

![test_tx_rx_fig](figs/test_tx_rx.png)

]> Test RX
----------

`test_tx_rx.grc`: is a two channels receiver only (RX1 and RX2).

![test_rx_fig](figs/test_rx.png)

]> Test TX
----------

`test_tx.grc`: is a two channels emitter only (TX1 and TX2).

![test_tx_fig](figs/test_tx.png)
