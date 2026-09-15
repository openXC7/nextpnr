# Sonata (Sonata ONE) constraints, trimmed to what this blinky uses.
# Written in the set_property LOC dialect accepted by nextpnr-xilinx and
# the himbaechel port.

set_property LOC P15 [get_ports mainClk]
set_property IOSTANDARD LVCMOS33 [get_ports {mainClk}]
create_clock -name mainClk -period 40.0 [get_ports mainClk]

set_property LOC B13 [get_ports {usrLed[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[0]}]
set_property LOC B14 [get_ports {usrLed[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[1]}]
set_property LOC C12 [get_ports {usrLed[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[2]}]
set_property LOC B12 [get_ports {usrLed[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[3]}]
set_property LOC B11 [get_ports {usrLed[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[4]}]
set_property LOC A11 [get_ports {usrLed[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[5]}]
set_property LOC F13 [get_ports {usrLed[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[6]}]
set_property LOC F14 [get_ports {usrLed[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usrLed[7]}]
