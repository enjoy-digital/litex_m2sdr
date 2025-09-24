
# Create Project

set_part xc7a200tsbg484-3
set_msg_config -id {Common 17-55} -new_severity {Warning}

# Add project commands


# Add Sources

read_verilog {/home/sens/litex_m2sdr/build/litex_m2sdr_m2_pcie_x1/gateware/litex_m2sdr_m2_pcie_x1.v}

# Add EDIFs


# Add IPs


# Add constraints

read_xdc litex_m2sdr_m2_pcie_x1.xdc
set_property PROCESSING_ORDER EARLY [get_files litex_m2sdr_m2_pcie_x1.xdc]

# Add pre-synthesis commands

create_ip -vendor xilinx.com -name pcie_7x -module_name pcie_s7
set obj [get_ips pcie_s7]
set_property -dict [list \
CONFIG.Bar0_Scale {Megabytes} \
CONFIG.Bar0_Size {1} \
CONFIG.Buf_Opt_BMA {True} \
CONFIG.Component_Name {pcie} \
CONFIG.Device_ID {7021} \
CONFIG.Interface_Width {64_bit} \
CONFIG.Link_Speed {5.0_GT/s} \
CONFIG.Max_Payload_Size {512_bytes} \
CONFIG.Maximum_Link_Width {X1} \
CONFIG.PCIe_Blk_Locn {X0Y0} \
CONFIG.Ref_Clk_Freq {100_MHz} \
CONFIG.Trans_Buf_Pipeline {None} \
CONFIG.Trgt_Link_Speed {4'h2} \
CONFIG.User_Clk_Freq {125} \
CONFIG.Legacy_Interrupt {None} \
CONFIG.IntX_Generation {False} \
CONFIG.MSI_64b {False} \
CONFIG.Multiple_Message_Capable {1_vector} \
CONFIG.Base_Class_Menu {Wireless_controller} \
CONFIG.Sub_Class_Interface_Menu {RF_controller} \
CONFIG.Class_Code_Base {0D} \
CONFIG.Class_Code_Sub {10} \
CONFIG.mode_selection {Advanced} \
CONFIG.en_ext_gt_common {True} \
] $obj
synth_ip $obj

# Synthesis

synth_design -directive default -top litex_m2sdr_m2_pcie_x1 -part xc7a200tsbg484-3

# Synthesis report

report_timing_summary -file litex_m2sdr_m2_pcie_x1_timing_synth.rpt
report_utilization -hierarchical -file litex_m2sdr_m2_pcie_x1_utilization_hierarchical_synth.rpt
report_utilization -file litex_m2sdr_m2_pcie_x1_utilization_synth.rpt
write_checkpoint -force litex_m2sdr_m2_pcie_x1_synth.dcp

# Add pre-optimize commands


# Optimize design

opt_design -directive default

# Add pre-placement commands

set_false_path -from [get_clocks {*s7pciephy_clkout0}] -to [get_clocks {*crg_*clkout0}]
set_false_path -from [get_clocks {*crg_*clkout0}] -to [get_clocks {*s7pciephy_clkout0}]
set_false_path -from [get_clocks {*s7pciephy_clkout1}] -to [get_clocks {*crg_*clkout0}]
set_false_path -from [get_clocks {*crg_*clkout0}] -to [get_clocks {*s7pciephy_clkout1}]
set_false_path -from [get_clocks {*s7pciephy_clkout3}] -to [get_clocks {*crg_*clkout0}]
set_false_path -from [get_clocks {*crg_*clkout0}] -to [get_clocks {*s7pciephy_clkout3}]
set_false_path -from [get_clocks {*s7pciephy_clkout0}] -to [get_clocks {*s7pciephy_clkout1}]
set_false_path -from [get_clocks {*s7pciephy_clkout1}] -to [get_clocks {*s7pciephy_clkout0}]
reset_property LOC [get_cells -hierarchical -filter {NAME=~pcie_s7/*genblk*.bram36_tdp_bl.bram36_tdp_bl}]

# Placement

place_design -directive default

# Placement report

report_utilization -hierarchical -file litex_m2sdr_m2_pcie_x1_utilization_hierarchical_place.rpt
report_utilization -file litex_m2sdr_m2_pcie_x1_utilization_place.rpt
report_io -file litex_m2sdr_m2_pcie_x1_io.rpt
report_control_sets -verbose -file litex_m2sdr_m2_pcie_x1_control_sets.rpt
report_clock_utilization -file litex_m2sdr_m2_pcie_x1_clock_utilization.rpt
write_checkpoint -force litex_m2sdr_m2_pcie_x1_place.dcp

# Add pre-routing commands


# Routing

route_design -directive default
phys_opt_design -directive default
write_checkpoint -force litex_m2sdr_m2_pcie_x1_route.dcp

# Routing report

report_timing_summary -no_header -no_detailed_paths
report_route_status -file litex_m2sdr_m2_pcie_x1_route_status.rpt
report_drc -file litex_m2sdr_m2_pcie_x1_drc.rpt
report_timing_summary -datasheet -max_paths 10 -file litex_m2sdr_m2_pcie_x1_timing.rpt
report_power -file litex_m2sdr_m2_pcie_x1_power.rpt
set_property BITSTREAM.CONFIG.UNUSEDPIN Pulldown [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 33 [current_design]
set_property BITSTREAM.CONFIG.OVERTEMPPOWERDOWN ENABLE [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

# Bitstream generation

write_bitstream -force litex_m2sdr_m2_pcie_x1.bit 
write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit "up 0x0             litex_m2sdr_m2_pcie_x1.bit" -file litex_m2sdr_m2_pcie_x1.bin
set_property BITSTREAM.CONFIG.TIMER_CFG 0x01000000 [current_design]
set_property BITSTREAM.CONFIG.CONFIGFALLBACK Enable [current_design]
write_bitstream -force litex_m2sdr_m2_pcie_x1_operational.bit 
write_cfgmem -force -format bin -interface spix4 -size 16 -loadbit "up 0x0                 litex_m2sdr_m2_pcie_x1_operational.bit" -file litex_m2sdr_m2_pcie_x1_operational.bin
set_property BITSTREAM.CONFIG.NEXT_CONFIG_ADDR 0x00800000 [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 1 [current_design]
write_bitstream -force litex_m2sdr_m2_pcie_x1_fallback.bit 
write_cfgmem -force -format bin -interface spix1 -size 16 -loadbit "up 0x0                 litex_m2sdr_m2_pcie_x1_fallback.bit" -file litex_m2sdr_m2_pcie_x1_fallback.bin

# End

quit