# VC707 MMCM phase defaults (xc7vx485tffg1761-2)

An `MMCME2_BASE` driving three `BUFG`s, where `CLKOUT0` sets its phase
explicitly to zero, `CLKOUT1` leaves `CLKOUT1_PHASE` unset, and `CLKOUT2`
sets a non-zero phase.

## What it covers

The unset phase is the point.  UG472 gives `CLKOUT*_PHASE` a default of 0
degrees.  `write_pll` in `fasm.cc` agrees and uses

    double phase = float_or_default(ci, name + "_PHASE", 0);

but `write_mmcm_clkout` used a default of `1`, so an MMCM output whose
phase was not set explicitly received a phase shift that the design never
asked for.  Nothing in the gate caught it: every MMCM in every demo
project writes all of its phases explicitly, so the default is never
consulted.

Because one output here deliberately does not write its phase, a change to
that default moves this design's frames and no other entry's -- which is
what makes it a regression test rather than a coincidence.

It also gives the clocking column a second shape to check: three bound
`BUFG`s fed from one MMCM, as opposed to `vc707-multibufg`'s three fed
directly from a pad.
