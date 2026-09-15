# Draft: coordinator first-hand notes on cell-type ID diff (constids.inc)

*Written by the coordinator from a direct diff of
`himbaechel/uarch/xilinx/constids.inc` (724 IDs) vs
`nextpnr-xilinx/xilinx/constids.inc` (753 IDs). Merges into docs 01/02/04.
This SUPERSEDES the "no xcup in himbaechel" remark in 00-file-mapping.md.*

## Key finding (REVISED after subagent 1 + verification): upstream xilinx uarch is xc7-ONLY

The E2/E3/E4-era IDs (RAMB36E2, FIFO36E2, DSP48E2, MMCME4, PLLE4,
ISERDESE3, OSERDESE3, IDELAYE3, ODELAYE3, BUFG_PS...) ARE present in
`constids.inc`, but they are a **superset vocabulary**: the only real code
touching them is vestigial (a BRAM-type check in `xilinx.cc:235` and
invertible/tied pin tables in `pins.cc`). There are **no E2/E3 packers**.
Verified: `match_device` = `substr(0,3)=="xc7"` (`xilinx.cc:815`) and the
device regex is `xc7[azkv]...` (`xilinx.cc:81`) — **the uarch is
xc7-only**.

UltraScale upstream = the separate `ng-ultra` uarch (device `NG-ULTRA`,
Project Beyond database — a non-Xilinx UltraScale-class part), which is
NOT a Xilinx UltraScale+ path.

**Corrected conclusion: the fork's xcup (Xilinx UltraScale+ via
RapidWright) IS a missing capability upstream.** (This supersedes my
earlier statement here and in 00-file-mapping.md.)


## IDs the FORK has that upstream constids lacks (config/hard-IP area)

BSCAN/BSCANE2, BUFH, DCIRESET, DNA_PORT, EFUSE_USR, FRAME_ECC(E2),
GTREFCLK0/1, IBUFDS_GTE2, ICAP(E2), JTAG_CHAIN, OBUFDS_GTE2, OFB_USED,
OPAD, PCIE_2_1, PLL0/PLL1_* (GT PLL ids), PROG_USR, STARTUP(E2),
USR_ACCESSE2, USRCCLKO, CLK_HROW_TOP_R.

i.e. upstream xilinx uarch appears to lack packers/constids for:
JTAG/BSCAN, DNA, EFUSE, FRAME_ECC, ICAP, STARTUP/USR_ACCESSE2, PCIe,
GT reference clocking (IBUFDS_GTE2), and BUFH (has BUFHCE).

(Upstream names BUFR_BUFR/BUFIO_BUFIO/BUFHCE where the fork uses
BUFR/BUFIO/BUFHCE_BUFHCE — naming, not capability.)

## IDs UPSTREAM has that the fork lacks

Mostly ultra-scale-era: CARRYCASCIN/OUT, CLKFBOUT_MULT(_F), DIVCLK_DIVIDE,
INTENT_SITE_WIRE, IOB18M_INBUF_DCIEN, MULTSIGNIN/OUT, SHIFTIN1/2,
SHIFTOUT1/2, TBYTEIN — plus newer naming for shared things.

## Caveats

- constids presence ≠ implemented packer; needs cross-check with
  cells.cc/pack*.cc on both sides (subagents 1 & 2).
- Fork's DNA/ICAP/STARTUP/BSCAN support includes recent fixes
  (e.g. jtag_led issue dir in workspace) — porting candidates.
