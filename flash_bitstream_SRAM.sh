# to flash the .bin  
openFPGALoader -c ch347_jtag --freq 10e6 --fpga-part xc7a200t --write-flash ./build/litex_m2sdr_m2_pcie_x1/gateware/litex_m2sdr_m2_pcie_x1_operational.bin
#openFPGALoader -c ch347_jtag ./build/litex_m2sdr_m2_pcie_x1/gateware/litex_m2sdr_m2_pcie_x1.bit