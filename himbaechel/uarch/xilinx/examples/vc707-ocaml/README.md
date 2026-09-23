# VC707 OCaml bytecode processor SoC (xc7vx485tffg1761-2)

A processor whose native instruction set is **OCaml 4.14 bytecode** — no
interpreter, the bytecode is what the silicon executes — with a 1G Ethernet
MAC, a LiteEth SGMII PCS/PMA over a GTX, and a UART console. Its memories
are block RAM: a 32K-word code ROM, a 32K-word heap with a Cheney copying
collector, and 8K of globals.

Like the LiteX SoCs here it ships a **netlist**, not Verilog:
`vc707_ethmin_vm.json.gz` is yosys' output, gzipped (35 MB raw, 2.2 MB
here). The sources live in xc7-bitstream-tools as `examples/vc707-ocaml`,
which builds this design from scratch and proves the bitstream's extracted
netlist equal to the synthesis it came from; this entry asks the narrower
question a gate should ask — what nextpnr does with the netlist.

The netlist carries its own software. yosys resolves the loader's
`$readmemh` at synthesis and writes the words out as RAMB `INIT`
parameters: 18492 initialised words, a complete netboot loader that does
DHCP, ARP and TFTP in OCaml and then runs whatever image it fetched. So
the netlist alone is a bootable SoC, and rebuilding its bitstream needs no
OCaml toolchain.

## What it covers

Some 14,300 cells, 168 RAMB36s and 20 DSP48E1s, which is large enough for
placement and routing to stop being easy, but it is not merely another big
SoC: it reaches paths the LiteX designs do not.

- **A double-precision FPU.** Berkeley HardFloat's cores behind the
  processor's trap port, with 20 DSP48E1s and a divider whose sequential
  loop is the longest path in the design.
- **Block RAM with contents, at width 1 and 9.** The processor's memories
  are read a word at a time and a byte at a time, which is the `x1`/`x9`
  encoding that was wrong in the RAMB36 width bits (openXC7/nextpnr#12).
- **Two inputs sharing one HP I/O tile.** CPU_RESET and a push button are
  the two halves of `LIOB18_X81Y128`; the enable bits they share are what
  the writer got wrong until both halves could receive.
- **An MMCM, a GTX and a hand-placed clock tree,** with the processor's
  clock and the Ethernet's 125 MHz domains crossing in the packet DMA.
- **Hold-fix territory.** The packet RAM's write data arrives faster than
  the block RAM's hold time; this design is where the detour and
  feedthrough machinery earns its keep, and where a detour through an
  occupied LUT once corrupted the DMA silently.

It is `stretch` tier — ungoldened and non-blocking — because a design this
size has no placement worth freezing a hash on. The question is whether it
comes out, closes timing and stays correct.

## The clock

The netlist asks for **50 MHz** on `clk_sys`, which is what this design
closes through this flow once the FPU is in it. The sweep, same design,
same router:

    100 MHz   Vivado closes it (WNS +0.559); the open flow reaches 71.8
     75 MHz   61.0 MHz achieved   fails
     62.5 MHz 59.5 MHz achieved   fails, and leaves two hold violations
     50 MHz   62.2 MHz achieved   passes, no hold violations   <- shipped

Without the FPU the same design reaches about 72–77 MHz through the open
flow, varying with placement — which is why 75 MHz, measured on an earlier
netlist and tried first here, was too optimistic: the achieved figure moves
several MHz between runs of the same sources, so a frequency that passes
once is not one to ship. Vivado closes the FPU design at 100 MHz, so the
gap between the two flows is a standing measure of what the open flow
leaves on the table.

`clk_sys` is derived from the MMCM in the netlist, not from the XDC, so
changing the frequency means resynthesising with a different `SYS_DIV`.

## Provenance

https://github.com/jrrk2/bytecode — `ocaml4142_vm_rtl.sv` and
`fpga/vc707-ethmin`. The netlist was built with the pinned yosys from
xc7-bitstream-tools, from `examples/vc707-ocaml` in that repository.
