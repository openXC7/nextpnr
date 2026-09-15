# VC707 ILOGIC delayed input (xc7vx485tffg1761-2)

Coverage for the RIOI pseudo-pip that selects the delayed ILOGIC input.
Before this design, nothing in any gate reached it: no FASM produced by the
five `demos.yml` projects contains an `IDELMUXE3` line, so the XC7V branch of
that mapping in `fasm.cc` rested entirely on reading the database.

`IBUF` -> `IDELAYE2` (`IDELAY_TYPE FIXED`, value 15) -> flop, with the data
pin on `C18`, bank 38, so the pad lands in an `RIOI*` tile and takes the
`RIOI` branch of `pp_config` rather than the `LIOI3` one.

It works.  The produced FASM contains

    RIOI_TBYTETERM_X311Y299.ILOGIC_Y0.IDELMUXE3.P0

and `virtex7/segbits_rioi.db` defines `RIOI.ILOGIC_Y0.IDELMUXE3.P0`.  Had the
feature kept the name `IDELAY_Y0.IDELMUXE3.P0` that an earlier revision of
this branch gave it, this design would fail with `FasmLookupError` -- the
database has no such key for any family.  The same database has no
`ILOGIC_Y*.ZINV_D` for virtex7, which is the other half of that mapping.

## What it found

The first run of this design did not assemble, for a reason unrelated to
IDELMUXE3:

    FasmLookupError: Segment DB RIOB18, key
    RIOB18.IOB_Y0.LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN
    not found from line 'RIOB18_X312Y299.IOB_Y0....STEPDOWN'

`segbits_riob18.db` contains no `STEPDOWN` key at all, in virtex7 or
kintex7, and a reference bitstream for an `LVCMOS18` input on bank 38 sets
none -- so the omission is real and emitting it was the bug.  The stepdown
block admitted any input-only single-ended HP pad, left or right; it is now
restricted to `LIOB18`, where the feature exists and the legacy flow uses
it.

The same comparison showed the right-hand HP input was under-programmed as
well: it was missing `LVCMOS12_LVCMOS15.IN`, the
`LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW` slew, the
`LVCMOS12_LVCMOS15_SSTL12_SSTL135_SSTL15.IN_ONLY` alias, and the partner
half's slew and `PULLTYPE.PULLDOWN`.  `RIOB18` has its own vocabulary --
no `IBUF_HP_BANK_GLUE`, no `STEPDOWN`, no wide `…LVTTL_SSTL135_SSTL15`
slew alias -- so it needed its own block rather than a widened `LIOB18`
one.  That tile now matches the reference line for line.

Not hardware-verified.
