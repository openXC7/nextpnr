# VC707 four-lane kHeavyHash miner (xc7vx485tffg1761-2)

A Kaspa kHeavyHash mining core with four hashing lanes, each a 64x64 matrix
multiply on 64 DSP48E1s and a keccak pipeline, behind a LiteEth SGMII MAC
(ethmin's `framing_top_sgmii`, GTXE2 transceiver) that takes block templates
from a host daemon and returns nonces.  It is here because it is the design
that defeated the placer: the lanes are compact and identical, their shared
matrix registers were merged by synthesis into one bank, and the design
fills a tenth of the die densely enough that a wirelength-only placement
does not route.

    cells         109340
    LUTs           67607
    flip-flops     39055
    CARRY4          2336
    DSP48E1          256   (64 per lane)
    RAMB36E1          46
    MMCME2_ADV         3
    GTXE2_CHANNEL      1

Synthesised by Verilator's structural backend (jrrk2/verilator, `synthesis`
branch; `--map-only-edif-flat` names, so the cells match the Vivado reference
flow by name) from jrrk2/synthgui's `vc707_miner_legacy4` project.  The
host side (`mine_daemon`, talking to a kaspad) is not part of the gate.

## Why this one is in the gate

Measured on the same netlist, in Vivado's tile grid, with synthgui's
`scripts/placement_fom.py`:

| placement | HPWL (x doubled) | cells in tiles with RUDY demand > 300 | overflow above 250, per cell | router2 |
|---|---|---|---|---|
| Vivado 2020.1 | 3.37M | 4.9 % | 6.5 | zero overuse in 84-155 passes, 35-39 min |
| HeAP, no flags | 3.55M | 8.0 % | 11.8 | pass 1 takes 6 h, does not converge |
| HeAP `--placer-heap-timingweight 60 --placer-heap-congestion-spread` | 4.10M | 11.2 % | 15.7 | does not converge |
| HeAP `--placer-heap-congestion-spread` only | 3.93M | 1.7 % | 2.4 | zero overuse in 9 passes, 2 h 59 total, 90.6 MHz |
| HeAP `--placer-heap-beta 0.7 --placer-heap-congestion-spread` | 3.24M | 2.2 % | 3.6 | zero overuse in 42 passes, 3 h 08 total, 97.8 MHz |

Local routing overflow at Vivado-like wirelength is what separates the
placements that route from the ones that do not; HPWL alone does not (the
no-flags placement is within 5 % of Vivado's and never finishes a pass).
The congestion-driven spread was in the tree all along; what had hidden it
was the timing weight it was always paired with, which on this design
undoes the spread.  The last row is the configuration the board runs: its
bitstream, with the MMCM retimed to 90.9 MHz, mined Kaspa testnet-10 on a
VC707 with every nonce verified in software (2026-11-03).

## What the gate checks

That P&R completes with those flags and the FASM assembles, like every
stretch entry.  The clock is 100 MHz in the XDC and the route reaches 97.8
MHz, so `--timing-allow-fail` is what lets it finish; the Fmax is printed
for the record, not gated.  Not proved by LVS in the gate (the design is too
large for the ten-minute budget per target).

Three hours of P&R on a 24-thread desktop, 8 GB resident; expect the full
six-hour budget on a four-core runner.  Anything that makes it fail to
converge again is the regression this entry exists to report.
