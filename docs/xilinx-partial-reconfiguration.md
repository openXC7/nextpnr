# Partial reconfiguration on Xilinx 7-series

The himbaechel `xilinx` uarch can reserve a rectangular *window* of the fabric, so that
a circuit can be built for that window on its own and loaded into a running design through
`ICAPE2`, using only open tools: nextpnr for both builds and Project X-Ray for the frames.

This has been run on a VC707 (xc7vx485t). A LiteX SoC's CPU fetched images over Ethernet and
swapped between two circuits with no JTAG involved. See [What has been measured](#what-has-been-measured)
and [What is not known](#what-is-not-known).

## The idea

Configuration memory is written in frames. If everything a circuit uses lies in frames that
nothing else uses, those frames can be rewritten while the rest of the design keeps running.
Two builds make that true:

- The **harness build** places and routes the static design with the window reserved, with a
  placeholder circuit inside to fix the routes that cross the boundary.
- The **window build** places and routes a replacement circuit on a chipdb of the window alone,
  with its ports pinned to the nodes where the harness's routes cross the boundary.

The window build's FASM replaces the harness's FASM in the window's tiles, and the changed frames
become an image for `ICAPE2`.

## Choosing a window

A window is a rectangle of the chipdb tile grid, and the hardware largely decides its shape:

- **Height:** a frame spans one clock region of a column, so the window is one clock region tall.
  That also puts the region's HCLK row inside it, so the clock leaves belong to the window. A
  window spanning more than one frame row is rejected by the image tool.
- **Width:** whole frame columns. The image tool writes from the window's first frame to the end
  of its frame row, so the window must end the row's CLB columns. Interconnect-only tail
  columns are allowed.
- **Contents:** block RAM and DSP columns inside the window come with it. Block RAM content frames
  are a separate block type and are only written if they changed.

The window used so far is clock region X1Y5 of the xc7vx485t: frame columns 138 to 153, tile grid
x 354..390, y 53..103. That is 544 frames, and an image is 220 KB.

## The harness build

```
nextpnr-himbaechel ... \
    -o pr-window=354,53,390,103 \
    -o pr-cells=at_block. \
    -o pr-interface=interface.txt
```

| Option | Meaning |
|---|---|
| `pr-window=x0,y0,x1,y1` | The rectangle, in tile grid coordinates, inclusive |
| `pr-cells=<prefix>` | Name prefix of the cells that go inside. A cluster with any cell inside goes inside whole |
| `pr-interface=<file>` | After routing, write the boundary crossings here |

Every net then falls into one of three classes, enforced through `checkPipAvailForNet` in both
routers and in `route_clocks`:

| Class | Pins | May use |
|---|---|---|
| Static | None inside | Pips outside the window only |
| Internal | All inside | Pips inside the window only |
| Crossing | Both sides | Anything |

After routing the classes are checked again on the final connectivity, and a violation is an error.

The interface file has one line per boundary node: `<net> <class> <tile>/<wire>`. The class matters:

| Class | Meaning | Usable as a pin? |
|---|---|---|
| `in`, `out` | Reached from the driver without crossing the boundary first | Yes |
| `reentry`, `reexit` | Reached through the other side first | No: once the window is replaced, nothing drives it |

Do not trust an entry just because it exists. Pinning a port to a `reentry` gave an input stuck at a
constant level on the board, while the same net through an `in` entry worked.

Because a harness synthesised flat has no hierarchy to select by, the block is synthesised alone and
merged into the harness netlist with its cell names prefixed. That is what `pr-cells` selects.

## The window chipdb

```
xilinx_gen.py --xray <prjxray-db>/artix7 --device xc7vx485t --bba win.bba \
    --window 354,53,390,103 --window-map window-map.json
```

(The generator derives the family's database path from the device, so `artix7` here is right.)

The result is a chipdb of the window plus a ring of border tiles. Every node that crosses the edge
gets a wire in a border tile with a `PARTPIN_IN` and a `PARTPIN_OUT` bel on it. Tiles keep their real
names, so the FASM comes out in the device's own terms. For the window above the chipdb is 11.5 MB
with 16357 boundary nodes, against 79 MB for the whole device.

`PARTPIN_IN` and `PARTPIN_OUT` are plain chipdb strings rather than constids, because any edit to
`constids.inc` invalidates every existing chipdb.

## The window build

The netlist is the replacement block, plus one `PARTPIN_IN` or `PARTPIN_OUT` cell per port, pinned with
a `BEL` attribute to the node the interface file gives for that port's net.

```
nextpnr-himbaechel --chipdb win.bin --json window.json ... \
    --router router2 --timing-allow-fail -o hold-fix \
    -o clock-tree=off -o delay-matrix=off \
    -o pr-reserve=reserve.txt
```

`pr-reserve` lists bels whose wires are withheld from every net: the other nodes the harness's routes
occupy in the window that no port was pinned to. A clock port is recognised by its partition pin driving
clock pins, and `route_clocks` keeps it on the dedicated tree. Without that, it gets spread over fabric
wires and every flip-flop is skewed.

## Making the image

`synthgui --tool pr-partial` does this, using three steps:

1. **Merge:** the harness FASM with its window-tile lines replaced by the window build's. A window FASM
   line outside the window is an error.
2. **Frames:** run `fasm2frames` on the merged FASM.
3. **Extract:** take the window's frames from the merged frames and wrap them in an ICAP word stream: sync,
   `RCRC`, `IDCODE`, `FAR`, `WCFG`, `FDRI`, `RCRC`, `DESYNC`. With `--ref`, every frame outside
   the window must be identical to the harness's, so an image that would touch the static design is
   rejected rather than loaded.

The merged frames also make a full bitstream, which helps separate a bad circuit from a bad ICAP write:
load the same design over JTAG and compare.

## Making ICAPE2 work

Bitstreams from this flow used to have a dead ICAP: reads returned zero, `IPROG` did nothing and frame
writes had no effect. Two things were needed.

**The `ICAP.IN_USE` bits.** A Vivado build of the same design read the IDCODE back. Comparing the configuration
centre's frames showed three bits that no prjxray-db feature covers, beside `ICAP_WIDTH`: `26_2206 26_2207
27_2205`. They are now the feature `CFG_CENTER_MID.ICAP.IN_USE`, and `fasm.cc` writes it for any `ICAPE2`.

This needs a prjxray-db with that feature, and `fasm2frames` is expected to reject the design otherwise. That only
concerns designs that instantiate an `ICAPE2`. So far it is added for `virtex7` and `artix7`, the two
families measured.

**The top site.** There are two ICAP sites, and the packer now defaults to the top one, `ICAP_X0Y1`. A `BEL`
constraint still overrides that.

Measured on a Sonata board (xc7a50t), with a small design that reads three registers through the ICAP:

| Site | `ICAP.IN_USE` | IDCODE | STAT | CTL0 |
|---|---|---|---|---|
| Top | Yes | `0362C093`, matching JTAG | `46107FFC` | `00000501`, as the bitstream writes |
| Top | No | 0 | 0 | 0 |
| Bottom | Yes | 0 | 0 | 0 |

`ICAPE2` also swaps the bits within each byte, on both `I` and `O`.

## What has been measured

All on a VC707 with the window above. The circuits are chains of LUTs between a launch flip-flop and a capture
flip-flop on a second MMCM output, with the capture phase stepped in eighths of a VCO period (104 ps at
1200 MHz). A path's error band closes when the capture edge passes its delay.

- **The swap happens.** The test block's counters read 0 before the ICAP write and 1, the reset state of the
  new circuit's counters, after it.
- **ICAP and JTAG agree.** The same merged design loaded each way gives 35 of 40 sweep points identical, and the
  other 5 are all on marginal edges of a band.
- **Swaps repeat, with no JTAG between them.** Going from A to B and back to A, A's bands repeat within one
  step, apart from the opening edge of its depth-0 path, which moved two steps.
- **The static design survives.** Ethernet and DDR kept working across three swaps at the BIOS prompt, since
  each image arrived over TFTP into DDR, and Linux boots to a shell afterwards.

Where each path's band ends, in ps from the launch clock's phase:

| Circuit A: LUT depth | 0 | 1 | 2 | 4 | 8 |
|---|---|---|---|---|---|
| Band closes by | 833 | 521 | 833 | 1562 | 2396 |

| Circuit B: LUT depth | 0 | 3 | 6 | 12 | 16 |
|---|---|---|---|---|---|
| Band closes by | 521 | 1354 | 2917 | 4062 | 5729 |

These are raw offsets including clock skew and routing, not LUT delays. Path delays also depend on each
circuit's own routing, which is why depth 0 can exceed depth 1 above.

## What is not known

- **A swap while Linux runs.** All swaps so far happened at the BIOS prompt, and the harness's crossing nets were
  idle during the write. A bus attached to a running CPU will need something to isolate it during a swap, and that
  does not exist yet.
- **Whether the stream has to be gapless.** The harness that works buffers the whole image in a 256 KB FIFO and
  releases it in one go. That was added while chasing the wrong cause, and streaming with gaps has not been tried
  since the real cause was found. The buffer costs block RAM and a lot of routing time (34 minutes against 5).
- **What the bits are.** `IN_USE` is an inferred name. What is measured is that the ICAP answers with them and not
  without them.
- **Why the bottom site reads zero.** `CTL0.ICAP_SELECT` being unset and the bits being specific to the top site have
  not been separated.
- **Register reads on the VC707.** The LiteX register core's BIOS command printed the same value for IDCODE, STAT and
  CTL0 there. The IDCODE is right, and the Sonata test shows the three registers differ, so the cause is probably in that
  core or command, but it has not been checked.
- **Kintex-7, Spartan-7 and Zynq-7 ICAP bits.** These are not measured, and those databases do not have the feature.
- **Other families' windows.** The flow has only been run on the xc7vx485t.

## Known limitations

- `hold_fix.cc` can fail in a window build with two flip-flop sinks sharing a slice's CE mux ("attempting to reserve sink
  input path wire"). Generated test blocks avoid clock enables for this reason.
- The left-column MMCM sites on the xc7vx485t (`MMCME2_ADV_X0Y*`) have no routes out of `DO`, `DRDY` or `LOCKED` in
  the chipdb, and router2 retries without end. Pin an MMCM using those outputs to a right-column site.
- A partition's ports each enter at one node. A net that fans out directly inside the block crosses at several, and the
  rest are reserved, so buffering each input once at the entry makes for a tidier interface.
- ICAP access can rewrite the whole device, not just the window. Whatever loads images should check their frame addresses.

## Tools

The nextpnr and generator options are in this tree. The rest is in
[synthgui](https://github.com/jrrk2/synthgui) and is still being committed:

| Tool | Purpose |
|---|---|
| `synthgui --tool pr-block-json` | Merge the block into the harness netlist; make the window netlist and reservation list |
| `synthgui --tool pr-partial` | Merge FASM; extract and check the image |
| `synthgui --tool at-gen`, `--tool at-decode` | Generate path-delay test blocks; decode their counters |
| `xc7/at_sweep.py` | Capture-phase sweep over the BIOS console |
| `xc7/tftp_serve.py` | Serve images to the BIOS `netload` command |
