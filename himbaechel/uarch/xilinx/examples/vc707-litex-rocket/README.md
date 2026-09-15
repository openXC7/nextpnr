# VC707 LiteX SoC around Rocket, RV64IMAC (xc7vx485tffg1761-2)

A 64-bit RISC-V SoC: SiFive's Rocket in LiteX's `medium` configuration --
**rv64imac, no FPU** -- with DDR3, SGMII Ethernet and a 128 KiB BIOS ROM.

This is the largest design in the gate by some way:

    cells        47719      against 22396 for the VexRiscv-SMP SoC
    RAMB36E1        13
    RAMB18E1        36
    DSP48E1          4
    INIT words   27914 initialised, of 47925

## Why no FPU

`LitexConfig_medium_1_8` is the no-FPU Rocket: `medium` is rv64imac, where
`full` adds F and D.  The `1_8` is one core and a 512-bit memory port
(`--cpu-mem-width 8`), which is what removes the last width conversion
between Rocket and LiteDRAM.

Leaving the FPU out is not a simplification for the gate's benefit.  It is
the configuration that fits: the FPU variants did not close timing on this
part, and an rv64imac Linux userland is a supported target where a
soft-float rv64 one is not universally so.

## Provenance

    netlist   yosys output for vc707-build/rocket-sf-eth-75, whose sources are
              gateware/xilinx_vc707_bramstyle.v (LiteX's own Verilog with the
              RAM style annotated), examples/vc707-ethmin/rtl/liteeth_sgmii_phy.v,
              and pythondata-cpu-rocket's LitexConfig_medium_1_8
    xdc       that build's own xilinx_vc707.xdc, 149 pin constraints

Every one of the netlist's 38 top-level ports is constrained by that xdc; the
pair belongs together and neither half is interchangeable with another build's.

## What it covers

Rocket is where the placer's deficit shows most clearly -- this SoC is twice
the VexRiscv-SMP design and uses the DSPs and the deep BRAM the smaller
entries do not.  It is a stretch entry: what it answers is whether nextpnr
completes at this scale and what it costs, not whether the result runs.  No
bitstream from this netlist has been on hardware.

## Regenerating

    vc707_litex.py --cpu-type rocket --cpu-variant linux --cpu-mem-width 8 \
                   --with-ddr64 --with-ethmin-phy --flow openXC7 \
                   --no-compile-gateware --build

`--no-compile-gateware`, not `--no-compile`: the latter disables software
compilation too, leaving the ROM full of zeroes and the SoC silent.
