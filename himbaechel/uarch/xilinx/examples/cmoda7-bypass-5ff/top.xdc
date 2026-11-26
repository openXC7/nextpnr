# Digilent Cmod A7-35T (xc7a35tcpg236-1)
set_property PACKAGE_PIN L17 [get_ports clk]
set_property IOSTANDARD LVCMOS33 [get_ports clk]
create_clock -period 83.333 -name sys_clk [get_ports clk]
set_property PACKAGE_PIN A17 [get_ports {led[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[0]}]
set_property PACKAGE_PIN C16 [get_ports {led[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[1]}]

# The case: BFF fed over BX, next to a used B5FF, in one slice
set_property BEL SLICE_X4Y120/BFF [get_cells ff_main]
set_property BEL SLICE_X4Y120/B5FF [get_cells ff_five]
set_property BEL SLICE_X4Y120/B5LUT [get_cells lut_five]
