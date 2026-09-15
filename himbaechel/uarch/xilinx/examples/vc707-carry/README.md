# VC707 carry-chain fabric taps (xc7vx485tffg1761-2)

Eight explicit `CARRY4` instances in which each position's sum **and**
that same position's carry-out both feed logic outside the slice.

## What it covers

A 7-series subslice has two independent fabric exits:

* the **direct pin** (`A`/`B`/`C`/`D`) -- hardwired, carrying either `O6` or
  the carry XOR.  prjxray has no config bit for it because there is nothing
  to configure.
* the **`xMUX`** -- configurable, via `OUTMUX.{O5,O6,XOR,CY,F7,F8,x5Q}`.

Because they are separate, a position may route its sum out of one and its
carry-out out of the other.  A packer that accounts for only the mux will
charge both to it and reject the combination.  Vivado emits this shape 121
times in 1348 carry bits of a design that runs on hardware, so it is legal.

No demo project reaches this: their carry chains are adders whose carry-out
is consumed by the next position internally, never tapped to fabric.

`CARRY4` is instantiated explicitly rather than inferred so the case is
guaranteed, and the design is self-contained -- an LFSR drives it and
everything XOR-reduces to one LED, so it needs three pins.

## Current status: a recorded routing failure

This design does not currently place-and-route.  It is in the gate with a
recorded known failure (`.github/references/vc707-carry.pnr-known-failure`)
rather than left out, because the failure is the defect it exists to find.

Observed, at two different sizes:

    N = 24: ERROR: Failed to route arc 9.0 of net 'lfsr[5]',
            from X69Y210/SLICE_X0Y0.B5FF_Q to X53Y210...
    N =  8: ERROR: Failed to route arc 8.0 of net 'lfsr[0]',
            from X79Y227/SLICE_X0Y0.B5FF_Q to X76Y227/SLICE_X0Y...

Both are the same shape: a net whose driver is an `x5FF` output.  Shrinking
the design from 24 chains to 8 changed which net failed and nothing else,
so this is not congestion.

The likely reading -- stated as a hypothesis, not a conclusion -- is that
the subslice has *three* candidates for its two fabric exits once a carry
is packed with a `5FF`:

| signal      | exit it can use                     |
|-------------|-------------------------------------|
| `O6`/carry XOR | the hardwired direct pin (`A`..`D`) |
| carry-out `CO` | the `xMUX` (`OUTMUX.CY`)            |
| `x5FF` `Q`     | the `xMUX` (`OUTMUX.x5Q`)           |

The last two contend for the same mux.  A legality check that separates the
direct pin from the mux -- correctly, they are separate -- but does not
also count the `5FF` output as a mux claimant will accept a packing that
then has no way out, and the failure surfaces in the router rather than in
the packer, a long way from its cause.

If this design starts routing, the known-failure file must be deleted and a
golden recorded in its place; the gate enforces that, so the fix cannot land
silently.
