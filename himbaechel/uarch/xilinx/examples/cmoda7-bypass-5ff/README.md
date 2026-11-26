# Cmod A7 bypass-fed FF next to a used 5FF (xc7a35tcpg236-1)

One slice in which the main flip-flop takes its D over the bypass input
while the 5FF of the same quarter is in use:

| cell       | BEL                  | D                                              |
|------------|----------------------|------------------------------------------------|
| `ff_main`  | `SLICE_X4Y120/BFF`   | `cnt[5]`, a flip-flop in another slice, so over `BX` |
| `lut_five` | `SLICE_X4Y120/B5LUT` | inverter of `cnt[6]`                           |
| `ff_five`  | `SLICE_X4Y120/B5FF`  | `lut_five`'s `O5`                              |

The two flip-flops share the slice's clock, CE and SR, so the slice is
legal. Vivado 2025.2, placing and routing yosys's netlist with the same
placement (`vivado.xdc`), builds it without a critical warning, and its
bitstream has, in `CLBLL_L_X4Y120`:

    SLICEL_X0.BFFMUX.BX
    SLICEL_X0.B5FFMUX.IN_A
    SLICEL_X0.BOUTMUX.B5Q

Vivado also chooses this pairing by itself: 25 times in a 1000-cell design
measured on a Cmod A7.

## Current status: a recorded placement failure

nextpnr refuses the placement:

    ERROR: illegal placement in logic tile X14Y31: 'ff_main' (X14Y31/SLICE_X0Y0.BFF), 'ff_five' (X14Y31/SLICE_X0Y0.B5FF)

The check is in `xilinx_place.cc`: a main FF that takes its D from the X
bypass may not share a column with a placed 5FF, because "the router fails
to bind the main FF's input MUX (xFFMUX) from X -- it silently leaves the
main FF's D floating". So the check works around a router limitation; the
chip allows the pairing. Without the three `BEL` lines in `top.xdc`, the
design builds and the placer keeps the two flip-flops apart.

The gate records the failure in
`.github/references/cmoda7-bypass-5ff.pnr-known-failure`. If this design
starts building, the router has learnt to set `BFFMUX` from `BX` with the
5FF in use: delete that file. The design then checks itself on hardware.
`led[1]` lights, and stays lit, at the first cycle in which `ff_main` or
`ff_five` holds a wrong value, such as a `BFF` whose D was left floating.
`led[0]` blinks at about 0.7 Hz.

## Files

- `top.v`: the design. `FDRE` and `LUT1` are instantiated so the case is
  guaranteed.
- `top.xdc`: pins and the three `BEL` constraints, in nextpnr's form.
- `vivado.xdc`: the same in Vivado's form (`LOC` and `BEL`), for a Vivado
  reference build of yosys's EDIF (`link_design`, `place_design`,
  `route_design`).
