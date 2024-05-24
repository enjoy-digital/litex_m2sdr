                         __   _ __      _  __    __  ______  _______  ___
                        / /  (_) /____ | |/_/___/  |/  /_  |/ __/ _ \/ _ \
                       / /__/ / __/ -_)>  </___/ /|_/ / __/_\ \/ // / , _/
                      /____/_/\__/\__/_/|_|   /_/  /_/____/___/____/_/|_|
                             LiteX based M2 SDR FPGA board
                          Copyright (c) 2024 Enjoy-Digital.
[> Intro
--------

Say hello to the LiteX based M2 SDR FPGA board!

Perfect for SDR enthusiasts, this versatile board fits directly in an M2 slot or can team up with
others in a PCIe M2 carrier for more complex SDR projects.

Mount it on the LiteX Acorn Mini Baseboard for Ethernet support with 1000BaseX/2500BaseX and SATA
connectivity to directly record/play samples to/from an SSD!

Dive in and start enjoying your SDR projects like never before!

[> Ethernet Tests
-----------------

Board mounted in Acorn Mini Baseboard:

```bash
./litex_m2sdr.py --with-ethernet --ethernet-sfp=0 --build --load
./litex_m2sdr.py --with-ethernet --ethernet-sfp=0 --build --load
ping 192.168.1.50
```

[> PCIe Tests
-------------

Board mounted in Acorn Mini Baseboard:

```bash
./litex_m2sdr.py --with-pcie --build --load
lspci
```

[> Software Tests
------------------

Kernel
```bash
cd software/kernel
make clean all
sudo ./init.sh
```

User
```bash
cd software/user
make clean all
./m2sdr_util info
./m2sdr_rf init -samplerate=30720000
./tone_gen.py tone_tx.bin
./m2sdr_play tone_tx.bin 100000
```
[> Enable Debug in Kernel
-------------------------

```bash
sudo sh -c "echo 'module litepcie +p' > /sys/kernel/debug/dynamic_debug/control"
```

[> Use JTAGBone
---------------

```bash
litex_server --jtag --jtag-config=openocd_xc7_ft2232.cfg
litex_cli --regs
litescope_cli
./test_clks.py
```


[> Contact
----------
E-mail: florent@enjoy-digital.fr
