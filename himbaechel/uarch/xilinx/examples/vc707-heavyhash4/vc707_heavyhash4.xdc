# vc707_miner (open flow).  Pins and hard-block placement from
# vc707_ethmin_vm_open.xdc; the PHY sits one level down here (u_core.u_framing.eth).

set_property -dict {PACKAGE_PIN E19 IOSTANDARD LVDS} [get_ports IO_CLK_P]
set_property -dict {PACKAGE_PIN E18 IOSTANDARD LVDS} [get_ports IO_CLK_N]
create_clock -period 5.000 -name sysclk [get_ports IO_CLK_P]

set_property -dict {PACKAGE_PIN AV40 IOSTANDARD LVCMOS18} [get_ports IO_RST]

set_property -dict {PACKAGE_PIN AM39 IOSTANDARD LVCMOS18} [get_ports {LED[0]}]
set_property -dict {PACKAGE_PIN AN39 IOSTANDARD LVCMOS18} [get_ports {LED[1]}]
set_property -dict {PACKAGE_PIN AR37 IOSTANDARD LVCMOS18} [get_ports {LED[2]}]
set_property -dict {PACKAGE_PIN AT37 IOSTANDARD LVCMOS18} [get_ports {LED[3]}]
set_property -dict {PACKAGE_PIN AR35 IOSTANDARD LVCMOS18} [get_ports {LED[4]}]
set_property -dict {PACKAGE_PIN AP41 IOSTANDARD LVCMOS18} [get_ports {LED[5]}]
set_property -dict {PACKAGE_PIN AP42 IOSTANDARD LVCMOS18} [get_ports {LED[6]}]
set_property -dict {PACKAGE_PIN AU39 IOSTANDARD LVCMOS18} [get_ports {LED[7]}]

set_property CFGBVS GND [current_design]
set_property CONFIG_VOLTAGE 1.8 [current_design]

# --- SGMII (GT bank 117); the GT diff pins take no IOSTANDARD ---
set_property PACKAGE_PIN AH8 [get_ports sgmii_refclk_p]
set_property PACKAGE_PIN AH7 [get_ports sgmii_refclk_n]
create_clock -period 8.000 -name sgmii_refclk [get_ports sgmii_refclk_p]
set_property PACKAGE_PIN AN2 [get_ports sgmii_txp]
set_property PACKAGE_PIN AN1 [get_ports sgmii_txn]
set_property PACKAGE_PIN AM8 [get_ports sgmii_rxp]
set_property PACKAGE_PIN AM7 [get_ports sgmii_rxn]
set_property PACKAGE_PIN AJ33 [get_ports eth_rst_n]
set_property IOSTANDARD LVCMOS18 [get_ports eth_rst_n]
set_false_path -to [get_ports eth_rst_n]

# Hard-block placement (see vc707_ethmin_vm_open.xdc for why these are
# required: left free, the PHY MMCMs land where the GT clocks cannot reach).
# get_cells takes the netlist's cell names, where the synthesis flattens the
# hierarchy with '_' (clkgen_mmcm, not clkgen.mmcm); nextpnr ignores a name
# that matches nothing without a word, so these must be exact.
set_property LOC BUFGCTRL_X0Y18 [get_cells clkgen_clk_mac_bufg]
set_property LOC BUFGCTRL_X0Y16 [get_cells clkgen_clk_sys_bufg]
set_property LOC MMCME2_ADV_X1Y5 [get_cells clkgen_mmcm]
set_property LOC GTXE2_CHANNEL_X1Y1 [get_cells u_core_u_framing_eth_i_phy_GTXE2_CHANNEL]
set_property LOC MMCME2_ADV_X0Y6 [get_cells u_core_u_framing_eth_i_phy_MMCME2_ADV]
set_property LOC MMCME2_ADV_X0Y3 [get_cells u_core_u_framing_eth_i_phy_MMCME2_ADV_1]
set_property LOC BUFGCTRL_X0Y1 [get_cells u_core_u_framing_eth_i_phy_bufg_ethrx125]
set_property LOC BUFGCTRL_X0Y0 [get_cells u_core_u_framing_eth_i_phy_bufg_ethrx62]
set_property LOC BUFGCTRL_X0Y19 [get_cells u_core_u_framing_eth_i_phy_bufg_ethtx125]
set_property LOC BUFGCTRL_X0Y17 [get_cells u_core_u_framing_eth_i_phy_bufg_ethtx62]
set_property LOC BUFGCTRL_X0Y2 [get_cells u_core_u_framing_eth_i_phy_bufg_rxoutrebuf]
set_property LOC BUFGCTRL_X0Y3 [get_cells u_core_u_framing_eth_i_phy_bufg_txoutrebuf]

# The PHY's recovered clocks (nextpnr cannot derive them from the GTX).
create_clock -name eth_rx_clk -period 8.000 [get_nets u_core.u_framing.eth.eth_rx_clk]
create_clock -name eth_tx_clk -period 8.000 [get_nets u_core.u_framing.eth.eth_tx_clk]

# Lanes: one BUFHCE per lane, and a BUFHCE drives exactly the clock region it
# sits in, so nextpnr keeps the whole lane there (heavyhash_lanes.sv).  Sites:
# BUFHCE_X0Y<12r..12r+11> drive region X0Y<r>, BUFHCE_X1Y<12r..> region X1Y<r>.
# The control logic, MAC and PHY are left X1Y0 and the PHY MMCM regions X0Y3/X0Y6.
# One line per lane in LANES (vc707_miner.json); a line for an absent lane is
# ignored.
