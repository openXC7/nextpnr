# VC707 block RAM with byte enables (xc7vx485tffg1761-2)

A 1024 x 64 block RAM with per-byte write enables and a registered read
port, explicitly marked `ram_style = "block"`.

## What it covers

The `RAMB*E1` write path that every LiteX design depends on, which no small
design in the gate reaches.  Three specific things:

* **Byte write-enables.**  A 64-bit word is two 36Kb primitives wide, so
  the eight byte lanes are split across them.  A lane-mapping error -- the
  failure mode of a memory lowering that ORs the write-enables together
  instead of routing them per lane -- shows up as a wrong read rather than
  as a build failure.
* **`ram_style = "block"`.**  This attribute is the mechanism LiteX builds
  use to keep wide buffers out of distributed RAM.  Getting it wrong is not
  merely an area regression: on a large design it is the difference between
  a router that converges and one that does not.
* **A registered read port**, so the output flop is packed into the BRAM
  site rather than the fabric.
