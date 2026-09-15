# VC707 DSP48E1 multiply-accumulate (xc7vx485tffg1761-2)

One `DSP48E1` in a registered multiply-accumulate configuration
(`AREG`/`BREG`/`CREG`/`MREG`/`PREG` all 1), driven by an LFSR.

## What it covers

The `DSP48E1` configuration writer, which nothing else in the gate
reaches: the blinky designs have no multiplier, and the LiteX SoCs here are
built without one.

The fields worth exercising are the register enables and the
`OPMODE`/`ALUMODE` encodings.  Two DSP configurations can be functionally
equivalent and still differ bit-for-bit in the FASM -- which means a
difference in this design's frames needs reading before it is believed,
and equally that a *missing* difference is meaningful.  `USE_DPORT` is
`FALSE` and the pre-adder is left off, so this covers the common
multiply-accumulate shape rather than every DSP mode.
