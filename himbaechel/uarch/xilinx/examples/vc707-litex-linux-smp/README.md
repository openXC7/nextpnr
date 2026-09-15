# VC707 LiteX Linux SoC, VexRiscv-SMP (xc7vx485tffg1761-2)

The second Linux-capable LiteX SoC in the gate, and the one that differs
from `vc707-litex-linux` in the parts that are hardest to get right:
VexRiscv-SMP with a RISC-V **CLINT and PLIC**, DDR3, SGMII Ethernet and an
SD card controller.

Like its sibling it ships a **netlist** rather than sources, for the reasons
given in that example's README -- this is the nextpnr repository, and
everything upstream of the netlist belongs to someone else's CI.

## Why both, when one LiteX Linux SoC is already here

They are not the same design with a different core.  The single-core variant
has no CLINT and no PLIC at all: its entire peripheral set is a LiteX timer
and a UART, and its machine-mode layer is LiteX's `emulator`.  This one has
`clint@f0000000` and `plic@f0c00000`, runs OpenSBI, and takes its interrupts
through a standard PLIC.

For place-and-route that means two genuinely different shapes: a different
clock structure, a different interrupt fabric, and roughly 20% more logic.
A change that breaks one and not the other is a change worth looking at.

## Known good on hardware

The bitstream built from this netlist boots 32-bit Linux 6.9 to a Buildroot
login prompt with its root filesystem mounted over NFS:

    CPU:       VexRiscv-SMP, 32-bit RISC-V
    OpenSBI:   litex-hub 1.3.1-linux-on-litex-vexriscv @ 0x40780000
    root:      192.168.1.106:/home/jonathan/vc707-nfsroot  (nfs vers=3,tcp)

Getting there needs three things paired correctly, none of which is implied
by the netlist: OpenSBI must be the litex-hub fork (upstream OpenSBI's
generic platform is fine here, since the CLINT exists, but the addresses
differ), the device tree must be the one matching this SoC's CSR map, and
the kernel must be built with `CONFIG_ROOT_NFS` -- most of the rv32 kernels
in circulation here are not.

## MAC address

This SoC answers to **10:e2:d5:53:4d:50** -- "SMP" spelled in the low three
bytes -- rather than the `00:00:07` its predecessor uses.  The MAC is
compiled into the gateware and names the TFTP directory the BIOS boots
from, so two SoCs sharing one silently share a payload directory, and
staging either overwrites the other.  That is not a theoretical hazard: an
rv64 payload served to an rv32 core resets on an illegal instruction, and a
payload whose device tree describes another SoC's CSR map hangs with no
output at all.  See `mac_for_cpu()` in the generator.

## The BIOS in this netlist

The netlist carries the SoC's BIOS in its ROM, so which BIOS was built is
part of what the netlist is -- and the one this entry first shipped hangs the
board.

`sdcard_wait_cmd_done()` and `sdcard_wait_data_done()` waited on the SD
core's "done" bit with `for(;;)`, and the only timeout they could report was
the core's own timeout bit.  A core that sees no bus activity sets neither,
so an SD card that does not respond hangs the BIOS outright.  Since sdcard
boot runs at priority 30 and network boot at 50, that hang arrives twenty
levels before the board would have netbooted, and the only way past it is to
interrupt the BIOS by hand.

This netlist is from a build whose BIOS bounds both waits, so an
unresponsive card costs one timeout and the boot continues.  Confirmed on
hardware: the same design, rebuilt, netboots unattended to a Linux login
with root over NFS, where the previous one sat in `sdcard_init`.

Note that the hang is a BIOS robustness bug, independent of *why* a card
fails to answer.  On this board the known cause is the card itself -- a
full-size SDHC that fails identification where a micro-SDHC in the same slot
enumerates immediately (`docs/sd-card-findings.md` in xc7-bitstream-tools).
The open flow's I/O was investigated at length and exonerated: the `IN_DIFF`
input-buffer bits, once suspected, were tested across three builds with no
effect.

## A caveat if you build a bitstream from this

This netlist was regenerated so that its MAC would be the SMP one, and the
CSR map moved in the process: `clint` sits at `0xf0010000` here against
`0xf0000000` in the September build the `smpsd-*.dtb` files were written
for.  A device tree is only valid for the SoC it was generated from, so
booting a bitstream built from this netlist needs a dtb regenerated from
*its* `csr.json` -- the existing ones will produce a kernel that hangs with
no output, which looks exactly like a dead clock.  For the gate itself this
does not matter: nothing here boots the design, it is placed and routed.
