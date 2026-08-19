# 02 — Fork feature inventory: `nextpnr-xilinx` (openXC7) feature reference

> **Status: final draft for review.** Read-only analysis of
> `/devel/HDL/kintex-reveng/nextpnr-xilinx` @ `68aeeb39` (branch `main`,
> tag `0.9.3`, verified 2026-08-19). This document inventories the fork's
> complete feature set so document 04 can diff it against upstream
> `himbaechel/uarch/xilinx`.
>
> **Framing.** Upstream nextpnr/himbaechel is the *successor* of
> nextpnr-xilinx and is the preferred implementation; the fork is used
> here strictly as the **feature reference for filling blanks** (things
> upstream lacks), never as the base to port from wholesale. Note: the
> fork gained 8 commits while this analysis was underway (PRs #134,
> #148, #151, #153); all counts are against the final `0.9.3` tag.

Line numbers cite the fork's `xilinx/` tree unless a `common/` path is given.

---

## 1. Chip families / devices supported

Two independent flows share one binary (`Arch` has an `xc7` bool):

| Flow | Database | Bitstream | Submodule |
|------|----------|-----------|-----------|
| **xc7** | Project X-Ray (openxc7 fork) | FASM → `fasm2frames` → `xc7frames2bit` (no Vivado) | `xilinx/external/prjxray-db` |
| **xcup (UltraScale+)** | RapidWright BBA export | RapidWright + Vivado (`json2dcp`) | none (uses `$RAPIDWRIGHT_PATH`) |

### xc7 families & parts (from `xilinx/external/prjxray-db/<family>/`)

Device dirs are the chipdb source; each has per-part speed-grade subdirs, `timings/`,
`tile_type_*.json`, `site_type_*.json`, `segbits_*`, `ppips_*`, `mask_*`, `gridinfo`,
`mapping`, `cells_data`, `settings.sh`:

- **artix7**: `xc7a35t`, `xc7a50t`, `xc7a100t`, `xc7a200t` (packages csg324/cpg236/ftg256/fgg484/fbg484/fbg676/ffg1156/…, grades −1/−2/−2L/−3)
- **kintex7**: `xc7k70t`, `xc7k160t`, `xc7k325t`, `xc7k420t`, `xc7k480t`
- **zynq7**: `xc7z010`, `xc7z020`, `xc7z030`, `xc7z035`, `xc7z045`, `xc7z100` (has `PS7`/`pss*` tile pips)
- **spartan7**: `xc7s50`
- **virtex7**: `xc7vx485t` (ffg1761-2; only the VC707 part, GTX-equipped)

Site-type coverage per family is visible in each `site_type_*.json` set — notably
artix7 = GTPE2 transceivers, kintex7/virtex7 = GTXE2, zynq7/kintex7 add `IOB18`/`RIOB18`
(HP banks) and `ILOGICE2`/`OLOGICE2`/`ODELAYE2`/`IDELAYE2_FINEDELAY`, virtex7 has no
BUFG HROW rebuf (uses `CLK_BUFG_TOP_R`/`BOT_R` only). The fork does **not** have
UltraScale (non-+) xc7-style database support; "xcup" is RapidWright-only.

### xcup parts (RapidWright)

- `xczu2cg-sbva484-1-e` — `xilinx/examples/blinky` + `attosoc`
- `xczu7ev-ffvc1156-2-e` — `xilinx/examples/zcu104`
- README (`xilinx/README`? actually repo root `README.md`) names `xczu2cg-sbva484-1-e`
  as the canonical example; any RapidWright-supported part can be exported, subject
  to the primitive caveats below (packers are UltraScale+-specific).

### Chipdb generation pipeline

- **xc7**: `python3 xilinx/python/bbaexport.py --device <part> --xray <db-dir> --bba out.bba`,
  then `bbasm -l out.bba out.bin`. The python side (`xilinx/python/`) is
  `bbaexport.py` (driver), `xilinx_device.py` (DB object model), `bels.py`,
  `tileconn.py`, `constid.py`, `nextpnr_structs.py`, `parse_sdf.py` (SDF → timing POD).
  `bba.py` emits the text BBA consumed by `bbasm`.
- **xcup**: `java -jar rapidwright_bbaexport.jar <part> xilinx/constids.inc out.bba`
  (`xilinx/java/bbaexport.java`), same `bbasm` second stage. `xilinx/java/json2dcp.java`
  converts the routed JSON back to a DCP for Vivado.
- `xilinx/chipdb.hexpat` — ImHex pattern for inspecting the binary chipdb.
- `xilinx/external/nextpnr-xilinx-meta/` — per-family `site_type_*.json` + `wire_intents.json`
  metadata (extracted "by a mix of Tcl and staring at the Vivado GUI").
- `xilinx/constids.inc` (786 lines) — every IdString the arch knows (cell types, BEL
  types, ports, params, node intents); the canonical primitive/port inventory.
- Build wiring: `xilinx/family.cmake` builds the Java helpers iff `RAPIDWRIGHT_PATH`
  + `GSON_PATH` are set; `CMakeLists.txt` sets `ARCH=xilinx`, `EXTERNAL_CHIPDB`,
  `SERIALIZE_CHIPDB` options.

---

## 2. Primitive support (packer → file + approx lines)

Cell-type → BEL transforms use the `XFormRule` mechanism (`pack.h:38`,
`pack.cc:62 xform_cell`, `pack.cc:114 generic_xform`). Top-level dispatch is
`Arch::pack()` at `pack.cc:1391` (xc7 branch) and `1429` (xcup branch). Order
(xc7): constants → inverters → IO → prepare_clocking → iologic → idelayctrl →
cfg → plls → gt → gbs → `split_lut6_2` → muxfs → carries → carry-O relocate →
srls → luts → dram → bram → dsps → ffs → finalise_muxfs → lutffs.

### Logic / fabric
- **LUT1–LUT6** → `SLICE_LUTX` (`pack_luts`, `pack.cc:347`).
- **LUT6_2** → two `SLICE_LUTX` halves (`split_lut6_2`, `pack.cc:201`); BEL-constrained
  LUT6_2 only split when *both* outputs are used (`pack.cc:223`, commit `bfdeaf7c`).
- **FDCE/FDPE/FDRE/FDSE/LDCE/LDPE (+ `_1`)** → `SLICE_FFX` (`pack_ffs`, `pack.cc:363`).
- **MUXF7/MUXF8/MUXF9** → `SELMUX2_1` on xc7, `F7MUX/F8MUX/F9MUX` on xcup
  (`finalise_muxfs`, `pack.cc:670`; tree legalisation/constraining in
  `legalise_muxf_tree`/`constrain_muxf_tree`/`create_muxf_tree`, `pack.cc:535/592/`
  `pack_dram.cc:97`). LUT6_2→MUXF7/8/9 use-after-free fixed (`31c8ea77`).
- **CARRY4** (xc7): two packers — legacy split into `MUXCY`+`XORCY`
  (`split_carry4s`, `pack_carry_xc7.cc:71`) and the default atomic
  `pack_carries_atomic` (`pack_carry_xc7.cc:120`, keeps Vivado CARRY4 as one cell,
  chain linked CO[3]→CI). `pack_carries` (`pack_carry_xc7.cc:719`) and
  `relocate_carry_o_fabric` (`pack_carry_xc7.cc:1052`, ALU chains that tap both O and
  CO: split + sum-duplication + spine link + row spread). `XC7_LEGACY_CARRY4_SPLIT=1`
  env fallback.
- **CARRY8** (xcup) → `pack_carries` (`pack_carry_xcup.cc:69`), `has_illegal_fanout`
  (`pack_carry_xcup.cc:35`).
- **SRL16E / SRLC32E** → `SLICE_LUTX` (`pack_srls`, `pack.cc:685`); cascade
  legalisation `constrain_srl_cascades` (`pack.cc:763`); placed like carry chains
  (commit `697e293b`).

### Distributed RAM (`pack_dram.cc`, `pack_dram` @148)
`RAM32M`, `RAM64M`, `RAM32M16`, `RAM64M8`, `RAM32X1S`, `RAM64X1S`, `RAM128X1S`,
`RAM256X1S`, `RAM32X1D`, `RAM64X1D`, `RAM128X1D`, `RAM256X1D`, `RAM64X2S`,
`RAM64X8SW`, `RAM512X1S`. Packed into SLICEM LUTs (`create_dram_lut` @35,
`create_dram32_lut` @66) plus F7/F8 output mux trees (`create_muxf_tree` @97).
`DRAMControlSet` keying (wa/wclk/we/wclk_inv/memtype) in `pack.h:52`. Recent fixes:
RAM128X1S scalar A0..A6 (`c0194daf`), RAM256X1S mux-tree own-slice-half (`363c055d`),
zoffset (`b390e9c9`).

### Block RAM
- xc7 **RAMB18E1 / RAMB36E1** (and FIFO18E1/FIFO36E1/RAMBFIFO36E1 constids) →
  `XC7Packer::pack_bram` (`pack.cc:1185`). SDP, registered outputs, read/write
  width config, ZINV_REGCLK (below).
- xcup **RAMB18E2 / RAMB36E2 / FIFO18E2 / FIFO36E2** → `USPacker::pack_bram`
  (`pack.cc:1053`); **URAM288** → `pack_uram` (`pack.cc:1370`).

### DSP
- xc7 **DSP48E1** → `DSP48E1_DSP48E1` (`pack_dsp_xc7.cc`); **cascading supported**
  (`walk_dsp` @26 walks ACOUT/BCOUT/PCOUT/CARRYCASCOUT/MULTSIGNOUT → constrains
  upper/lower BEL z, `pack_dsps` @83).
- xcup **DSP48E2** → `pack_dsp_xcup.cc:26` (`pack_dsps`), **no cascading** (macro
  expansion to multiple BELs, comment @50).

### IOB / pads
- xc7: `IBUF/OBUF/IBUFDS/OBUFDS/OBUFT/OBUFTDS/IOBUF/IOBUFDS` + `_DCIEN`/
  `_INTERMDISABLE`/`_DIFF_OUT` variants; `IBUFGDS` accepted as alias of `IBUFDS`
  (`pack_io_xc7.cc`, commit `55c3bc87`). `decompose_iob` (`pack_io_xc7.cc:67`)
  builds PAD/INBUF/OUTBUF/TRIBUF tree per HR (`is_hr`) or HP (RIOB18/LIOB18) bank;
  `pack_io` @327; pad validation `check_valid_pad` @584 (IOSTANDARD/DRIVE checking,
  HP-bank drive rules).
- xcup: `IBUFCTRL/INBUF/DIFFINBUF` decomposition (`pack_io_xcup.cc:35–122`),
  `IOBUFDS_DIFF_OUT` etc.; `pack_io` @461.

### IOLOGIC (xc7, `pack_io_xc7.cc`)
- **IDDR** → `ILOGICE3_IFF` (`pack_iologic` @772; `DDR_CLK_EDGE=SAME_EDGE_PIPELINED`
  supported, commit `9a6a7e3b`, `OPPOSITE_EDGE` default; all four IFF flops
  initialised `d455ae52`; routethru SRTYPE conflict fixed `f77907ac`/`16accf3b`).
- **ODDR** → `OLOGICE2/3_OUTFF` (or `_TFF` for tristate) @868.
- **ISERDESE2 / OSERDESE2** → `ISERDESE2_ISERDESE2`/`OSERDESE2_OSERDESE2` @778; master/
  slave SHIFTOUT/SHIFTIN pairing, OFB (OSERDES→ISERDES) placement @903–1106.
- **IDELAYE2 / ODELAYE2** → `IDELAYE2_IDELAYE2`/`ODELAYE2_ODELAYE2` @782–857.
- **IDELAYCTRL** → `pack_idelayctrl` @1129 (RDY AND-tree of duplicated CTRLs; no-delay
  IDELAYCTRL = warning not error, `06769c05`).
- xcup equivalents (`pack_io_xcup.cc`): `ISERDESE3/OSERDESE3/IDDRE1/ODDRE1/IDELAYE3/
  ODELAYE3`, BITSLICE RX/TX (`prepare_iologic` @578, `pack_iologic` @595,
  `pack_idelayctrl` @745).

### Clocking
- xc7 (`pack_clocking_xc7.cc`): `prepare_clocking` @36; **PLLE2_ADV/PLLE2_BASE,
  MMCME2_ADV/MMCME2_BASE** → `pack_plls` @132 (MMCM/PLL tables, filter tables,
  CLKFBOUT_MULT_F range-checked, commit `e33b5f1a`/`0e85a878`); **BUFG/BUFGCTRL/
  BUFGCE/BUFH/BUFHCE/BUFMRCE** → `pack_gbs` @230 (preplace dedicated/short routes,
  fabric-driven fallback, PLL/MMCM-driven pinned to bottom region, const control-pin
  disconnect); `pack_clocking` @331. **BUFR/BUFIO** (BUFR_DIVIDE honoured, `0b914578`).
- xcup (`pack_clocking_xcup.cc`): `prepare_clocking` @150, `pack_plls` @170
  (PLLE4/MMCME4), `pack_gbs` @210, `pack_clocking` @238; `find_bel_with_short_route`
  @35, `try_preplace` @112, `preplace_unique` @137 (shared with xc7 via `pack.h:163`).

### GT / transceivers (xc7, `pack_gt_xc7.cc`)
`GTPE2_CHANNEL`/`GTPE2_COMMON` (artix7), `GTXE2_CHANNEL`/`GTXE2_COMMON`
(kintex7/virtex7), `IBUFDS_GTE2`, `OBUFDS_GTE2`. `get_gt_site` @26,
`constrain_ibufds_gt_site` @44, `constrain_gt` @109, `pack_gt` @130 (REFCLK/GTGREFCLK/
PLL0/PLL1 port remapping, channel/common site binding). xcup constids also list
`GTHE2/GTHE4/GTHE3`, `IBUFDS_GTE3/GTE4`, `OBUFDS_GTE3/GTE4_ADV` etc. (RapidWright path).

### Configuration / misc (xc7)
`pack_cfg` (`pack_io_xc7.cc:1232`): **BSCANE2** (JTAG_CHAIN 1–4 → `BSCAN_X0Y{n}/BSCAN`),
**DCIRESET, DNA_PORT, EFUSE_USR, ICAPE2, FRAME_ECCE2, STARTUPE2, USR_ACCESSE2** — each
preplaced to its single dedicated site (`d42d6c9b`). Other constids: **PS7** (zynq),
**PCIE_2_1**, **XADC/SYSMONE1**, **PHASER_IN/PHASER_OUT/PHASER_REF**, **CAPTURE**,
**STARTUP** (`STARTUPE2` has USRCCLKO). `$PACKER_VCC_NET`/`$PACKER_GND_NET` pseudo
cells (`PSEUDO_VCC/PSEUDO_GND`) created in `pack_constants` (`pack.cc:914`).

Full cell/port list: `xilinx/constids.inc` (786 lines) — the authoritative reference.

---

## 3. FASM / bitstream correctness (`xilinx/fasm.cc`, 5292 lines)

`Arch::writeFasm` @5266; single `FasmBackend` class drives emission via
`write_fasm()` @5250 = `write_logic → write_cfg → write_io → write_routing →
write_bram → write_clocking → write_ip`.

Major features:
- **Run-identity header** (`writeFasm` @5272–5285, commit `7037c948`): emits
  `# nextpnr-xilinx <GIT_DESCRIBE>`, `# chipdb <name> version <v> generator <g>`,
  `# placer seed <s>`, `# placer rngstate <r>` as FASM comments (dropped by
  fasm2frames, so bitstream hash is unaffected).
- **Emission helpers**: `write_bit`/`write_vector`/`write_int_vector` (@68–96),
  context stack `push/pop` (@44–52), `blank()` line grouping.
- **Pseudo-pip config** `pp_config` (@98–122): maps (tileType,dest,source) → FASM
  feature list for route-thru/pseudo pips; `extra_data` default-bit handling (@441).
- **Phantom-BUFGCTRL guard** (`populate_bufgctrl_bound_slots` @122–…): suppresses
  `BUFGCTRL.BUFGCTRL_X0Y*.*` bits emitted purely because the router crossed an idle
  BUFG tile's IMUX; fixes double-programmed BUFG contention (dead clock) on single-BUFG
  designs.
- **Logic**: LUT INIT packing (`write_luts_config` @837, `INIT` param → bits @591),
  LUT6_2 O5/O6; FF config (`write_ffs_config` @676) — `ZINI/ZRST/LATCH/FFSYNC/CLKINV/
  NOCLKINV/SRUSEDMUX/CEUSEDMUX`; carry (`write_carry_config` @935) — `PRECYINIT.C1/C0`,
  CY0 pass-through (S=VCC fill), COUT spine; DRAM/SRL `WA7USED/WA8USED/SMALL/RAM/SRL`
  (@889–928); output-mux `xOUTMUX` emission (@896–922).
- **BRAM** (`write_bram` @2389, `write_bram_half` @2281, `write_bram_width` @2141,
  `write_bram_init` @2244): read/write width markers, SDP opposite-port width
  (`f1c77134`/`11f9b694`), 36-wide marker collision fix (`1b7d51b9`), registered
  outputs `ZINV_REGCLKARDRCLK`/`ZINV_REGCLKB` (`e71acda2`).
- **IO config** (`write_io_config` @1082, `write_io` @1772): IOSTANDARD/DRIVE/SLEW/
  PULLTYPE emission (@1086–1499) — HP-bank glue logic: skip `IBUF_HP_BANK_GLUE` on
  RIOB18 master Y0 (`d6b7f64d`), skip HP `IN_ONLY` when partner IOB drives output
  (`e4a261ce`), skip HP cross-site `SLEW.SLOW` when partner active (`70a5952c`), skip
  RIOB18 diff-input `SLEW.SLOW` on bidir pads (`c2e50b99`), SSTL15/SSTL135 SLEW.SLOW
  skip (`6f33adf0`), per-IOSTANDARD DRIVE groups (@1140–1200), LVDS/TMDS_33/LVDS_25
  fixed drive, PULLTYPE.PULLDOWN complement handling (@1479–1504).
- **Clocking** (`write_clocking` @1947): `write_pll` @2536 / `write_mmcm` @2720
  (DIVCLK/CLKFBOUT/CLKOUT0–6; PLL-specific `LKTABLE`/`TABLE` filter tables,
  `e33b5f1a`), `write_bufr` @2504 (BUFR_DIVIDE), `write_gtp_pll` @3167 /
  `write_gtx_pll` @4464, `write_ibufds_gte2` @3150.
- **IP** (`write_ip` @5218): `write_dsp_cell` @5084, `write_gtp_channel` @3245,
  `write_gtx_channel` @4538, `write_pcie_2_1` @3822, BUFR.
- **Routing** (`write_routing` @1021, `write_pip` @359, `write_routing_bel` @620):
  pip emission with BUFGCTRL phantom guard, LUT-input permutation (`PIP_LUT_PERMUTATION`).

---

## 4. Constraints (`xilinx/xdc.cc`, 309 lines)

Parsed (not ignored):
- **`set_property`** on `get_ports`/`get_nets` (only these selectors; `get_cells`
  unsupported for set_property → warning @112). Stores raw attr on the cell:
  `PACKAGE_PIN`, `IOSTANDARD`, `DRIVE`, `SLEW`, `PULLTYPE` (PULLUP/PULLDOWN/KEEPER/
  NONE), `LOC`/`BEL`, `IN_TERM`, and any other property verbatim. Semantics are
  applied later in `pins.cc` + `fasm.cc` (IOSTANDARD/DRIVE/SLEW/PULLTYPE) and
  `arch.cc:getBelByName`/placer (LOC/BEL). `-dict` supported (@188).
  `INTERNAL_VREF` explicitly skipped (@198).
- **`create_clock`** (@208): `-period` (required), `-name`, `-waveform`, `-add`
  consumed; sets `net->clkconstr` period/high/low. Virtual clocks (no target) →
  warning + skip (@227–231). Miss target → warning + default period kept (@234–238).
- **`set_multicycle_path`** (@245): `-setup` only; `-to [get_cells -hier -filter
  {NAME =~ *glob*}]` NAME-glob matched (`*`/`?`), tags matching cells with
  `NEXTPNR_MCP_SETUP` attr; `-hold` parsed but no-op (@294). Honoured in
  `common/timing.cc` (walk_paths @390–393, @567–569, @909).

Parsed-then-ignored / unsupported (logged):
- `[current_design]` / `[current_project]` target → warning + skip (@200).
- `create_clock` virtual clock → warning + skip.
- Any other command (e.g. `set_false_path`, `set_input_delay`, `set_output_delay`,
  `set_clock_groups`, `set_max_delay`) → `ignoring unsupported XDC command` (@298).

Robustness/diagnostics:
- `name[0]` one-bit-vector port de-busing (`debus_zero` @97, commit `b257be4d`).
- Missing-target handling: silent by default, itemised only with `--verbose`
  (`get_cells` @123–132, `get_nets` @158–163, summary @303; commits `3da43687`,
  `555d326c`).
- `#` comment stripping, `;` terminator, `{}`/`[]` grouping in tokeniser
  (`split_to_args` @52).

---

## 5. Timing engine

- **Delay model source**: prjxray-db `timings/` SDF files (`*_slicel/slicem`,
  `BRAM_L/R`, `CLBLL_L/R`, `carry4_*`, …) parsed by `xilinx/python/parse_sdf.py`
  and baked into the chipdb as `timing_data` POD (`bbaexport.py` @42–52, @282–357:
  wire/pip timing classes + per-tile cell-timing instances).
- **Cell delay lookup**: `Arch::getCellDelay` (`arch.cc:2375`) →
  `xc7_cell_timing_lookup` (`arch.cc:2577`) binary-searches the per-BEL-instance
  `CellTimingPOD` by variant name (`LUT5`/`LUT6`/`LUT_OR_MEM5LRAM`/`LUT_OR_MEM6LRAM`,
  `CARRY4`, `F7MUX`/`F8MUX`/`F9MUX`/`SELMUX2_1`) and (from_port,to_port), returns
  `max_delay`. Fractured-LUT pin-shadow guard (@2383–2396). DSP48E1 combinational
  model: `dsp48e1IsCombinational` @2328, `dsp48e1CombInputDelayNS` @2344,
  `dsp48e1IsTimedOutput` @2367.
- **Port classes**: `getPortTimingClass` (`arch.cc:2451`), `getPortClockingInfo`
  @2504 (fixed 0.1 ns setup/hold/clkToQ, RISING_EDGE), `estimateDelay` @650 /
  `predictDelay` @822 / `getBoundingBoxCost` @804 (wire/pip heuristics).
- **Timing-driven placement**: HeAP `cfg.criticalityExponent = 7` (`arch.cc:859`),
  placer1 refinement pass.
- **Final timing**: `route()` runs `timing_analysis(..., print_fmax, print_path)`
  after router2 (`arch.cc:2165`, commit `7ea51730`); router1 runs its own.
- **`--report <file>`** (`common/command.cc:124`, `common/timing.cc:833 reportJson`):
  mainline-schema JSON — `critical_paths`, `fmax` (`achieved`/`constraint` per clock,
  with `$iopadmap$`/`$internal` name aliasing), `utilization` (available/used per BEL
  type). `reportClockFmaxJson` @818.
- Combinational loops ignored by default (warning instead of failure, `f6e06896`);
  set_multicycle_path −setup relaxes capture setup in the walk (above).

---

## 6. Placement / legalisation (`xilinx/arch_place.cc`, 1860 lines)

Only 8 `Arch::` methods — the bulk is the two validity checkers.

- **`xc7_logic_tile_valid`** (`arch_place.cc:325–1007`) — the core legality engine:
  1. Frozen-tile fast path: all-`STRENGTH_USER`-bound (imported Vivado) tile is
     trusted legal (`@336–367`).
  2. SLICEM-only guard: memory/SRL LUTs illegal in SLICEL (`@368–384`).
  3. 5LUT bel constraints: ≤5 inputs, ≤1 output, no A6 (`@385–423`).
  4. **Per-position site-exit (OUTMUX) budget** (`@424–506`): ≤1 claimant among
     5LUT O5 / carry O / carry CO / 5FF Q external fanout (`f0f2c817`).
  5. LUT6/LUT5 coexistence: input_count==6 or output_count==2 forbids a 5LUT
     (except SRL pairs, imported slots) (`@541–583`); shared-input counting for
     >5 total inputs with imported-trust exemption (`@584–623`).
  6. X-input over-use: F7/F8 mux select, carry X, FF1/FF2 D must agree on one X net
     (`@658–787`); memory address-MSB collision (`@806–819`).
  7. **carry→FF pairing**: cross-position carry O→FF flat-rejected (`@731–745`);
     5FF fed by own-slice carry rejected (`@766–774`); FF1_uses_x + 5FF co-pack
     rejected unless both BEL-pinned (`@789–804`, commit `b888dfd6`).
  8. Output-mux contention among O5 / carry O / carry CO / F7F8 / 5FF
     (`@821–904`; CO-vs-5FF opt-out `NEXTPNR_ALLOW_CO_5FF_CONTENTION`).
  9. Half-tile control-set guards: clk/sr/ce + clkinv/srinv/latch/ffsync must match
     across a half; wclk must equal FF clk in bottom half (`@913–1005`).
- **`xcu_logic_tile_valid`** (`arch_place.cc:54–324`) — UltraScale+ analogue.
- **`isBelLocationValid`** (`arch_place.cc:1032`), **`isValidBelForCell`** @1079.
- **`fixupPlacement`** (`arch_place.cc:1193`): post-place legalisation — relocates
  stranded cluster roots (LUT6 on 5LUT, memory/SRL in SLICEL), carry-O relocation
  round 2, pin-merge fixup, SRL/OSERDES post-fixes.
- **`fixupRouting`** (`arch_place.cc:1669`): post-route LUT permutation → `X_ORIG_PORT`
  physical↔logical remap (RapidWright/Vivado compatibility).
- **`Arch::place()`** (`arch.cc:853`): HeAP config (criticalityExponent=7,
  ioBufTypes, `cellGroups` SLICE_LUTX/SLICE_FFX/CARRY8, hpwl/spread scales,
  env knobs `NEXTPNR_PLACER_BETA/SPREAD_SCALE_X/SPREAD_SCALE_Y/PLACER_ALPHA`);
  `placer1` fallback; `fixupPlacement` after.

Preplacement (packer-side, all in `pack_*`): dedicated/short-route BUFGs, fabric-driven
BUFG fallback, PLL/MMCM BUFGs pinned to bottom-region (`pack_clocking_xc7.cc:230`,
commits `ce9e6a96`, `a70ae4a8`, `f7049aee`), single-site CFG primitives
(`pack_io_xc7.cc:1232`, `d42d6c9b`), IDELAYCTRL (`pack_io_xc7.cc:1129`), GT sites
(`pack_gt_xc7.cc`), PS7/PCIE (`pack_clocking_xc7.cc:235`).

---

## 7. Routing features

`Arch::route()` (`arch.cc:2097`):
- Order: `assign_budget` → `routeClock()` (@1752 dedicated clock backbone, runs
  FIRST so Vcc flood can't claim CCIO→CMT→HROW→CK_MUXED) → `applyFixedRoutes()`
  (@1134) → optional `--route-clock-only` (hands off to RapidWright classic router)
  → `findSourceSinkLocations()` → router1 or router2 → `routeVcc()` as post-router
  fill → `fixupRouting()` → final `timing_analysis` (router2 only).
- **`routeVcc`** (`arch.cc:912`): per-sink uphill BFS to the VCC/GND backbone,
  bounded (50k iters), bridges real bridge-pips, emits them to FASM; GND holdouts
  recorded to a file for LUT1(INIT=0) fallback (`NEXTPNR_GND_HOLDOUT_FILE`).
- **`gtClockTemplateRoute`** (`arch.cc:1018`): GT-clock→BUFG template bodge for
  xc7vx485t (`NEXTPNR_GT_CLK_BODGE=1`).
- **`setup_pip_blacklist`** (`arch.cc:351`): blacklists undocumented clock plumbing
  (`HCLK_CLB`, `CLK_FEED*`, `HCLK_FEEDTHRU*`), LIOI IDELAY/I2GCLK detours, GT pips;
  `NEXTPNR_PIP_BLACKLIST[_TILE]` env files (@424, @570).
- **`applyFixedRoutes` / `writeFixedRoutes`** (@1134/@1726): frozen hard-macro
  routing lock/unlock (`--fixed-routes`, `--write-fixed-routes` in `main.cc`).
- **router2** (`common/router2.cc`, commit `df858b1e`): **wire reservation made
  conflict-aware** — `reserved_net`/`reserved_locked`/`RESERVED_CONTESTED` sentinel
  (@79–100), locked-corridor fixpoint (@236, @332, @442–492). Arch config:
  `bb_margin_{x,y}=4`, `backwards_max_iter=200`, `perf_profile=true`
  (`arch.cc:2141–2145`).
- SRCC clock-buffer route BFS cap raised (`f7049aee`); `--route-clock-only` for the
  hybrid route-only flow.

---

## 8. Global infrastructure (likely absent upstream)

- **`--report <file>`** JSON (timing + utilization, mainline schema) — `common/timing.cc:833`.
- **CLI surface** (`xilinx/main.cc`): `--chipdb`, `--xdc` (repeatable),
  `--fasm`, `--fixed-routes`, `--write-fixed-routes`; plus inherited `--json`,
  `--write`, `--freq`, `--report`, `--router`, `--placer`, `--seed`.
- **Verbose XDC diagnostics**: `--verbose` itemises unmatched constraint targets
  and not-in-design ports (`xdc.cc:129–132`, `303–306`).
- **Error-message quality**: numerous targeted messages (e.g. `OBUFDS` swapped diff
  pins names the wrong pin `0ebf6394`; "port a of type PAD has no IOSTANDARD
  property"; range-checked `CLKFBOUT_MULT_F` `0e85a878`; BEL-attr-unknown-tile
  non-fatal `8399469c`).
- **Chipdb tooling**: `bbaexport.py` + `bbasm` + `chipdb.hexpat` (ImHex) + java
  `bbaexport`/`json2dcp` (RapidWright) + `nextpnr-xilinx-meta` submodule.
- **Examples** (`xilinx/examples/`): `arty-a35` (attosoc SoC + blinky, xc7a35t),
  `artyz7-20` (blinky, xc7z020), `attosoc` (SoC, xczu2cg), `blinky` (xczu2cg),
  `counter25` (VC707 xc7vx485t, full open-source no-Vivado flow incl. VerilogPhys
  sim + dcp2fasm + openFPGALoader), `sim/` (io_sim, ramd64e_sim), `zcu104`
  (xczu7ev blinky).
- **CI** (`.github/workflows/demos.yml`): per-PR demo regression gate — builds this
  PR's nextpnr + prjxray-db, builds chipdb for 3 parts (spartan7 `xc7s50csga324-1`,
  artix7 `xc7a35tcsg324-1`, kintex7 `xc7k325tffg676-1`), builds demo-projects
  (litex-ddr-arty-s7, litex-minimal-arty-s7, blinky-digilent-arty, blinky-qmtech,
  litex-ddr-qmtech-kintex7), compares normalised bitstream hash against committed
  goldens, uploads `.bit` artifacts. Proves the PR's own binary is used.
- **Env knobs**: `NEXTPNR_PLACER_BETA/SPREAD_SCALE_*/PLACER_ALPHA`,
  `NEXTPNR_FRESH_REGION_MARGIN`, `NEXTPNR_BUFG_CONST_DISCONNECT`,
  `NEXTPNR_GT_CLK_BODGE`, `NEXTPNR_PIP_BLACKLIST[_TILE]`, `NEXTPNR_GND_HOLDOUT_FILE`,
  `NEXTPNR_ALLOW_CO_5FF_CONTENTION`, `NEXTPNR_SKIP_FAILED_ARCS`, `XC7_LEGACY_CARRY4_SPLIT`.

---

## 9. Feature commit digest (`0.9.2..HEAD`)

114 commits (77 non-merge). Grouped by area; `#NNN` are PR merge refs.

### Carry / ALU chains (the single largest theme)
`c2c05095` relocate carry-O fabric fanout (split + sum dup) · `8d4b732d` carry-O
relocation round 2 (spine link + chain row spread) · `4a3d7e12` heap radius-growth
fail-fast · `31025814` carry-split livelock fix (spine-link steal + rowless lanes) ·
`254a50ab` heap deterministic fallback · `ecf5edd8` assign carry lane rows by netlist
ownership · `b03d0c98` carry splits keep global bit indices, S=VCC fill ·
`f3c8f84d` drop unsatisfiable chain-spread row pin · `b80c92a1` release relocated
sum-FFs · `70949581` guard carry sum-FF co-location against control-set mismatch ·
`35c78e03` realize relocated carry pass-throughs · `0b914578` configure placed BUFR +
BUFR_DIVIDE.

### Placement legaliser / HeAP
`fedc910d` heap-legalise-validity · `8d53d6d8`/`9c9b0a0d` heap-refine-validity ·
`0e24e3ff` propagate placer1_refine failure · `369038ed` per-cell legaliser timeout ·
`80a44148` eviction as last resort · `347583c3` deterministic fallback for sparse
site types · `bf78fccf` restore real slice validation for placer-constrained tiles ·
`9b40f352`/`bd4cf5c6` name chain-continuation/egress/5FF-feed conditions ·
`05aaa06b` slice-egress-validity · `b888dfd6` flat-reject cross-position carry→FF ·
`f0f2c817` budget per-position site exit.

### SRL / distributed RAM
`652d85f5` place SRL cascades like carry chains · `697e293b` route longer chains via Q ·
`c0194daf` RAM128X1S scalar A0..A6 · `9c9b0a0d`/`b390e9c9` RAM256X1S muxf zoffset ·
`363c055d` RAM256X1S mux tree in own slice half.

### BRAM
`d5b2c610`/`f1c77134` SDP opposite-port width · `11f9b694` SDP width conflict ·
`748bd6c6`/`e71acda2` ZINV_REGCLK* for registered outputs · `1b7d51b9` 36-wide width
marker collision.

### IO / IOLOGIC
`9a6a7e3b` IDDR SAME_EDGE_PIPELINED · `f77907ac` IDDR routethru SRTYPE conflict ·
`16accf3b` ILOGIC comb pass-through SRTYPE · `c05f0d05` INV_OCLK + OSERDES
TRISTATE_WIDTH.W4 · `b9ed05a2` OSERDESE2 IS_CLKDIV_INVERTED · `f2412ff3`/`0e85a878`
IOL dyn inv en · `2ae8c3d1` IOL OCLK tristate · `f1ffe695`/`55c3bc87` IBUFGDS≡IBUFDS ·
`0ebf6394` OBUFDS swapped-pin diagnostic · `d455ae52` init all four IFF flops ·
`06769c05` IDELAYCTRL no-delay → warning · `53793193` artix7 ilogic DB bump.

### Clocking
`09db508a` stop holding MMCM reset (reverted `b608fd2c`) · `e33b5f1a` PLLE2
LKTABLE/TABLE from PLL tables · `74357a79`/`442f4496` PLLE2 lock filter tables ·
`0e85a878` CLKFBOUT_MULT_F range check · `a70ae4a8` pin PLL/MMCM BUFGs to bottom
region · `ce9e6a96` preplace fabric-driven BUFGs · `f7049aee` SRCC clock-buffer BFS
cap · `a17f9415` SRCC clock buffer route.

### FASM
`7037c948` run-identity header · `d6b7f64d` skip RIOB18 Y0 IBUF_HP_BANK_GLUE ·
`e4a261ce` skip HP IN_ONLY · `70a5952c`/`c2e50b99` skip HP SLEW.SLOW cross-site/diff ·
`6f33adf0` SSTL15/SSTL135 SLEW.SLOW · `f8e76430`/`a92eb3d1` prjxray DB bumps
(RIOB18/HP-bank segbits).

### XDC
`3da43687` warn on unmatched target · `7d9ea304`/`555d326c` verbose-only not-in-design ·
`8399469c`/`0d8b1c2b` BEL-attr-unknown-tile non-fatal · `b257be4d` name[0] de-busing ·
`813bb715`/`c8c4064f` set_multicycle_path −setup.

### Mux
`31c8ea77` LUT6_2→MUXF7/8/9 use-after-free · `bfdeaf7c` BEL-constrained LUT6_2
both-outputs-only · `7d649e0b` self-documenting LUT6_2 split bools.

### Timing / routing / report
`7ea51730`/`ecb67bd7` final timing after router2 · `f6e06896` ignore comb loops ·
`df858b1e` router2 wire-reservation conflict-aware · `3266cf4d`/`01176b73` `--report`.

### CFG / infrastructure
`d42d6c9b`/`b086846e` preplace single-site CFG primitives · `8b836d66` NLnet badge ·
`8a3c29df`/`98a08d9d` submodule bumps.

### CI
`db24a3b4` per-PR demos gate · `5f864853`/`89b4fd89` assert PR binary used ·
`eacee2d2`/`5373a6e8`/`98eee3e0` ci-tests devshell + Python · `c4ab65b4` upload
bitstreams · `93b42b54` litex-minimal-arty-s7 · `e09d25c4` PR-CI scope trim.

---

## Appendix A — top-20 fork features most likely missing upstream

1. CARRY4 atomic packer + carry-O fabric-fanout relocation (`pack_carry_xc7.cc:120/1052`).
2. `xc7_logic_tile_valid` legality engine (OUTMUX budget, carry→FF rejection, control-set guards) — `arch_place.cc:325`.
3. FASM HP-bank glue/SLEW/DRIVE logic + phantom-BUFGCTRL guard — `fasm.cc:122/1082`.
4. Run-identity FASM header — `fasm.cc:5272`.
5. BUFR_DIVIDE + placed-BUFR configuration — `fasm.cc:2504`.
6. PLL/MMCM LKTABLE/TABLE + CLKFBOUT_MULT_F range check — `pack_clocking_xc7.cc:132`.
7. set_multicycle_path −setup in XDC + timing engine — `xdc.cc:245`, `common/timing.cc:390`.
8. `--report` mainline-schema JSON — `common/timing.cc:833`.
9. Final timing analysis after router2 — `arch.cc:2165`.
10. router2 wire-reservation conflict-awareness — `common/router2.cc:79`.
11. SRL cascade placement (like carry chains) — `pack.cc:763`, `arch_place.cc`.
12. RAM128X1S/RAM256X1S/SDP-BRAM width fixes — `pack_dram.cc`, `pack.cc:1185`.
13. IDDR SAME_EDGE_PIPELINED + ISERDES/OSERDES master-slave — `pack_io_xc7.cc:772/903`.
14. IBUFGDS≡IBUFDS + OBUFDS swapped-pin diagnostic — `pack_io_xc7.cc`.
15. GT (GTPE2/GTXE2) packing + IBUFDS_GTE2 site constraints — `pack_gt_xc7.cc`.
16. Single-site CFG primitive preplacement (BSCAN/STARTUP/ICAP/…) — `pack_io_xc7.cc:1232`.
17. Fabric-driven BUFG preplacement + bottom-region pinning — `pack_clocking_xc7.cc:230`.
18. `routeVcc`/`routeClock` dedicated VCC/GND + clock-backbone routing — `arch.cc:912/1752`.
19. `setup_pip_blacklist` undocumented-pip blocking — `arch.cc:351`.
20. Fixed-routes (frozen hard-macro) lock/unlock + GT-clock template — `arch.cc:1018/1134/1726`.
