################################################################################
# IO constraints
################################################################################
# clk100:0
set_property LOC C18 [get_ports {clk100}]
set_property IOSTANDARD LVCMOS33 [get_ports {clk100}]

# si5351_i2c:0.scl
set_property LOC AA20 [get_ports {si5351_i2c_scl}]
set_property PULLUP TRUE [get_ports {si5351_i2c_scl}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_i2c_scl}]

# si5351_i2c:0.sda
set_property LOC AB21 [get_ports {si5351_i2c_sda}]
set_property PULLUP TRUE [get_ports {si5351_i2c_sda}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_i2c_sda}]

# si5351_pwm:0
set_property LOC W19 [get_ports {si5351_pwm}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_pwm}]

# si5351_ssen_clkin:0
set_property LOC W22 [get_ports {si5351_ssen_clkin}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_ssen_clkin}]

# si5351_clk0:0
set_property LOC J19 [get_ports {si5351_clk0}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_clk0}]

# si5351_clk1:0
set_property LOC E19 [get_ports {si5351_clk1}]
set_property IOSTANDARD LVCMOS33 [get_ports {si5351_clk1}]

# user_led:0
set_property LOC AB15 [get_ports {user_led}]
set_property IOSTANDARD LVCMOS33 [get_ports {user_led}]

# flash_cs_n:0
set_property LOC T19 [get_ports {flash_cs_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {flash_cs_n}]

# flash:0.mosi
set_property LOC P22 [get_ports {flash_mosi}]
set_property IOSTANDARD LVCMOS33 [get_ports {flash_mosi}]

# flash:0.miso
set_property LOC R22 [get_ports {flash_miso}]
set_property IOSTANDARD LVCMOS33 [get_ports {flash_miso}]

# flash:0.wp
set_property LOC P21 [get_ports {flash_wp}]
set_property IOSTANDARD LVCMOS33 [get_ports {flash_wp}]

# flash:0.hold
set_property LOC R21 [get_ports {flash_hold}]
set_property IOSTANDARD LVCMOS33 [get_ports {flash_hold}]

# pcie_x1_m2:0.rst_n
set_property LOC A15 [get_ports {pcie_x1_m2_rst_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {pcie_x1_m2_rst_n}]
set_property PULLUP TRUE [get_ports {pcie_x1_m2_rst_n}]

# pcie_x1_m2:0.clk_p
set_property LOC F6 [get_ports {pcie_x1_m2_clk_p}]

# pcie_x1_m2:0.clk_n
set_property LOC E6 [get_ports {pcie_x1_m2_clk_n}]

# pcie_x1_m2:0.rx_p
set_property LOC D9 [get_ports {pcie_x1_m2_rx_p}]

# pcie_x1_m2:0.rx_n
set_property LOC C9 [get_ports {pcie_x1_m2_rx_n}]

# pcie_x1_m2:0.tx_p
set_property LOC D7 [get_ports {pcie_x1_m2_tx_p}]

# pcie_x1_m2:0.tx_n
set_property LOC C7 [get_ports {pcie_x1_m2_tx_n}]

# ad9361_rfic:0.rx_clk_p
set_property LOC V4 [get_ports {ad9361_rfic_rx_clk_p}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_clk_p}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_clk_p}]

# ad9361_rfic:0.rx_clk_n
set_property LOC W4 [get_ports {ad9361_rfic_rx_clk_n}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_clk_n}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_clk_n}]

# ad9361_rfic:0.rx_frame_p
set_property LOC AB7 [get_ports {ad9361_rfic_rx_frame_p}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_frame_p}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_frame_p}]

# ad9361_rfic:0.rx_frame_n
set_property LOC AB6 [get_ports {ad9361_rfic_rx_frame_n}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_frame_n}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_frame_n}]

# ad9361_rfic:0.rx_data_p
set_property LOC U6 [get_ports {ad9361_rfic_rx_data_p[0]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[0]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[0]}]

# ad9361_rfic:0.rx_data_p
set_property LOC W6 [get_ports {ad9361_rfic_rx_data_p[1]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[1]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[1]}]

# ad9361_rfic:0.rx_data_p
set_property LOC Y6 [get_ports {ad9361_rfic_rx_data_p[2]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[2]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[2]}]

# ad9361_rfic:0.rx_data_p
set_property LOC V7 [get_ports {ad9361_rfic_rx_data_p[3]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[3]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[3]}]

# ad9361_rfic:0.rx_data_p
set_property LOC W9 [get_ports {ad9361_rfic_rx_data_p[4]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[4]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[4]}]

# ad9361_rfic:0.rx_data_p
set_property LOC V9 [get_ports {ad9361_rfic_rx_data_p[5]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_p[5]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_p[5]}]

# ad9361_rfic:0.rx_data_n
set_property LOC V5 [get_ports {ad9361_rfic_rx_data_n[0]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[0]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[0]}]

# ad9361_rfic:0.rx_data_n
set_property LOC W5 [get_ports {ad9361_rfic_rx_data_n[1]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[1]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[1]}]

# ad9361_rfic:0.rx_data_n
set_property LOC AA6 [get_ports {ad9361_rfic_rx_data_n[2]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[2]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[2]}]

# ad9361_rfic:0.rx_data_n
set_property LOC W7 [get_ports {ad9361_rfic_rx_data_n[3]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[3]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[3]}]

# ad9361_rfic:0.rx_data_n
set_property LOC Y9 [get_ports {ad9361_rfic_rx_data_n[4]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[4]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[4]}]

# ad9361_rfic:0.rx_data_n
set_property LOC V8 [get_ports {ad9361_rfic_rx_data_n[5]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_rx_data_n[5]}]
set_property DIFF_TERM TRUE [get_ports {ad9361_rfic_rx_data_n[5]}]

# ad9361_rfic:0.tx_clk_p
set_property LOC T5 [get_ports {ad9361_rfic_tx_clk_p}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_clk_p}]

# ad9361_rfic:0.tx_clk_n
set_property LOC U5 [get_ports {ad9361_rfic_tx_clk_n}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_clk_n}]

# ad9361_rfic:0.tx_frame_p
set_property LOC AA8 [get_ports {ad9361_rfic_tx_frame_p}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_frame_p}]

# ad9361_rfic:0.tx_frame_n
set_property LOC AB8 [get_ports {ad9361_rfic_tx_frame_n}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_frame_n}]

# ad9361_rfic:0.tx_data_p
set_property LOC U3 [get_ports {ad9361_rfic_tx_data_p[0]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[0]}]

# ad9361_rfic:0.tx_data_p
set_property LOC Y4 [get_ports {ad9361_rfic_tx_data_p[1]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[1]}]

# ad9361_rfic:0.tx_data_p
set_property LOC AB3 [get_ports {ad9361_rfic_tx_data_p[2]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[2]}]

# ad9361_rfic:0.tx_data_p
set_property LOC AA1 [get_ports {ad9361_rfic_tx_data_p[3]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[3]}]

# ad9361_rfic:0.tx_data_p
set_property LOC W1 [get_ports {ad9361_rfic_tx_data_p[4]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[4]}]

# ad9361_rfic:0.tx_data_p
set_property LOC AA5 [get_ports {ad9361_rfic_tx_data_p[5]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_p[5]}]

# ad9361_rfic:0.tx_data_n
set_property LOC V3 [get_ports {ad9361_rfic_tx_data_n[0]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[0]}]

# ad9361_rfic:0.tx_data_n
set_property LOC AA4 [get_ports {ad9361_rfic_tx_data_n[1]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[1]}]

# ad9361_rfic:0.tx_data_n
set_property LOC AB2 [get_ports {ad9361_rfic_tx_data_n[2]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[2]}]

# ad9361_rfic:0.tx_data_n
set_property LOC AB1 [get_ports {ad9361_rfic_tx_data_n[3]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[3]}]

# ad9361_rfic:0.tx_data_n
set_property LOC Y1 [get_ports {ad9361_rfic_tx_data_n[4]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[4]}]

# ad9361_rfic:0.tx_data_n
set_property LOC AB5 [get_ports {ad9361_rfic_tx_data_n[5]}]
set_property IOSTANDARD LVDS_25 [get_ports {ad9361_rfic_tx_data_n[5]}]

# ad9361_rfic:0.rst_n
set_property LOC E1 [get_ports {ad9361_rfic_rst_n}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_rst_n}]

# ad9361_rfic:0.enable
set_property LOC P4 [get_ports {ad9361_rfic_enable}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_enable}]

# ad9361_rfic:0.txnrx
set_property LOC B2 [get_ports {ad9361_rfic_txnrx}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_txnrx}]

# ad9361_rfic:0.en_agc
set_property LOC N5 [get_ports {ad9361_rfic_en_agc}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_en_agc}]

# ad9361_rfic:0.ctrl
set_property LOC T1 [get_ports {ad9361_rfic_ctrl[0]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_ctrl[0]}]

# ad9361_rfic:0.ctrl
set_property LOC U1 [get_ports {ad9361_rfic_ctrl[1]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_ctrl[1]}]

# ad9361_rfic:0.ctrl
set_property LOC M3 [get_ports {ad9361_rfic_ctrl[2]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_ctrl[2]}]

# ad9361_rfic:0.ctrl
set_property LOC M1 [get_ports {ad9361_rfic_ctrl[3]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_ctrl[3]}]

# ad9361_rfic:0.stat
set_property LOC L1 [get_ports {ad9361_rfic_stat[0]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[0]}]

# ad9361_rfic:0.stat
set_property LOC M2 [get_ports {ad9361_rfic_stat[1]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[1]}]

# ad9361_rfic:0.stat
set_property LOC P1 [get_ports {ad9361_rfic_stat[2]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[2]}]

# ad9361_rfic:0.stat
set_property LOC R2 [get_ports {ad9361_rfic_stat[3]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[3]}]

# ad9361_rfic:0.stat
set_property LOC R3 [get_ports {ad9361_rfic_stat[4]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[4]}]

# ad9361_rfic:0.stat
set_property LOC N3 [get_ports {ad9361_rfic_stat[5]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[5]}]

# ad9361_rfic:0.stat
set_property LOC N2 [get_ports {ad9361_rfic_stat[6]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[6]}]

# ad9361_rfic:0.stat
set_property LOC N4 [get_ports {ad9361_rfic_stat[7]}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_rfic_stat[7]}]

# ad9361_spi:0.clk
set_property LOC P5 [get_ports {ad9361_spi_clk}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_spi_clk}]

# ad9361_spi:0.cs_n
set_property LOC E2 [get_ports {ad9361_spi_cs_n}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_spi_cs_n}]

# ad9361_spi:0.mosi
set_property LOC P6 [get_ports {ad9361_spi_mosi}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_spi_mosi}]

# ad9361_spi:0.miso
set_property LOC M6 [get_ports {ad9361_spi_miso}]
set_property IOSTANDARD LVCMOS25 [get_ports {ad9361_spi_miso}]

################################################################################
# Design constraints
################################################################################

set_property SEVERITY {Warning} [get_drc_checks REQP-49]

set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk10_clk]

################################################################################
# Clock constraints
################################################################################


create_clock -name clk100 -period 10.0 [get_ports clk100]

create_clock -name si5351_clk0 -period 26.041 [get_ports si5351_clk0]

create_clock -name si5351_clk1 -period 10.0 [get_ports si5351_clk1]

create_clock -name jtag_clk -period 50.0 [get_nets jtag_clk]

create_clock -name icap_clk -period 128.0 [get_nets icap_clk]

create_clock -name dna_clk -period 16.0 [get_nets dna_clk]

create_clock -name pcie_x1_m2_clk_p -period 10.0 [get_ports pcie_x1_m2_clk_p]

create_clock -name rfic_clk -period 4.069 [get_nets rfic_clk]

################################################################################
# False path constraints
################################################################################


set_false_path -quiet -through [get_nets -hierarchical -filter {mr_ff == TRUE}]

set_false_path -quiet -to [get_pins -filter {REF_PIN_NAME == PRE} -of_objects [get_cells -hierarchical -filter {ars_ff1 == TRUE || ars_ff2 == TRUE}]]

set_max_delay 2 -quiet -from [get_pins -filter {REF_PIN_NAME == C} -of_objects [get_cells -hierarchical -filter {ars_ff1 == TRUE}]] -to [get_pins -filter {REF_PIN_NAME == D} -of_objects [get_cells -hierarchical -filter {ars_ff2 == TRUE}]]

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets rfic_clk]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets dna_clk]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets jtag_clk]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets icap_clk]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets clk10_clk]] -group [get_clocks -include_generated_clocks -of [get_nets basesoc_crg_clkin]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -group [get_clocks -include_generated_clocks -of [get_nets pcie_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -group [get_clocks -include_generated_clocks -of [get_nets clk10_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -group [get_clocks -include_generated_clocks -of [get_nets basesoc_crg_clkin]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_ports si5351_clk1]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_ports si5351_clk0]] -group [get_clocks -include_generated_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -include_generated_clocks -of [get_ports si5351_clk0]] -group [get_clocks -include_generated_clocks -of [get_ports si5351_clk1]] -asynchronous