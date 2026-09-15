################################################################################
# IO constraints
################################################################################
# cpu_reset:0
set_property LOC AV40 [get_ports {cpu_reset}]
set_property IOSTANDARD LVCMOS18 [get_ports {cpu_reset}]

# clk200:0.p
set_property LOC E19 [get_ports {clk200_p}]
set_property IOSTANDARD LVDS [get_ports {clk200_p}]

# clk200:0.n
set_property LOC E18 [get_ports {clk200_n}]
set_property IOSTANDARD LVDS [get_ports {clk200_n}]

# serial:0.rx
set_property LOC AU33 [get_ports {serial_rx}]
set_property IOSTANDARD LVCMOS18 [get_ports {serial_rx}]

# serial:0.tx
set_property LOC AU36 [get_ports {serial_tx}]
set_property IOSTANDARD LVCMOS18 [get_ports {serial_tx}]

# ddram:0.a
set_property LOC A20 [get_ports {ddram_a[0]}]
set_property SLEW FAST [get_ports {ddram_a[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[0]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[0]}]

# ddram:0.a
set_property LOC B19 [get_ports {ddram_a[1]}]
set_property SLEW FAST [get_ports {ddram_a[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[1]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[1]}]

# ddram:0.a
set_property LOC C20 [get_ports {ddram_a[2]}]
set_property SLEW FAST [get_ports {ddram_a[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[2]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[2]}]

# ddram:0.a
set_property LOC A19 [get_ports {ddram_a[3]}]
set_property SLEW FAST [get_ports {ddram_a[3]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[3]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[3]}]

# ddram:0.a
set_property LOC A17 [get_ports {ddram_a[4]}]
set_property SLEW FAST [get_ports {ddram_a[4]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[4]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[4]}]

# ddram:0.a
set_property LOC A16 [get_ports {ddram_a[5]}]
set_property SLEW FAST [get_ports {ddram_a[5]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[5]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[5]}]

# ddram:0.a
set_property LOC D20 [get_ports {ddram_a[6]}]
set_property SLEW FAST [get_ports {ddram_a[6]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[6]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[6]}]

# ddram:0.a
set_property LOC C18 [get_ports {ddram_a[7]}]
set_property SLEW FAST [get_ports {ddram_a[7]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[7]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[7]}]

# ddram:0.a
set_property LOC D17 [get_ports {ddram_a[8]}]
set_property SLEW FAST [get_ports {ddram_a[8]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[8]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[8]}]

# ddram:0.a
set_property LOC C19 [get_ports {ddram_a[9]}]
set_property SLEW FAST [get_ports {ddram_a[9]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[9]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[9]}]

# ddram:0.a
set_property LOC B21 [get_ports {ddram_a[10]}]
set_property SLEW FAST [get_ports {ddram_a[10]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[10]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[10]}]

# ddram:0.a
set_property LOC B17 [get_ports {ddram_a[11]}]
set_property SLEW FAST [get_ports {ddram_a[11]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[11]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[11]}]

# ddram:0.a
set_property LOC A15 [get_ports {ddram_a[12]}]
set_property SLEW FAST [get_ports {ddram_a[12]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[12]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[12]}]

# ddram:0.a
set_property LOC A21 [get_ports {ddram_a[13]}]
set_property SLEW FAST [get_ports {ddram_a[13]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[13]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[13]}]

# ddram:0.a
set_property LOC F17 [get_ports {ddram_a[14]}]
set_property SLEW FAST [get_ports {ddram_a[14]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[14]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[14]}]

# ddram:0.a
set_property LOC E17 [get_ports {ddram_a[15]}]
set_property SLEW FAST [get_ports {ddram_a[15]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_a[15]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_a[15]}]

# ddram:0.ba
set_property LOC D21 [get_ports {ddram_ba[0]}]
set_property SLEW FAST [get_ports {ddram_ba[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_ba[0]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_ba[0]}]

# ddram:0.ba
set_property LOC C21 [get_ports {ddram_ba[1]}]
set_property SLEW FAST [get_ports {ddram_ba[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_ba[1]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_ba[1]}]

# ddram:0.ba
set_property LOC D18 [get_ports {ddram_ba[2]}]
set_property SLEW FAST [get_ports {ddram_ba[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_ba[2]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_ba[2]}]

# ddram:0.ras_n
set_property LOC E20 [get_ports {ddram_ras_n}]
set_property SLEW FAST [get_ports {ddram_ras_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_ras_n}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_ras_n}]

# ddram:0.cas_n
set_property LOC K17 [get_ports {ddram_cas_n}]
set_property SLEW FAST [get_ports {ddram_cas_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_cas_n}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_cas_n}]

# ddram:0.we_n
set_property LOC F20 [get_ports {ddram_we_n}]
set_property SLEW FAST [get_ports {ddram_we_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_we_n}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_we_n}]

# ddram:0.cs_n
set_property LOC J17 [get_ports {ddram_cs_n}]
set_property SLEW FAST [get_ports {ddram_cs_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_cs_n}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_cs_n}]

# ddram:0.dm
set_property LOC M13 [get_ports {ddram_dm[0]}]
set_property SLEW FAST [get_ports {ddram_dm[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dm[0]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_dm[0]}]

# ddram:0.dm
set_property LOC K15 [get_ports {ddram_dm[1]}]
set_property SLEW FAST [get_ports {ddram_dm[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dm[1]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_dm[1]}]

# ddram:0.dm
set_property LOC F12 [get_ports {ddram_dm[2]}]
set_property SLEW FAST [get_ports {ddram_dm[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dm[2]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_dm[2]}]

# ddram:0.dm
set_property LOC A14 [get_ports {ddram_dm[3]}]
set_property SLEW FAST [get_ports {ddram_dm[3]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dm[3]}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_dm[3]}]

# ddram:0.dq
set_property LOC N14 [get_ports {ddram_dq[0]}]
set_property SLEW FAST [get_ports {ddram_dq[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[0]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[0]}]

# ddram:0.dq
set_property LOC N13 [get_ports {ddram_dq[1]}]
set_property SLEW FAST [get_ports {ddram_dq[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[1]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[1]}]

# ddram:0.dq
set_property LOC L14 [get_ports {ddram_dq[2]}]
set_property SLEW FAST [get_ports {ddram_dq[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[2]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[2]}]

# ddram:0.dq
set_property LOC M14 [get_ports {ddram_dq[3]}]
set_property SLEW FAST [get_ports {ddram_dq[3]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[3]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[3]}]

# ddram:0.dq
set_property LOC M12 [get_ports {ddram_dq[4]}]
set_property SLEW FAST [get_ports {ddram_dq[4]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[4]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[4]}]

# ddram:0.dq
set_property LOC N15 [get_ports {ddram_dq[5]}]
set_property SLEW FAST [get_ports {ddram_dq[5]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[5]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[5]}]

# ddram:0.dq
set_property LOC M11 [get_ports {ddram_dq[6]}]
set_property SLEW FAST [get_ports {ddram_dq[6]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[6]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[6]}]

# ddram:0.dq
set_property LOC L12 [get_ports {ddram_dq[7]}]
set_property SLEW FAST [get_ports {ddram_dq[7]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[7]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[7]}]

# ddram:0.dq
set_property LOC K14 [get_ports {ddram_dq[8]}]
set_property SLEW FAST [get_ports {ddram_dq[8]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[8]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[8]}]

# ddram:0.dq
set_property LOC K13 [get_ports {ddram_dq[9]}]
set_property SLEW FAST [get_ports {ddram_dq[9]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[9]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[9]}]

# ddram:0.dq
set_property LOC H13 [get_ports {ddram_dq[10]}]
set_property SLEW FAST [get_ports {ddram_dq[10]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[10]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[10]}]

# ddram:0.dq
set_property LOC J13 [get_ports {ddram_dq[11]}]
set_property SLEW FAST [get_ports {ddram_dq[11]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[11]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[11]}]

# ddram:0.dq
set_property LOC L16 [get_ports {ddram_dq[12]}]
set_property SLEW FAST [get_ports {ddram_dq[12]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[12]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[12]}]

# ddram:0.dq
set_property LOC L15 [get_ports {ddram_dq[13]}]
set_property SLEW FAST [get_ports {ddram_dq[13]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[13]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[13]}]

# ddram:0.dq
set_property LOC H14 [get_ports {ddram_dq[14]}]
set_property SLEW FAST [get_ports {ddram_dq[14]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[14]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[14]}]

# ddram:0.dq
set_property LOC J15 [get_ports {ddram_dq[15]}]
set_property SLEW FAST [get_ports {ddram_dq[15]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[15]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[15]}]

# ddram:0.dq
set_property LOC E15 [get_ports {ddram_dq[16]}]
set_property SLEW FAST [get_ports {ddram_dq[16]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[16]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[16]}]

# ddram:0.dq
set_property LOC E13 [get_ports {ddram_dq[17]}]
set_property SLEW FAST [get_ports {ddram_dq[17]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[17]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[17]}]

# ddram:0.dq
set_property LOC F15 [get_ports {ddram_dq[18]}]
set_property SLEW FAST [get_ports {ddram_dq[18]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[18]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[18]}]

# ddram:0.dq
set_property LOC E14 [get_ports {ddram_dq[19]}]
set_property SLEW FAST [get_ports {ddram_dq[19]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[19]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[19]}]

# ddram:0.dq
set_property LOC G13 [get_ports {ddram_dq[20]}]
set_property SLEW FAST [get_ports {ddram_dq[20]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[20]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[20]}]

# ddram:0.dq
set_property LOC G12 [get_ports {ddram_dq[21]}]
set_property SLEW FAST [get_ports {ddram_dq[21]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[21]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[21]}]

# ddram:0.dq
set_property LOC F14 [get_ports {ddram_dq[22]}]
set_property SLEW FAST [get_ports {ddram_dq[22]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[22]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[22]}]

# ddram:0.dq
set_property LOC G14 [get_ports {ddram_dq[23]}]
set_property SLEW FAST [get_ports {ddram_dq[23]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[23]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[23]}]

# ddram:0.dq
set_property LOC B14 [get_ports {ddram_dq[24]}]
set_property SLEW FAST [get_ports {ddram_dq[24]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[24]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[24]}]

# ddram:0.dq
set_property LOC C13 [get_ports {ddram_dq[25]}]
set_property SLEW FAST [get_ports {ddram_dq[25]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[25]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[25]}]

# ddram:0.dq
set_property LOC B16 [get_ports {ddram_dq[26]}]
set_property SLEW FAST [get_ports {ddram_dq[26]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[26]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[26]}]

# ddram:0.dq
set_property LOC D15 [get_ports {ddram_dq[27]}]
set_property SLEW FAST [get_ports {ddram_dq[27]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[27]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[27]}]

# ddram:0.dq
set_property LOC D13 [get_ports {ddram_dq[28]}]
set_property SLEW FAST [get_ports {ddram_dq[28]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[28]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[28]}]

# ddram:0.dq
set_property LOC E12 [get_ports {ddram_dq[29]}]
set_property SLEW FAST [get_ports {ddram_dq[29]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[29]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[29]}]

# ddram:0.dq
set_property LOC C16 [get_ports {ddram_dq[30]}]
set_property SLEW FAST [get_ports {ddram_dq[30]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[30]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[30]}]

# ddram:0.dq
set_property LOC D16 [get_ports {ddram_dq[31]}]
set_property SLEW FAST [get_ports {ddram_dq[31]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dq[31]}]
set_property IOSTANDARD SSTL15_T_DCI [get_ports {ddram_dq[31]}]

# ddram:0.dqs_p
set_property LOC N16 [get_ports {ddram_dqs_p[0]}]
set_property SLEW FAST [get_ports {ddram_dqs_p[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_p[0]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_p[0]}]

# ddram:0.dqs_p
set_property LOC K12 [get_ports {ddram_dqs_p[1]}]
set_property SLEW FAST [get_ports {ddram_dqs_p[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_p[1]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_p[1]}]

# ddram:0.dqs_p
set_property LOC H16 [get_ports {ddram_dqs_p[2]}]
set_property SLEW FAST [get_ports {ddram_dqs_p[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_p[2]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_p[2]}]

# ddram:0.dqs_p
set_property LOC C15 [get_ports {ddram_dqs_p[3]}]
set_property SLEW FAST [get_ports {ddram_dqs_p[3]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_p[3]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_p[3]}]

# ddram:0.dqs_n
set_property LOC M16 [get_ports {ddram_dqs_n[0]}]
set_property SLEW FAST [get_ports {ddram_dqs_n[0]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_n[0]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_n[0]}]

# ddram:0.dqs_n
set_property LOC J12 [get_ports {ddram_dqs_n[1]}]
set_property SLEW FAST [get_ports {ddram_dqs_n[1]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_n[1]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_n[1]}]

# ddram:0.dqs_n
set_property LOC G16 [get_ports {ddram_dqs_n[2]}]
set_property SLEW FAST [get_ports {ddram_dqs_n[2]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_n[2]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_n[2]}]

# ddram:0.dqs_n
set_property LOC C14 [get_ports {ddram_dqs_n[3]}]
set_property SLEW FAST [get_ports {ddram_dqs_n[3]}]
set_property VCCAUX_IO HIGH [get_ports {ddram_dqs_n[3]}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_dqs_n[3]}]

# ddram:0.clk_p
set_property LOC H19 [get_ports {ddram_clk_p}]
set_property SLEW FAST [get_ports {ddram_clk_p}]
set_property VCCAUX_IO HIGH [get_ports {ddram_clk_p}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_clk_p}]

# ddram:0.clk_n
set_property LOC G18 [get_ports {ddram_clk_n}]
set_property SLEW FAST [get_ports {ddram_clk_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_clk_n}]
set_property IOSTANDARD DIFF_SSTL15 [get_ports {ddram_clk_n}]

# ddram:0.cke
set_property LOC K19 [get_ports {ddram_cke}]
set_property SLEW FAST [get_ports {ddram_cke}]
set_property VCCAUX_IO HIGH [get_ports {ddram_cke}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_cke}]

# ddram:0.odt
set_property LOC H20 [get_ports {ddram_odt}]
set_property SLEW FAST [get_ports {ddram_odt}]
set_property VCCAUX_IO HIGH [get_ports {ddram_odt}]
set_property IOSTANDARD SSTL15 [get_ports {ddram_odt}]

# ddram:0.reset_n
set_property LOC C29 [get_ports {ddram_reset_n}]
set_property SLEW FAST [get_ports {ddram_reset_n}]
set_property VCCAUX_IO HIGH [get_ports {ddram_reset_n}]
set_property IOSTANDARD LVCMOS15 [get_ports {ddram_reset_n}]

# eth:0.rst_n
set_property LOC AJ33 [get_ports {eth_rst_n}]
set_property IOSTANDARD LVCMOS18 [get_ports {eth_rst_n}]

# eth:0.int_n
set_property LOC AL31 [get_ports {eth_int_n}]
set_property IOSTANDARD LVCMOS18 [get_ports {eth_int_n}]

# eth:0.mdio
set_property LOC AK33 [get_ports {eth_mdio}]
set_property IOSTANDARD LVCMOS18 [get_ports {eth_mdio}]

# eth:0.mdc
set_property LOC AH31 [get_ports {eth_mdc}]
set_property IOSTANDARD LVCMOS18 [get_ports {eth_mdc}]

# eth:0.rx_p
set_property LOC AM8 [get_ports {eth_rx_p}]

# eth:0.rx_n
set_property LOC AM7 [get_ports {eth_rx_n}]

# eth:0.tx_p
set_property LOC AN2 [get_ports {eth_tx_p}]

# eth:0.tx_n
set_property LOC AN1 [get_ports {eth_tx_n}]

# sgmii_clock:0.p
set_property LOC AH8 [get_ports {sgmii_clock_p}]

# sgmii_clock:0.n
set_property LOC AH7 [get_ports {sgmii_clock_n}]

# user_led:0
set_property LOC AM39 [get_ports {user_led0}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led0}]

# user_led:1
set_property LOC AN39 [get_ports {user_led1}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led1}]

# user_led:2
set_property LOC AR37 [get_ports {user_led2}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led2}]

# user_led:3
set_property LOC AT37 [get_ports {user_led3}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led3}]

# user_led:4
set_property LOC AR35 [get_ports {user_led4}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led4}]

# user_led:5
set_property LOC AP41 [get_ports {user_led5}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led5}]

# user_led:6
set_property LOC AP42 [get_ports {user_led6}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led6}]

# user_led:7
set_property LOC AU39 [get_ports {user_led7}]
set_property IOSTANDARD LVCMOS18 [get_ports {user_led7}]

################################################################################
# Design constraints
################################################################################

set_property CFGBVS VCCO [current_design]

set_property CONFIG_VOLTAGE 2.5 [current_design]

set_property BITSTREAM.STARTUP.MATCH_CYCLE NoWait [current_design]

set_property LOC GTXE2_CHANNEL_X1Y1 [get_cells liteeth_sgmii_phy.GTXE2_CHANNEL]

set_property LOC IBUFDS_GTE2_X1Y0 [get_cells liteeth_sgmii_phy.IBUFDS_GTE2]

set_property LOC MMCME2_ADV_X0Y6 [get_cells liteeth_sgmii_phy.MMCME2_ADV]

set_property LOC MMCME2_ADV_X0Y3 [get_cells liteeth_sgmii_phy.MMCME2_ADV_1]

set_property LOC BUFGCTRL_X0Y2 [get_cells liteeth_sgmii_phy.bufg_rxoutrebuf]

set_property LOC BUFGCTRL_X0Y3 [get_cells liteeth_sgmii_phy.bufg_txoutrebuf]

set_property LOC BUFGCTRL_X0Y5 [get_cells liteeth_sgmii_phy.bufg_ethrx62]

set_property LOC BUFGCTRL_X0Y6 [get_cells liteeth_sgmii_phy.bufg_ethrx125]

set_property LOC BUFGCTRL_X0Y16 [get_cells liteeth_sgmii_phy.bufg_ethtx62]

set_property LOC BUFGCTRL_X0Y17 [get_cells liteeth_sgmii_phy.bufg_ethtx125]

################################################################################
# Clock constraints
################################################################################


create_clock -name clk200_p -period 5.0 [get_ports clk200_p]

create_clock -name sgmii_clock_p -period 8.0 [get_ports sgmii_clock_p]

create_clock -name eth_tx_clk -period 8.0 [get_nets eth_tx_clk]

create_clock -name eth_rx_clk -period 8.0 [get_nets eth_rx_clk]

################################################################################
# False path constraints
################################################################################


set_false_path -quiet -to [get_cells -hierarchical -filter {mr_ff == TRUE}]

set_false_path -quiet -to [get_pins -filter {REF_PIN_NAME == PRE} -of_objects [get_cells -hierarchical -filter {ars_ff1 == TRUE || ars_ff2 == TRUE}]]

set_max_delay 2 -quiet -from [get_pins -filter {REF_PIN_NAME == C} -of_objects [get_cells -hierarchical -filter {ars_ff1 == TRUE}]] -to [get_pins -filter {REF_PIN_NAME == D} -of_objects [get_cells -hierarchical -filter {ars_ff2 == TRUE}]]

set_clock_groups -group [get_clocks -of [get_nets eth_tx_clk]] -group [get_clocks -of [get_nets eth_rx_clk]] -asynchronous

set_clock_groups -group [get_clocks -of [get_nets eth_tx_clk]] -group [get_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -of [get_nets eth_rx_clk]] -group [get_clocks -of [get_nets sys_clk]] -asynchronous

set_clock_groups -group [get_clocks -of [get_nets sys_clk]] -group [get_clocks -of [get_nets main_crgddr_clkin_signal]] -asynchronous