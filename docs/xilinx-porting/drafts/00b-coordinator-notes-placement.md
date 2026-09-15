# Draft: coordinator first-hand notes on himbaechel xilinx placement legality

*Written by the coordinator from a direct read of
`himbaechel/uarch/xilinx/xilinx_place.cc` (640 lines, all read). Merges
into doc 03 (algorithmic diff). Subagent 3 should be cross-checked
against this.*

## What upstream xilinx_place.cc already implements

1. **`xc7_logic_tile_valid(tile_type, LogicTileStatus)` (L43–353)** — full
   SLICE-tile legality, evaluated per "eight" (4 LUT/FF columns) and per
   half-tile:
   - SLICEM/SLICEL: memory & SRL LUTs only in SLICEM; SRL only in bottom
     half (`i < 4`).
   - LUT5+LUT6 coexistence: no memory/SRL mismatch, shared-input counting
     (`input_count + input_count > 5` → need shared inputs).
   - X-input single-net constraint: F7MUX select (eights A/C/E/G),
     F8MUX select (eights B/F), CARRY4 x_sigs[i%4], and indirect FF1/FF2 D
     inputs must all agree on one X net.
   - Memory top-address-bit collisions (i==2 → A5, i==1 → A6 of top LUT).
   - Mux output usage exclusivity: O5/F7F8/carry-out/FF2 may not all
     compete for the single mux output (`mux_output_used` logic).
   - Per-half-tile FF control-set matching: same CLK/SR/CE (+ their
     inversion bits, latch bit, sync bit) across all 8 FFs; wclk==clk
     constraint for memory LUTs.
2. **`isBelLocationValid()` (L355–387)** — dispatches to the above for
   logic tiles; BRAM tiles: one-hot RAMB36/RAMBFIFO36/FIFO36 and
   RAMB18/RAMBFIFO18/FIFO18, 18-vs-36 exclusivity.
3. **`fixup_placement()` (L389–510)** — post-placement legalisation:
   - Re-permutes LUT5/LUT6 inputs onto A1..A6 avoiding overlap, recording
     the logical permutation in `X_ORIG_PORT_A<i>` attributes (RapidWright/
     Vivado interop).
   - Memory/SRL LUTs: tie A6 to VCC.
   - PS7: tie unused inputs to constants.
4. **`fixup_routing()` (L512–638)** — post-routing:
   - LUT permutation pips → physical-to-logical mapping via X_ORIG_PORT.
   - OSERDESE3 T_OUT absent → OSERDES_T_BYPASS=TRUE.

## What is visibly ABSENT upstream (to verify against fork)

- **No carry-chain-level legality**: CARRY4 appears only as per-tile
  `carry.x_sigs`/`carry.out_sigs` tags; there is no chain
  continuation/spine/lane logic like the fork's `pack_carry_xc7.cc` +
  `arch_place.cc` (carry-O relocation, split, lane ownership,
  cross-position pairing rejection).
- **No SRL cascade placement rules** (fork: SRL cascades placed like
  carry chains).
- **No IO/IOB pairing legality** (fork: RIOB18/IOB18 pairing, diff-pair
  checks, SLEW group emission rules — though some of this may live in the
  fork's fasm.cc rather than placement).
- **No global-buffer placement rules** (fork: fabric-driven BUFG
  preplacement, BUFG pinning to bottom region, SRCC clock pin BFS).
- **No IDELAYCTRL/IDELAYE2 grouping checks** visible here (fork: IDELAYCTRL
  with no delays is a warning; grouping logic in pack_io).
- **No 5-FF-feed / egress budgeting / cross-position carry→FF rejection**
  (these are named conditions in the fork's recent commits, e.g.
  b888dfd6, f0f2c817, bd4cf5c6).

## Interpretation (for doc 03)

Upstream encodes the *static* slice-level legality well (probably better
structured than the fork's), but lacks the *chain/spanning* legality that
the fork accumulated: carry chains across slices/tiles, SRL cascades, IO
bank pairing, clock-buffer placement. That split — static legality OK,
spanning legality missing — should structure the "must-port legality"
list. This matches the user's premise: keep upstream algorithms, port
the missing legality knowledge as checks/hooks.
