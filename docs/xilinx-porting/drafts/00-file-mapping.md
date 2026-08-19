# Draft: cross-repo file mapping (fork xilinx/ ↔ himbaechel uarch/xilinx)

*Prepared by the coordinator (first-hand, from directory listings and line
counts; will be merged into doc 05-porting-plan.md). Line counts are
`wc -l` as of 2026-08-19.*

## Size summary

| | Lines |
|---|---|
| fork `nextpnr-xilinx/xilinx/` (all .cc/.h) | ~21.1k |
| himbaechel `uarch/xilinx/` (all .cc/.h) | ~10.2k |
| himbaechel framework (`himbaechel/*.cc|h`) | ~3.4k |

The fork carries roughly twice the xc7-specific code; part of that is the
fork's own chipdb/device-model code (replaced by the himbaechel framework +
meta JSON upstream) and its own placement work (arch_place.cc, 1860 lines).

## File-by-file mapping

| fork `xilinx/` | lines | himbaechel equivalent | lines | Notes |
|---|---|---|---|---|
| arch.cc | 2615 | himbaechel/arch.cc + chipdb.h + xilinx.cc | 662+249+820 | fork arch.cc is device/chipdb/bel/wire model + timing + router2 glue; upstream splits into framework (chipdb) + uarch glue |
| arch.h | 1713 | himbaechel/arch.h + xilinx.h + archdefs.h | 920+195+150 | |
| arch_place.cc | 1860 | xilinx_place.cc | 640 | fork has heavy xc7-specific placement/legalisation; upstream delegates to common/place + small fixup |
| pack.cc | 1598 | pack.cc | 849 | |
| pack_carry_xc7.cc | 1445 | pack_carry.cc | 403 | fork carry handling is far more elaborate (relocation/split, fanout duplication) |
| pack_io_xc7.cc | 1271 | pack_io.cc | 996 | |
| pack_io_xcup.cc | 817 | — (no xcup: himbaechel xilinx uarch is xc7-only, see 00e) | | UltraScale+ RapidWright flow in fork, missing upstream |
| pins.cc | 737 | pins.cc | 480 | |
| pack_dram.cc | 597 | pack_dram.cc | 527 | |
| pack_carry_xcup.cc | 385 | — | | |
| pack_gt_xc7.cc | 380 | — (no dedicated packer; meta has GTPE2/GTXE2 site types) | | GT packing in fork vs none apparent upstream |
| pack_clocking_xc7.cc | 337 | pack_clocking.cc + mmcm_tables.cc | 536+144 | |
| cells.cc | 331 | cells.cc | 197 | |
| xdc.cc | 309 | xdc.cc | 270 | |
| pack.h | 269 | pack.h | 221 | |
| archdefs.h | 255 | extra_data.h + archdefs.h | 107+150 | |
| pack_clocking_xcup.cc | 243 | — | | |
| pack_dsp_xc7.cc | 168 | pack_dsp_xc7.cc | 197 | |
| pack_dsp_xcup.cc | 133 | — | | |
| main.cc | 102 | himbaechel/main.cc + uarch main.cc | 103+103 | |
| fasm.cc | 5292 | fasm.cc | 1864 | fork FASM writer is 2.8x larger — most fork bugfixes live here |
| chipdb.hexpat | — | meta/*.json (per-family) | | fork: binary chipdb + hexpat; upstream: JSON meta + RapidWright-generated chipdb |

## Open questions for the final docs

- What the fork's `common/` modifications map to in upstream
  `common/place` + `common/route` (subagent 3 covers this).
- Whether himbaechel `uarch/xilinx` has any GT packing at all
  (subagent 1 covers this).
