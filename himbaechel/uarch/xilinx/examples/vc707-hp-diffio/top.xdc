# 200 MHz LVDS system clock, bank 38 (RIOB18_X312Y287)
set_property PACKAGE_PIN E19 [get_ports sysclk_p]
set_property PACKAGE_PIN E18 [get_ports sysclk_n]
set_property IOSTANDARD LVDS [get_ports sysclk_p]
set_property IOSTANDARD LVDS [get_ports sysclk_n]
create_clock -period 5.000 -name sysclk [get_ports sysclk_p]

# CPU_RESET, bank 15 (LIOB18_X81Y128)
set_property PACKAGE_PIN AV40 [get_ports rst]
set_property IOSTANDARD LVCMOS18 [get_ports rst]

# LVDS driver, bank 38 pair IO_L1P/N_T0_38 (RIOB18_X312Y309)
set_property PACKAGE_PIN C19 [get_ports dout_p]
set_property PACKAGE_PIN B19 [get_ports dout_n]
set_property IOSTANDARD LVDS [get_ports dout_p]
set_property IOSTANDARD LVDS [get_ports dout_n]

# Bidirectional differential pad, bank 38 pair IO_L2P/N_T0_38 (RIOB18_X312Y307)
set_property PACKAGE_PIN A16 [get_ports dq_p]
set_property PACKAGE_PIN A15 [get_ports dq_n]
set_property IOSTANDARD DIFF_SSTL15 [get_ports dq_p]
set_property IOSTANDARD DIFF_SSTL15 [get_ports dq_n]

# LEDs, bank 15
set_property PACKAGE_PIN AM39 [get_ports {led[0]}]
set_property PACKAGE_PIN AN39 [get_ports {led[1]}]
set_property IOSTANDARD LVCMOS18 [get_ports {led[0]}]
set_property IOSTANDARD LVCMOS18 [get_ports {led[1]}]

# SLEW FAST on the bidirectional pair is the point of this case: the HP-bank
# slew is a single shared bit with two FASM names of opposite polarity, so a
# pad that is both driven FAST and received is where the receiver's
# SLEW.SLOW line collides with the driver's SLEW.FAST.  That collision is
# what fasm2frames rejected on litex-ddr-qmtech-kintex7 (FasmInconsistentBits)
# before the receiver line was guarded on !is_output.  Nothing on xc7v
# reached this shape, and with the default SLOW slew it would not: both
# halves would agree and the file would assemble either way.
set_property SLEW FAST [get_ports dq_p]
set_property SLEW FAST [get_ports dq_n]
