# VC707 LiteX Linux SoC, VexRiscv MMU (xc7vx485tffg1761-2)

A Linux-capable LiteX SoC: VexRiscv in its `linux` variant -- the rv32ima
core with an MMU -- with DDR3, SGMII Ethernet and a 128 KiB BIOS ROM.

Unlike every other example here this one ships a **netlist**, not Verilog.
`xilinx_vc707.json.gz` is yosys' output, gzipped (29.2 MB raw, 1.9 MB here).

## Why a netlist and not sources

This is the nextpnr repository, and the question a gate entry should answer
is what nextpnr does with a given netlist.  Generating this SoC from source
would put a LiteX version, a migen version, a yosys version and a RISC-V
toolchain between a code change and a result, none of which this repository
controls -- and every one of which could move a golden on its own.

The netlist also carries the software.  yosys resolves the BIOS ROM's
`$readmemh` at synthesis time and writes the words out as RAMB `INIT`
parameters: `rom.0.0` through `rom.0.3`, 128 `INIT` words each, 10439
initialised words in total.  So the netlist alone is a complete, bootable
SoC, and rebuilding its bitstream needs no compiler at all.

## What it covers

The only design in the gate at the scale where placement and routing stop
being easy.  Large designs fail in ways small ones cannot reach: router
congestion floors, placer timeouts, an int32 overflow in the HeAP cell
budget, and cell shapes no small design instantiates.  It is `stretch`
tier -- ungoldened and non-blocking -- because a design this size does not
have placement worth freezing a hash on; the question is whether it comes
out at all.

## Known good on hardware

The bitstream built from this netlist was flashed to a VC707 and observed:

    BIOS CRC passed (88d23f32)
    CPU:      VexRiscv_Linux @ 100MHz
    SDRAM:    512.0MiB 32-bit @ 800MT/s (CL-6 CWL-5)
    Memtest OK          write 62.1MiB/s, read 64.3MiB/s
    Ethernet init...    Local IP 192.168.1.50
    Booting from network... 24158216 bytes transferred over TFTP

DDR3 calibration converges on all four modules -- write levelling, DQ-DQS
training, read levelling -- which exercises the memory PHY's timing along
with the Ethernet path and the ROM.

That provenance is the point of choosing this netlist over one that merely
elaborates.  A SoC with a zeroed ROM, a dead clock or a miscalibrated memory
PHY places, routes, assembles and configures the FPGA with DONE asserted,
and is distinguishable from a working one only on a board.

The board resets rather than reaching userspace, because the payload served
to it over TFTP is an RV64 image -- it contains `ADDIW`, which is RV64I only
-- while this core is RV32.  That is a payload mismatch on the bench and
says nothing about the gateware.

## Regenerating

    vc707_litex.py --with-led-chaser --cpu-type vexriscv --cpu-variant linux \
                   --with-ddr --with-ethmin-phy --flow openXC7 \
                   --no-compile-gateware --build

`--no-compile-gateware`, not `--no-compile`: the latter disables *software*
compilation too, leaving the ROM full of zeroes and the SoC silent.
